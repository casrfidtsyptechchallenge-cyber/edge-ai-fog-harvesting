"""The arc predictor (M5a): a small 1D-CNN on a window of collector current, trained here, run in 8 bits on the
MCX (firmware/node/lib/edge_ai/arc_predictor.cpp).

    window (256 samples, µA) -> z = (x - median) / max(median, 2 µA), clipped to ±1          (preprocess)
    conv 1 -> 8 channels, kernel 7, stride 2, ReLU        125 steps
    conv 8 -> 16 channels, kernel 5, stride 2, ReLU        61 steps
    average and maximum over time                          32 numbers
    dense 32 -> 1, sigmoid                                 probability of an arc within 5 s

About 750 weights and 46 000 multiply-adds: a few milliseconds on the MCX.

8 bits: weights are int8 with one scale per layer; activations are int8 (input scale 1/127); each layer's
int32 sum is brought back to int8 with an integer multiplier and a shift, so the C++ and the reference
below give exactly the same numbers (tested with golden vectors). Only the final sigmoid is in float.

A rule-based fallback runs on the same window: it counts pre-discharge pulses (2 samples or more, 5 noise
sigmas above the median), measures the noise and the current; near sparkover all three grow. Their
logarithms go through a logistic regression with four numbers fitted on the same data.
The median and the noise come from the sorted window (noise = median - 16th percentile, one sigma for
Gaussian noise): the pulses only push the upper half, so they cannot hide themselves by raising the noise.
"""

import math
import time

import numpy as np

from .arc_physics import FS_HZ, WINDOW

C1, K1, S1 = 8, 7, 2
C2, K2, S2 = 16, 5, 2
L1 = (WINDOW - K1) // S1 + 1     # 125
L2 = (L1 - K2) // S2 + 1         # 61
FEATURES = 2 * C2
MIN_SCALE_UA = 2.0               # below 2 µA of current the window is scaled as if it had 2 µA
PULSE_SIGMAS = 5.0               # a pulse: at least 2 samples this many noise sigmas above the median...
PULSE_MIN_Z = 0.05               # ...and at least 5 % of it


# ---------------------------------------------------------------- the window, as the MCX prepares it

NOISE_INDEX = 40                 # sorted[40] of 256 is the 16th percentile: median - it = one noise sigma


