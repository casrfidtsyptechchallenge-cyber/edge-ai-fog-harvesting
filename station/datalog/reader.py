"""Read a run's raw.jsonl: keep only messages that follow the contract, drop exact duplicates, sort by time.

Every line is {"topic": ..., "payload": {...}} (plus "retain" and, when recorded live, "rx_ts").
Invalid lines are counted and reported, never silently mixed into the dataset.
"""

import json
from collections import defaultdict
from pathlib import Path

import aeroion_contract as ac

KEPT_KINDS = ("telemetry", "event", "wave", "info")


def read_text_lines(path):
    """Lines of a text file in UTF-8 (with or without BOM) or UTF-16 (Windows PowerShell's > redirection)."""
    data = Path(path).read_bytes()
    text = data.decode("utf-16") if data.startswith((b"\xff\xfe", b"\xfe\xff")) else data.decode("utf-8-sig")
    return text.splitlines()


class RunMessages:
    """The clean content of one run, per node, sorted by timestamp."""

    def __init__(self):
        self.telemetry = defaultdict(list)  # node_id -> [payload]
        self.events = defaultdict(list)
        self.waves = defaultdict(list)
        self.info = {}  # node_id -> latest info payload
        self.stats = {"lines": 0, "kept": 0, "skipped": 0, "unreadable": 0, "invalid": 0, "duplicates": 0}
        self.problems = []  # first few problems, shown in the summary
        self.quality = {}  # node_id -> {"lost": n, "restarts": n}

    def nodes(self):
        return sorted(set(self.telemetry) | set(self.events) | set(self.waves) | set(self.info))


def read_run(path, max_problems=20):
    out = RunMessages()
    seen = set()
    for lineno, line in enumerate(read_text_lines(path), 1):
        line = line.strip().lstrip("\ufeff")  # BOM left by appending files together
        if not line:
            continue
        out.stats["lines"] += 1
        try:
            row = json.loads(line)
            topic, payload = row["topic"], row["payload"]
            scope, kind = ac.parse_topic(topic)
        except (ValueError, KeyError, TypeError) as e:
            _problem(out, "unreadable", f"line {lineno}: {e}", max_problems)
            continue
        if kind not in KEPT_KINDS or scope in (ac.SIM, ac.STATION, ac.BROADCAST):
            out.stats["skipped"] += 1  # commands, status, simulator truth: not dataset material
            continue
        errors = ac.validate(kind, payload)
        if errors:
            _problem(out, "invalid", f"line {lineno} {topic}: {errors[0]}", max_problems)
            continue
        node = payload["node_id"]
        if kind == "info":
            out.info[node] = payload
            out.stats["kept"] += 1
            continue
        key = (node, payload["seq"], payload["ts"])  # an exact repeat, e.g. a buffered message sent twice
        if key in seen:
            out.stats["duplicates"] += 1
            continue
        seen.add(key)
        {"telemetry": out.telemetry, "event": out.events, "wave": out.waves}[kind][node].append(payload)
        out.stats["kept"] += 1

    for table in (out.telemetry, out.events, out.waves):
        for messages in table.values():
            messages.sort(key=lambda m: (m["ts"], m["seq"]))
    for node in out.nodes():
        out.quality[node] = _sequence_quality(out.telemetry[node] + out.events[node] + out.waves[node])
    return out


def _problem(out, kind, text, max_problems):
    out.stats[kind] += 1
    if len(out.problems) < max_problems:
        out.problems.append(text)


def _sequence_quality(messages, reorder_tolerance=50):
    """Lost messages and node restarts.

    seq restarts at 1 when a node reboots, so a big drop in seq marks a new boot. Small drops are only
    reordering (a current window is timestamped at its first sample, slightly before the event that
    triggered it). Within one boot, lost = numbers missing between the lowest and highest seq.
    """
    boots, current, top = [], [], None
    for m in sorted(messages, key=lambda m: (m["ts"], m["seq"])):
        if top is not None and m["seq"] < top - reorder_tolerance:
            boots.append(current)
            current, top = [], None
        current.append(m["seq"])
        top = m["seq"] if top is None else max(top, m["seq"])
    if current:
        boots.append(current)
    lost = sum(max(b) - min(b) + 1 - len(set(b)) for b in boots)
    return {"lost": lost, "restarts": max(0, len(boots) - 1)}