def preprocess(x):
    """(N, WINDOW) µA -> z (N, WINDOW) float, the noise sigma in z units (N,) and the median µA (N,).
    Mirrors the C++ (float32)."""
    x = np.asarray(x, dtype=np.float32)
    s = np.sort(x, axis=1)
    half = x.shape[1] // 2
    median = ((s[:, half - 1] + s[:, half]) * np.float32(0.5))[:, None]
    noise = (median[:, 0] - s[:, NOISE_INDEX * x.shape[1] // WINDOW]).astype(np.float64)
    scale = np.maximum(median, np.float32(MIN_SCALE_UA))
    z = np.clip((x - median) / scale, -1.0, 1.0).astype(np.float64)
    return z, noise / scale[:, 0], median[:, 0].astype(np.float64)


def round_half_away(v):
    return np.sign(v) * np.floor(np.abs(v) + 0.5)


def quantize_input(z):
    return round_half_away(np.asarray(z) * 127.0).clip(-127, 127).astype(np.int64)


def pulse_rate(z, sigma, fs=FS_HZ):
    """Pre-discharge pulses per second: runs of 2 samples or more above the threshold. Mirrors the C++."""
    thr = np.maximum(PULSE_SIGMAS * sigma, PULSE_MIN_Z)[:, None]
    above = z > thr
    starts = above[:, 1:] & ~above[:, :-1]
    starts = np.concatenate([above[:, :1], starts], axis=1)
    longer = np.concatenate([above[:, 1:], np.zeros((z.shape[0], 1), bool)], axis=1)  # the next one too
    return (starts & longer).sum(axis=1) * fs / z.shape[1]


# ---------------------------------------------------------------- float network, for training

def _cols(x, k, stride):
    """(B, C, L) -> (B, Lout, C*k), channel-major then tap: what each output step sees."""
    b, c, length = x.shape
    lout = (length - k) // stride + 1
    sb, sc, sl = x.strides
    view = np.lib.stride_tricks.as_strided(x, (b, lout, c, k), (sb, sl * stride, sc, sl))
    return view.reshape(b, lout, c * k)


def _uncols(dcols, shape, k, stride):
    b, c, length = shape
    lout = dcols.shape[1]
    d = dcols.reshape(b, lout, c, k)
    dx = np.zeros(shape)
    for j in range(k):
        dx[:, :, j:j + stride * lout:stride] += d[:, :, :, j].transpose(0, 2, 1)
    return dx


class ArcNet:
    def __init__(self, seed=0):
        rng = np.random.default_rng(seed)
        self.p = {
            "w1": rng.normal(0, math.sqrt(2 / K1), (C1, 1, K1)), "b1": np.zeros(C1),
            "w2": rng.normal(0, math.sqrt(2 / (C1 * K2)), (C2, C1, K2)), "b2": np.zeros(C2),
            "w3": rng.normal(0, math.sqrt(1 / FEATURES), FEATURES), "b3": np.zeros(1),
        }

    def forward(self, z, keep=False):
        x0 = z[:, None, :]
        c1 = _cols(np.ascontiguousarray(x0), K1, S1)
        a1 = np.maximum(c1 @ self.p["w1"].reshape(C1, -1).T + self.p["b1"], 0).transpose(0, 2, 1)   # (B, C1, L1)
        c2 = _cols(np.ascontiguousarray(a1), K2, S2)
        a2 = np.maximum(c2 @ self.p["w2"].reshape(C2, -1).T + self.p["b2"], 0).transpose(0, 2, 1)   # (B, C2, L2)
        feat = np.concatenate([a2.mean(axis=2), a2.max(axis=2)], axis=1)
        logit = feat @ self.p["w3"] + self.p["b3"][0]
        if keep:
            self.cache = (x0.shape, c1, a1, c2, a2, feat)
        return logit, a1, a2

    def backward(self, dlogit):
        x0_shape, c1, a1, c2, a2, feat = self.cache
        g = {"w3": feat.T @ dlogit, "b3": np.array([dlogit.sum()])}
        dfeat = np.outer(dlogit, self.p["w3"])
        da2 = np.repeat(dfeat[:, :C2, None] / L2, L2, axis=2)
        idx = a2.argmax(axis=2)
        b_idx, c_idx = np.meshgrid(np.arange(a2.shape[0]), np.arange(C2), indexing="ij")
        da2[b_idx, c_idx, idx] += dfeat[:, C2:]
        da2 *= a2 > 0
        d2 = da2.transpose(0, 2, 1)                                    # (B, L2, C2)
        g["w2"] = np.einsum("blc,blk->ck", d2, c2).reshape(C2, C1, K2)
        g["b2"] = d2.sum(axis=(0, 1))
        da1 = _uncols(d2 @ self.p["w2"].reshape(C2, -1), a1.shape, K2, S2) * (a1 > 0)
        d1 = da1.transpose(0, 2, 1)
        g["w1"] = np.einsum("blc,blk->ck", d1, c1).reshape(C1, 1, K1)
        g["b1"] = d1.sum(axis=(0, 1))
        return g

    def predict(self, z, batch=2048):
        out = [self.forward(z[i:i + batch])[0] for i in range(0, len(z), batch)]
        return 1 / (1 + np.exp(-np.concatenate(out)))


def train(net, z, y, z_val, y_val, epochs=30, batch=128, lr=3e-3, seed=0, log=print):
    """Adam on the cross-entropy; keeps the weights of the best validation loss."""
    rng = np.random.default_rng(seed)
    m = {k: np.zeros_like(v) for k, v in net.p.items()}
    v2 = {k: np.zeros_like(v) for k, v in net.p.items()}
    step, best, best_p = 0, math.inf, None
    y = y.astype(np.float64)
    for epoch in range(epochs):
        t0 = time.time()
        order = rng.permutation(len(z))
        for i in range(0, len(z), batch):
            idx = order[i:i + batch]
            logit, _, _ = net.forward(z[idx], keep=True)
            prob = 1 / (1 + np.exp(-logit))
            grads = net.backward((prob - y[idx]) / len(idx))
            step += 1
            for k in net.p:
                m[k] = 0.9 * m[k] + 0.1 * grads[k]
                v2[k] = 0.999 * v2[k] + 0.001 * grads[k] ** 2
                net.p[k] -= lr * (m[k] / (1 - 0.9 ** step)) / (np.sqrt(v2[k] / (1 - 0.999 ** step)) + 1e-8)
        val = log_loss(net.predict(z_val), y_val)
        if val < best:
            best, best_p = val, {k: v.copy() for k, v in net.p.items()}
        log(f"epoch {epoch + 1:>2}: validation loss {val:.4f}  ({time.time() - t0:.1f} s)")
    net.p = best_p
    return best


def log_loss(prob, y):
    prob = np.clip(prob, 1e-7, 1 - 1e-7)
    return float(-np.mean(y * np.log(prob) + (1 - y) * np.log(1 - prob)))


# ---------------------------------------------------------------- 8 bits

def _multiplier(real):
    """real > 0 -> (m, shift) with m in [2^30, 2^31): x * real ~= (x * m + 2^(shift-1)) >> shift."""
    shift = 31
    while real * 2 ** shift < 2 ** 30:
        shift += 1
    while real * 2 ** shift >= 2 ** 31:
        shift -= 1
    return int(round(real * 2 ** shift)), shift


def _requant(acc, m, shift):
    return np.clip((acc * m + (1 << (shift - 1))) >> shift, -128, 127)


class QuantArcNet:
    """The network as the MCX runs it: integers only, up to the logit. Built from a trained ArcNet."""

    def __init__(self, net, z_calib):
        p = net.p
        s0 = 1 / 127
        _, a1, a2 = net.forward(z_calib)
        s1 = max(np.percentile(a1, 99.99), 1e-6) / 127
        s2 = max(np.percentile(a2, 99.99), 1e-6) / 127
        sw1 = max(np.abs(p["w1"]).max(), 1e-9) / 127
        sw2 = max(np.abs(p["w2"]).max(), 1e-9) / 127
        sw3 = max(np.abs(p["w3"]).max(), 1e-9) / 127
        self.w1 = np.round(p["w1"] / sw1).astype(np.int64).reshape(C1, K1)
        self.b1 = np.round(p["b1"] / (s0 * sw1)).astype(np.int64)
        self.m1, self.sh1 = _multiplier(s0 * sw1 / s1)
        self.w2 = np.round(p["w2"] / sw2).astype(np.int64).reshape(C2, C1 * K2)
        self.b2 = np.round(p["b2"] / (s1 * sw2)).astype(np.int64)
        self.m2, self.sh2 = _multiplier(s1 * sw2 / s2)
        self.w3 = np.round(p["w3"] / sw3).astype(np.int64)
        self.b3 = int(np.round(p["b3"][0] / (s2 * sw3)))
        self.logit_scale = float(np.float32(s2 * sw3))

    def accumulate(self, q0):
        """(N, WINDOW) int8 values -> the dense layer's int32 sum (N,), and the pooled features (N, 32)."""
        q0 = np.asarray(q0, dtype=np.int64)
        c1 = _cols(np.ascontiguousarray(q0[:, None, :]), K1, S1)
        q1 = np.maximum(_requant(c1 @ self.w1.T + self.b1, self.m1, self.sh1), 0).transpose(0, 2, 1)
        c2 = _cols(np.ascontiguousarray(q1), K2, S2)
        q2 = np.maximum(_requant(c2 @ self.w2.T + self.b2, self.m2, self.sh2), 0).transpose(0, 2, 1)
        feat = np.concatenate([(q2.sum(axis=2) + L2 // 2) // L2, q2.max(axis=2)], axis=1)
        return feat @ self.w3 + self.b3, feat

    def predict(self, z):
        acc, _ = self.accumulate(quantize_input(z))
        logit = acc.astype(np.float32) * np.float32(self.logit_scale)
        return 1 / (1 + np.exp(-logit.astype(np.float64)))


# ---------------------------------------------------------------- the rule-based fallback

NOISE_FLOOR = 1e-3               # log(noise + this): a window with no noise at all stays finite
CURRENT_FLOOR_UA = 0.1           # log(max(median, this))


def rule_inputs(z, sigma, median):
    """The rule's three inputs: log(1 + pulses per second), log(noise sigma), log(median current µA)."""
    return np.stack([np.log1p(pulse_rate(z, sigma)), np.log(sigma + NOISE_FLOOR),
                     np.log(np.maximum(median, CURRENT_FLOOR_UA))], axis=1)


def fit_rule(z, sigma, median, y, iterations=100):
    """Logistic regression of the label on the rule's inputs:
    risk = sigmoid(a·log(1 + rate) + c·log(sigma) + d·log(current) + b).
    Newton's method, each step halved until the loss goes down. Returns (a, c, d, b)."""
    A = np.hstack([rule_inputs(z, sigma, median), np.ones((len(z), 1))])
    y = np.asarray(y, dtype=np.float64)

    def prob(w):
        return 1 / (1 + np.exp(-np.clip(A @ w, -30, 30)))

    w = np.array([0.0, 0.0, 0.0, math.log(max(y.mean(), 1e-3) / max(1 - y.mean(), 1e-3))])
    for _ in range(iterations):
        p = prob(w)
        h = (A * (p * (1 - p) + 1e-9)[:, None]).T @ A + 1e-6 * np.eye(4)
        step = np.linalg.solve(h, A.T @ (p - y))
        now = log_loss(p, y)
        while np.abs(step).max() > 1e-9 and log_loss(prob(w - step), y) > now:
            step = step / 2
        if np.abs(step).max() <= 1e-9:
            break
        w = w - step
    return tuple(float(v) for v in w)


def rule_predict(z, sigma, median, rule):
    a, c, d, b = rule
    f = rule_inputs(z, sigma, median)
    return 1 / (1 + np.exp(-np.clip(a * f[:, 0] + c * f[:, 1] + d * f[:, 2] + b, -30, 30)))


# ---------------------------------------------------------------- export for the firmware

def _c_array(ctype, name, values, per_line=16):
    values = [int(v) for v in np.asarray(values).ravel()]
    lines = [", ".join(str(v) for v in values[i:i + per_line]) for i in range(0, len(values), per_line)]
    return f"static const {ctype} {name}[{len(values)}] = {{\n    " + ",\n    ".join(lines) + "};\n"


def header(q, rule, name, notes):
    a, c, d, b = rule
    return (
        "// The arc predictor's weights (M5a), 8 bits. GENERATED by python -m ml arc-train: do not edit, train again.\n"
        f"// {notes}\n"
        "#pragma once\n#include <stdint.h>\n\n"
        f'#define ARC_MODEL_NAME "{name}"\n'
        f"#define ARC_MODEL_INPUT {WINDOW}\n#define ARC_MODEL_FS_HZ {FS_HZ}\n"
        f"#define ARC_MODEL_C1 {C1}\n#define ARC_MODEL_K1 {K1}\n#define ARC_MODEL_C2 {C2}\n#define ARC_MODEL_K2 {K2}\n\n"
        + _c_array("int8_t", "kArcW1", q.w1) + _c_array("int32_t", "kArcB1", q.b1)
        + f"static const int32_t kArcM1 = {q.m1};\nstatic const int kArcShift1 = {q.sh1};\n\n"
        + _c_array("int8_t", "kArcW2", q.w2) + _c_array("int32_t", "kArcB2", q.b2)
        + f"static const int32_t kArcM2 = {q.m2};\nstatic const int kArcShift2 = {q.sh2};\n\n"
        + _c_array("int8_t", "kArcW3", q.w3)
        + f"static const int32_t kArcB3 = {q.b3};\n"
        + f"static const float kArcLogitScale = {float(np.float32(q.logit_scale))!r}f;\n\n"
        + "// Rule-based fallback: risk = 1 / (1 + exp(-(A * log(1 + pulses per second) + C * log(noise + 0.001)\n"
        + "//                                           + D * log(max(median uA, 0.1)) + B)))\n"
        + f"static const float kArcRuleA = {float(np.float32(a))!r}f;\nstatic const float kArcRuleC = {float(np.float32(c))!r}f;\n"
        + f"static const float kArcRuleD = {float(np.float32(d))!r}f;\nstatic const float kArcRuleB = {float(np.float32(b))!r}f;\n"
    )


def test_header(q, windows):
    """Golden vectors for the firmware test: the same windows must give the same integers on the PC and the MCX."""
    z, sigma, median = preprocess(windows)
    q0 = quantize_input(z)
    acc, feat = q.accumulate(q0)
    out = ["// Golden vectors for test/test_edge_ai. GENERATED by python -m ml arc-train: do not edit.\n#pragma once\n"
           "#include <stdint.h>\n\n", f"#define ARC_TEST_WINDOWS {len(windows)}\n\n"]
    out.append("// Raw windows (µA, as floats), their int8 input, the pooled features and the dense layer's sum.\n")
    exact = lambda v: np.format_float_positional(np.float32(v), unique=True, trim="0") + "f"  # the same float32
    rows = ",\n    ".join("{" + ", ".join(exact(v) for v in w) + "}" for w in np.asarray(windows, dtype=np.float32))
    out.append(f"static const float kArcTestUa[{len(windows)}][{WINDOW}] = {{\n    {rows}}};\n")
    rows = ",\n    ".join("{" + ", ".join(str(int(v)) for v in r) + "}" for r in q0)
    out.append(f"static const int8_t kArcTestInput[{len(windows)}][{WINDOW}] = {{\n    {rows}}};\n")
    rows = ",\n    ".join("{" + ", ".join(str(int(v)) for v in r) + "}" for r in feat)
    out.append(f"static const int8_t kArcTestFeatures[{len(windows)}][{FEATURES}] = {{\n    {rows}}};\n")
    out.append(_c_array("int32_t", "kArcTestSum", acc))
    rate = pulse_rate(z, sigma)
    out.append("static const float kArcTestPulseRate[%d] = {%s};\n" % (len(windows), ", ".join(exact(r) for r in rate)))
    out.append("static const float kArcTestNoise[%d] = {%s};\n" % (len(windows), ", ".join(exact(s) for s in sigma)))
    out.append("static const float kArcTestMedian[%d] = {%s};\n" % (len(windows), ", ".join(exact(m) for m in median)))
    return "".join(out)


QUANT_FIELDS = ("w1", "b1", "m1", "sh1", "w2", "b2", "m2", "sh2", "w3", "b3", "logit_scale")


def save_model(path, net, q, rule, info):
    """One version of the model: float weights (to train on), int8 network and rule (as flashed), and info."""
    import json

    np.savez(path, **{f"float_{k}": v for k, v in net.p.items()}, **{f"q_{k}": getattr(q, k) for k in QUANT_FIELDS},
             rule=np.array(rule), info=np.array(json.dumps(info)))


def load_model(path):
    """-> (QuantArcNet, rule, info) as saved by save_model."""
    import json

    q = QuantArcNet.__new__(QuantArcNet)
    with np.load(path) as data:  # closed at once: Windows does not let an open file be written again
        for k in QUANT_FIELDS:
            value = data[f"q_{k}"]
            setattr(q, k, value if value.ndim else value.item())
        return q, tuple(float(v) for v in data["rule"]), json.loads(str(data["info"]))