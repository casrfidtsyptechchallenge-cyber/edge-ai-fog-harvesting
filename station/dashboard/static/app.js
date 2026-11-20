// AeroIon-Edge station dashboard (M10). Plain JavaScript, no library and nothing from the internet: the Pi's
// hotspot has none. Data arrives on /api/stream (Server-Sent Events). Text that comes from a node (reasons,
// details, badge ids) is always inserted with textContent, never as HTML.
"use strict";

const SVG = "http://www.w3.org/2000/svg";
const CHARTS = [  // index: position in a history point [ts, fog_opt, fog_virt, i_ua, mass_g, rate_gpm, hv_level, p_w]
  { key: "fog", title: "Fog index (optical sensor)", index: 1, unit: "", max: 1, digits: 2 },
  { key: "water", title: "Water collected (g)", index: 4, unit: "g", digits: 1 },
  { key: "current", title: "Collector current (µA)", index: 3, unit: "µA", digits: 1 },
];
const GAP_MS = 5000;  // a line breaks where a node was silent longer than this
const SEVERITY_WORD = { critical: "Critical", serious: "Serious", warning: "Warning", info: "Info", good: "Good" };

const WATER_VERDICT = { ok: "Safe to drink", bad: "NOT SAFE", unknown: "Quality unknown" };
const view = {
  nodes: [], totals: null, alerts: [], source: null, commands: false, water: null,
  history: {}, historyMs: 600000, front: null, feed: [], streamOk: false,
};
const charts = {};
const cards = {};

// ------------------------------------------------------------------ small helpers

function h(tag, attrs, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (k === "class") e.className = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else e.setAttribute(k, v);
  }
  for (const c of children) if (c !== null && c !== undefined) e.append(c);  // strings become text nodes
  return e;
}

function s(tag, attrs) {
  const e = document.createElementNS(SVG, tag);
  for (const [k, v] of Object.entries(attrs || {})) e.setAttribute(k, v);
  return e;
}

function svgText(x, y, words, attrs) {
  const t = s("text", Object.assign({ x, y }, attrs || {}));
  t.textContent = words;
  return t;
}

function nodeColor(id) {
  const n = parseInt(String(id).slice(1), 10);
  return n >= 1 && n <= 8 ? `var(--node-${n})` : "var(--node-other)";
}

function num(v, digits) {
  if (v === null || v === undefined) return "–";
  return Number(v).toLocaleString(undefined, { minimumFractionDigits: digits, maximumFractionDigits: digits });
}

function grams(v) {
  return v === null || v === undefined ? "–" : num(v, Math.abs(v) >= 100 ? 0 : 1);
}

function clockTime(ts, withSeconds = true) {  // 24-hour: short, and the same on every screen
  const opts = { hour: "2-digit", minute: "2-digit", hourCycle: "h23" };
  if (withSeconds) opts.second = "2-digit";
  return new Date(ts).toLocaleTimeString([], opts);
}

function compass(deg) {
  const names = ["north", "north-north-east", "north-east", "east-north-east", "east", "east-south-east",
    "south-east", "south-south-east", "south", "south-south-west", "south-west", "west-south-west", "west",
    "west-north-west", "north-west", "north-north-west"];
  return names[Math.round(((deg % 360) + 360) % 360 / 22.5) % 16];
}

function keyLabel(id) {
  const station = id === "all" || id === "water";
  return h("span", { class: "key" }, h("i", { style: `background:${station ? "var(--muted)" : nodeColor(id)}` }), id);
}

function severity(level) {
  return h("span", { class: `sev sev-${level}` }, h("i"), SEVERITY_WORD[level] || level);
}

function stateChip(state) {
  return h("span", { class: `state state-${state || "unknown"}` }, h("i"), state || "no data");
}

function toast(words) {
  const t = document.getElementById("toast");
  t.textContent = words;
  t.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { t.hidden = true; }, 5000);
}

function lastTs() {
  let t = 0;
  for (const points of Object.values(view.history)) if (points.length) t = Math.max(t, points[points.length - 1][0]);
  return t;
}

// ------------------------------------------------------------------ live stream

function connect() {
  const es = new EventSource("/api/stream");
  es.addEventListener("snapshot", (e) => { view.streamOk = true; applySnapshot(JSON.parse(e.data)); });
  es.addEventListener("tick", (e) => { view.streamOk = true; applyTick(JSON.parse(e.data)); });
  es.addEventListener("point", (e) => addPoint(JSON.parse(e.data)));
  es.addEventListener("feed", (e) => addFeed(JSON.parse(e.data)));
  es.addEventListener("front", (e) => { view.front = JSON.parse(e.data); renderFront(); renderMap(); });
  es.onerror = () => { view.streamOk = false; renderSource(); };  // the browser reconnects by itself
}

function applySnapshot(snap) {
  view.history = snap.history || {};
  view.historyMs = (snap.history_s || 600) * 1000;
  view.front = snap.front;
  view.feed = snap.feed || [];
  view.commands = !!snap.commands;
  document.getElementById("lock-all").hidden = !view.commands;
  for (const id of Object.keys(cards)) { cards[id].root.remove(); delete cards[id]; }
  applyTick(snap);
  renderFeed();
  renderFront();
  renderCharts();
}

function applyTick(t) {
  view.nodes = t.nodes || [];
  view.totals = t.totals;
  view.alerts = t.alerts || [];
  view.source = t.source;
  view.water = t.water || null;
  renderSource();
  renderWater();
  renderSource();
  renderTiles();
  renderAlerts();
  renderNodes();
  renderMap();
}

function addPoint(msg) {
  const points = view.history[msg.node] || (view.history[msg.node] = []);
  const p = msg.p;
  let i = points.length;
  while (i > 0 && points[i - 1][0] > p[0]) i--;  // replayed points go back in time order
  if (i > 0 && points[i - 1][0] === p[0]) return;
  points.splice(i, 0, p);
  const cut = points[points.length - 1][0] - view.historyMs;
  while (points.length && points[0][0] < cut) points.shift();
  scheduleCharts();
}

function addFeed(entry) {
  view.feed.push(entry);
  if (view.feed.length > 200) view.feed.shift();
  const list = document.getElementById("feed");
  list.prepend(feedRow(entry));
  while (list.children.length > 200) list.lastChild.remove();
}

// ------------------------------------------------------------------ header, tiles, alerts, feed

function renderSource() {
  const pill = document.getElementById("source");
  const src = view.source || {};
  let words, cls;
  if (!view.streamOk) { words = "station not reachable, reconnecting…"; cls = "bad"; }
  else if (src.kind === "mqtt") { words = src.connected ? `broker ${src.detail}` : `broker: ${src.detail}`; cls = src.connected ? "ok" : "bad"; }
  else if (src.kind === "replay") { words = src.connected ? `replay: ${src.detail}` : src.detail; cls = ""; }
  else { words = "starting…"; cls = ""; }
  pill.textContent = words;
  pill.className = `pill ${cls}`;
}

function renderTiles() {
  const t = view.totals || {};
  document.getElementById("t-water").textContent = grams(t.water_g);
  document.getElementById("t-water-sub").textContent =
    `on the collectors (since the last TARE) · ${t.rate_gpm === null || t.rate_gpm === undefined ? "–" : num(t.rate_gpm, 2)} g/min now`;
  document.getElementById("t-harvest").textContent = t.nodes ? `${t.nodes_harvesting} of ${t.nodes}` : "–";
  document.getElementById("t-harvest-sub").textContent = t.nodes ? `${t.nodes_live} of ${t.nodes} sending data` : "no node heard yet";
  document.getElementById("t-gwh").textContent = t.g_per_wh === null || t.g_per_wh === undefined ? "–" : `${num(t.g_per_wh, 0)} g/Wh`;
  document.getElementById("t-gwh-sub").textContent = t.energy_wh === null || t.energy_wh === undefined
    ? "needs the power sensor (INA219)"
    : `${num(t.energy_wh, 2)} Wh used · ${t.power_w === null ? "–" : num(t.power_w, 2)} W now`;
  document.getElementById("t-arcs").textContent = t.arc_warnings === undefined ? "–" : num(t.arc_warnings, 0);
  document.getElementById("t-arcs-sub").textContent = t.arcs === undefined ? ""
    : `warnings that made a node back off · ${t.arcs} arc(s) happened, ${t.arcs_missed} not predicted`;
}

function renderFront() {
  const f = view.front;
  const value = document.getElementById("t-front");
  const sub = document.getElementById("t-front-sub");
  if (!f) {
    value.textContent = "not tracked yet";
    sub.textContent = "the tracker (M8) publishes it";
  } else if (!f.active) {
    value.textContent = "no front now";
    sub.textContent = f.ts ? `last update ${clockTime(f.ts)}` : "";
  } else if (f.direction_deg === null) {
    value.textContent = "fog seen";
    sub.textContent = `at ${(f.nodes_detected || []).join(", ") || "–"} · direction once 3 nodes have seen it`;
  } else {
    value.textContent = `toward ${compass(f.direction_deg)}`;
    const next = Object.entries(f.eta_s || {}).sort((a, b) => a[1] - b[1])[0];
    sub.textContent = `${f.speed_mps === null ? "–" : num(f.speed_mps, 1)} m/s · fit ${f.fit_quality === null ? "–" : num(f.fit_quality, 2)}`
      + (next ? ` · ${next[0]} in ${num(next[1], 0)} s` : "");
  }
}

function renderAlerts() {
  const list = document.getElementById("alerts");
  list.replaceChildren();
  if (!view.alerts.length) {
    list.append(h("li", { class: "none" }, severity("good"), " Nothing needs attention"));
    return;
  }
  for (const a of view.alerts) list.append(h("li", {}, severity(a.severity), keyLabel(a.node), h("span", {}, a.text)));
}

function feedRow(e) {
  return h("li", {}, h("span", { class: "time" }, clockTime(e.ts || e.rx_ms)), keyLabel(e.node), severity(e.severity),
    h("span", {}, e.text, e.buffered ? h("span", { class: "tag", title: "sent later from the node's offline buffer" }, "replayed") : null));
}

function renderFeed() {
  const list = document.getElementById("feed");
  list.replaceChildren(...view.feed.slice().reverse().map(feedRow));
}

// ------------------------------------------------------------------ node cards

function metric(label, cls) {
  const value = h("div", { class: "value" });
  const root = h("div", { class: `metric ${cls || ""}` }, h("div", { class: "label" }, label), value);
  return { root, value };
}

function meter(m) {
  const fill = h("div");
  const bar = h("div", { class: "meter" }, fill);
  m.root.append(bar);
  return { bar, fill };
}

function createCard(id) {
  const c = { root: h("article", { class: "card node-card" }) };
  c.state = h("span");
  c.age = h("span", { class: "age" });
  c.reason = h("div", { class: "node-reason" });
  c.m = {
    fog: metric("Fog, optical"), fogv: metric("Fog, virtual"), water: metric("Water"),
    current: metric("Collector current"), hv: metric("HV level"), health: metric("Electrode health"),
    risk: metric("Arc risk", "detail"), power: metric("Power", "detail"), air: metric("Air", "detail"),
    safety: metric("Safety sensors", "detail"), link: metric("Link", "detail"), batt: metric("Battery", "detail"),
  };
  c.hvMeter = meter(c.m.hv);
  c.healthMeter = meter(c.m.health);
  c.alerts = h("ul", { class: "node-alerts" });
  c.fw = h("span", { class: "muted" });
  c.foot = h("div", { class: "node-foot" },
    h("button", { type: "button", class: "btn btn-small", onclick: () => sendCommand(id, "TARE") }, "TARE"),
    h("button", { type: "button", class: "btn btn-small", onclick: () => sendCommand(id, "LOCK") }, "LOCK"),
    c.fw);
  c.root.append(h("div", { class: "node-head" }, keyLabel(id), c.state, c.age), c.reason,
    h("div", { class: "metrics" }, ...Object.values(c.m).map((m) => m.root)), c.alerts, c.foot);
  return c;
}

function setMetric(m, main, small) {
  m.value.replaceChildren(main, small ? h("small", {}, ` ${small}`) : "");
}

function yesNo(v, yes, no) {
  return v === null || v === undefined ? "–" : v ? yes : no;
}

function updateCard(c, n) {
  c.state.replaceChildren(stateChip(n.live || n.online !== false ? n.state : null));
  c.age.textContent = n.online === false ? "offline" : n.age_s === null ? "no data yet" : n.live ? "live" : `silent ${num(n.age_s, 0)} s`;
  c.root.classList.toggle("stale", !n.live);
  c.reason.textContent = n.state_reason ? `last change: ${n.state_reason}` : "";
  setMetric(c.m.fog, num(n.fog_idx_opt, 2));
  setMetric(c.m.fogv, num(n.fog_idx_virt, 2), n.fog_idx_virt === null && n.hv_on === false ? "HV off" : "");
  setMetric(c.m.water, grams(n.mass_g), n.mass_g === null ? "" : `g · ${num(n.rate_gpm, 2)} g/min`);
  setMetric(c.m.current, num(n.i_ua, 1), n.i_ua === null ? "" : "µA");
  setMetric(c.m.hv, num(n.hv_level, 2), n.hv_on === null ? "" : n.hv_on ? "on" : "off");
  c.hvMeter.fill.style.width = `${Math.round((n.hv_level || 0) * 100)}%`;
  const score = n.health.score;
  setMetric(c.m.health, num(score, 2), score === null ? "" : score >= 0.5 ? "good" : score >= 0.25 ? "clean soon" : "clean now");
  c.healthMeter.bar.className = `meter ${score === null ? "" : score >= 0.5 ? "good" : score >= 0.25 ? "warning" : "critical"}`;
  c.healthMeter.fill.style.width = `${Math.round((score || 0) * 100)}%`;
  // arc_novel (contract 1.3): share of the predictor's recent windows unlike its training data
  const unsure = n.arc_novel !== null && n.arc_novel !== undefined && n.arc_novel > 0.2;
  setMetric(c.m.risk, num(n.arc_risk, 2), unsure
    ? `AI unsure: ${Math.round(n.arc_novel * 100)} % of windows unlike its training (not climbing)`
    : `${n.counters.arc_warnings} warning(s), ${n.counters.arcs} arc(s)`);
  setMetric(c.m.power, num(n.p_w, 2), n.p_w === null ? "" : `W · ${num(n.e_wh, 3)} Wh`);
  setMetric(c.m.air, n.t_c === null ? "–" : `${num(n.t_c, 1)} °C`, n.rh_pct === null ? "" : `${num(n.rh_pct, 0)} % RH`);
  setMetric(c.m.safety, `person ${yesNo(n.radar_presence, "YES", "no")}`, `lid ${yesNo(n.lid_closed, "closed", "OPEN")}`);
  setMetric(c.m.link, n.rssi_dbm === null ? "–" : `${num(n.rssi_dbm, 0)} dBm`,
    n.time_synced === null ? "" : n.time_synced ? "clock synced" : "clock NOT synced");
  setMetric(c.m.batt, n.batt_pct === null ? "bench power" : `${num(n.batt_pct, 0)} %`);
  c.alerts.replaceChildren(...n.alerts.map((a) => h("li", {}, severity(a.severity), " ", a.text)));
  c.foot.hidden = !view.commands;
  c.fw.textContent = n.fw ? `firmware ${n.fw}` : "";
}

function renderNodes() {
  const box = document.getElementById("nodes");
  if (!view.nodes.length && !box.children.length) {
    box.append(h("p", { class: "muted" }, "No node heard yet. Is the broker running, and a node or the simulator?"));
  }
  for (const n of view.nodes) {
    if (!cards[n.node_id]) {
      if (!Object.keys(cards).length) box.replaceChildren();
      cards[n.node_id] = createCard(n.node_id);
      box.append(cards[n.node_id].root);
    }
    updateCard(cards[n.node_id], n);
  }
}

// ------------------------------------------------------------------ water point (M9)

const water = {};

function createWater() {
  water.m = {
    tds: metric("TDS"), ph: metric("pH"), temp: metric("Water temperature", "detail"), valve: metric("Valve"),
    session: metric("Drawing now"), filter: metric("Filter"), today: metric("Given out today"),
  };
  water.sessionMeter = meter(water.m.session);
  water.filterMeter = meter(water.m.filter);
  document.getElementById("w-metrics").append(...Object.values(water.m).map((m) => m.root));
}

function renderWater() {
  const w = view.water;
  const card = document.getElementById("water-card");
  card.hidden = !w;
  if (!w) return;
  if (!water.m) createWater();
  const q = w.quality, s = w.session, f = w.filter;
  const verdict = w.live ? q.verdict : "unknown";
  document.getElementById("w-verdict").replaceChildren(
    h("span", { class: `state water-${verdict}` }, h("i"), w.live ? WATER_VERDICT[verdict] : "no status"));
  document.getElementById("w-age").textContent = w.live ? "live" : `silent ${num(w.age_s, 0)} s`;
  card.classList.toggle("stale", !w.live);
  document.getElementById("w-reason").textContent = q.reason
    ? (q.verdict === "ok" ? q.reason : `${q.reason}: the valve stays closed`) : "";
  setMetric(water.m.tds, q.tds_ppm === null ? "–" : `${num(q.tds_ppm, 0)} ppm`, "safe below 500");
  setMetric(water.m.ph, num(q.ph, 2), "safe 6.5 to 8.5");
  setMetric(water.m.temp, q.temp_c === null ? "–" : `${num(q.temp_c, 1)} °C`);
  setMetric(water.m.valve, w.valve_open ? "open" : "closed", w.valve_open ? "water flowing" : "");
  water.sessionMeter.bar.hidden = !s;
  if (s) {
    setMetric(water.m.session, s.card, `${num(s.litres, 1)} L · ${num(s.quota_left_l, 1)} L left today`);
    const total = (s.litres || 0) + (s.quota_left_l || 0);
    water.sessionMeter.fill.style.width = `${total ? Math.round(100 * (s.litres || 0) / total) : 0}%`;
  } else {
    setMetric(water.m.session, "no one", "a household card opens the valve");
  }
  const filterWords = { ok: "", expiring: "change it soon", expired: "used up: change it", none: "none fitted" };
  setMetric(water.m.filter, f.tag || "none",
    [f.litres === null ? "" : `${num(f.litres, 0)} of ${num(f.capacity_l, 0)} L`, filterWords[f.state] || ""]
      .filter(Boolean).join(" · "));
  water.filterMeter.bar.className = `meter ${{ ok: "good", expiring: "warning", expired: "critical" }[f.state] || ""}`;
  water.filterMeter.fill.style.width = f.litres === null || !f.capacity_l ? "0%"
    : `${Math.min(100, Math.round(100 * f.litres / f.capacity_l))}%`;
  setMetric(water.m.today, w.dispensed_today_l === null ? "–" : `${num(w.dispensed_today_l, 1)} L`);
}

// ------------------------------------------------------------------ commands (LOCK and TARE only)

async function sendCommand(node, type) {
  const question = type === "LOCK"
    ? `${node === "all" ? "Stop EVERY node" : `Stop ${node}`} (LOCK)? The HV turns off; an operator badge is needed to arm again.`
    : `Zero the load cell of ${node} (TARE)? Its water count restarts from 0.`;
  if (!window.confirm(question)) return;
  try {
    const r = await fetch("/api/cmd", {
      method: "POST",
      headers: { "Content-Type": "application/json", "X-AeroIon-Dashboard": "1" },
      body: JSON.stringify({ node, type, reason: "stop from the dashboard" }),
    });
    const body = await r.json().catch(() => ({}));
    toast(r.ok ? `${type} sent to ${node}: the node's answer appears in the events` : `Not sent: ${body.error || r.status}`);
  } catch (err) {
    toast("Not sent: the station cannot be reached");
  }
}

// ------------------------------------------------------------------ site map

function renderMap() {
  const svg = document.getElementById("map");
  const W = svg.clientWidth || 600;
  const H = svg.clientHeight || 300;
  svg.setAttribute("viewBox", `0 0 ${W} ${H}`);
  svg.replaceChildren();
  const caption = document.getElementById("map-caption");
  const nodes = view.nodes;
  if (!nodes.length) { caption.textContent = "No node yet."; return; }

  const placed = nodes.filter((n) => n.position);
  let pos;
  if (placed.length) {
    const xs = placed.map((n) => n.position.x_m), ys = placed.map((n) => n.position.y_m);
    const minX = Math.min(...xs) - 6, maxX = Math.max(...xs) + 6, minY = Math.min(...ys) - 6, maxY = Math.max(...ys) + 6;
    const k = Math.min((W - 80) / (maxX - minX), (H - 70) / (maxY - minY));
    const ox = (W - (maxX - minX) * k) / 2, oy = (H - (maxY - minY) * k) / 2;
    pos = (n) => n.position ? [ox + (n.position.x_m - minX) * k, oy + (maxY - n.position.y_m) * k] : null;
    // scale bar: a round length about a quarter of the width
    const target = (W / 4) / k;
    const len = [1, 2, 5, 10, 20, 50, 100, 200, 500].find((v) => v >= target * 0.6) || 1000;
    svg.append(s("line", { x1: 16, y1: H - 14, x2: 16 + len * k, y2: H - 14, stroke: "var(--ink-2)", "stroke-width": 2 }));
    svg.append(svgText(16, H - 20, `${len} m`, { fill: "var(--ink-2)", "font-size": 11 }));
    caption.textContent = "Positions from each node's info message. North is up.";
  } else {
    pos = (n) => [W * (nodes.indexOf(n) + 1) / (nodes.length + 1), H / 2];
    caption.textContent = "Positions unknown (no info message yet): nodes shown in a row.";
  }
  // north arrow
  svg.append(s("path", { d: `M ${W - 24} 34 L ${W - 18} 18 L ${W - 12} 34 Z`, fill: "var(--ink-2)" }));
  svg.append(svgText(W - 18, 48, "N", { "text-anchor": "middle", fill: "var(--ink-2)", "font-size": 11, "font-weight": 600 }));

  const f = view.front;
  const eta = f && f.active ? f.eta_s || {} : {};
  for (const n of nodes) {  // fog around each node: a grey halo, wider with denser fog
    const p = pos(n);
    if (p && n.live && n.fog_idx_opt !== null) svg.append(s("circle", { cx: p[0], cy: p[1], r: 12 + 34 * n.fog_idx_opt, fill: "var(--fog-halo)" }));
  }
  if (f && f.active && f.direction_deg !== null) {  // the front: an arrow across the site, toward where it moves
    const pts = nodes.map(pos).filter(Boolean);
    const cx = pts.reduce((a, p) => a + p[0], 0) / pts.length, cy = pts.reduce((a, p) => a + p[1], 0) / pts.length;
    const a = f.direction_deg * Math.PI / 180, L = Math.min(W, H) * 0.42;
    const dx = Math.sin(a), dy = -Math.cos(a);
    const x1 = cx - dx * L, y1 = cy - dy * L, x2 = cx + dx * L, y2 = cy + dy * L;
    svg.append(s("line", { x1, y1, x2, y2, stroke: "var(--ink-2)", "stroke-width": 2, "stroke-linecap": "round" }));
    const hx = x2 - dx * 12, hy = y2 - dy * 12;
    svg.append(s("path", { d: `M ${x2} ${y2} L ${hx - dy * 6} ${hy + dx * 6} L ${hx + dy * 6} ${hy - dx * 6} Z`, fill: "var(--ink-2)" }));
    svg.append(svgText(x2 + dx * 12, y2 + dy * 12 + 4, "fog front", { "text-anchor": dx > 0.3 ? "start" : dx < -0.3 ? "end" : "middle", fill: "var(--ink-2)", "font-size": 11 }));
    caption.textContent += ` Fog moving toward the ${compass(f.direction_deg)} (${num(f.direction_deg, 0)}°) at ${num(f.speed_mps, 1)} m/s.`;
  }
  for (const n of nodes) {
    const p = pos(n);
    if (!p) continue;
    svg.append(s("circle", { cx: p[0], cy: p[1], r: 9, fill: n.live ? nodeColor(n.node_id) : "var(--surface)",
      stroke: n.live ? "var(--surface)" : nodeColor(n.node_id), "stroke-width": 2 }));
    svg.append(svgText(p[0], p[1] - 15, n.node_id, { "text-anchor": "middle", fill: "var(--ink)", "font-size": 12, "font-weight": 600 }));
    const line2 = n.online === false ? "offline" : !n.live ? "silent" : n.state || "";
    svg.append(svgText(p[0], p[1] + 24, line2, { "text-anchor": "middle", fill: "var(--ink-2)", "font-size": 11 }));
    if (eta[n.node_id] !== undefined) {
      svg.append(svgText(p[0], p[1] + 38, `fog in ${num(eta[n.node_id], 0)} s`, { "text-anchor": "middle", fill: "var(--ink)", "font-size": 11, "font-weight": 600 }));
    }
  }
}

// ------------------------------------------------------------------ charts

function niceStep(range, target) {
  const raw = range / target, p = Math.pow(10, Math.floor(Math.log10(raw)));
  return [1, 2, 2.5, 5, 10].map((m) => m * p).find((v) => v >= raw) || 10 * p;
}

function createChart(card, def) {
  const legend = h("div", { class: "legend" });
  const svg = s("svg", { class: "chart-svg", tabindex: "0", role: "img", "aria-label": def.title });
  card.append(h("div", { class: "chart-head" }, h("h2", {}, def.title), legend), svg);
  const c = { def, svg, legend, cursor: null, geom: null };
  svg.addEventListener("pointermove", (e) => { c.cursor = { t: xToT(c, e.clientX), x: e.clientX, y: e.clientY }; drawCursor(c); });
  svg.addEventListener("pointerleave", () => { c.cursor = null; drawCursor(c); });
  svg.addEventListener("focus", () => { const b = svg.getBoundingClientRect(); c.cursor = { t: lastTs(), x: b.right - 40, y: b.top + 20 }; drawCursor(c); });
  svg.addEventListener("blur", () => { c.cursor = null; drawCursor(c); });
  svg.addEventListener("keydown", (e) => {
    if (!c.cursor || !["ArrowLeft", "ArrowRight"].includes(e.key)) return;
    c.cursor.t += (e.key === "ArrowLeft" ? -1 : 1) * view.historyMs / 60;
    e.preventDefault();
    drawCursor(c);
  });
  return c;
}

function xToT(c, clientX) {
  const g = c.geom;
  if (!g) return 0;
  const x = clientX - c.svg.getBoundingClientRect().left;
  return g.t0 + (Math.min(Math.max(x, g.left), g.right) - g.left) / (g.right - g.left) * (g.t1 - g.t0);
}

let chartTimer = null;
function scheduleCharts() {
  if (!chartTimer) chartTimer = setTimeout(() => { chartTimer = null; renderCharts(); }, 400);
}

function renderCharts() {
  for (const c of Object.values(charts)) renderChart(c);
}

function renderChart(c) {
  const { def, svg } = c;
  const W = svg.clientWidth || 400, H = svg.clientHeight || 190;
  svg.setAttribute("viewBox", `0 0 ${W} ${H}`);
  svg.replaceChildren();
  const ids = Object.keys(view.history).filter((id) => view.history[id].length).sort();
  c.legend.replaceChildren(...(ids.length > 1 ? ids.map((id) => h("span", {}, h("i", { style: `background:${nodeColor(id)}` }), id)) : []));
  const t1 = lastTs(), t0 = t1 - view.historyMs;
  let vmax = def.max || 0;
  for (const id of ids) for (const p of view.history[id]) if (p[0] >= t0 && p[def.index] !== null) vmax = Math.max(vmax, p[def.index]);
  if (!ids.length || !t1) { c.geom = null; svg.append(svgText(W / 2, H / 2, "waiting for telemetry…", { "text-anchor": "middle" })); return; }
  if (!def.max) vmax = vmax > 0 ? vmax * 1.1 : 1;
  const step = niceStep(vmax, 4);
  vmax = def.max || Math.ceil(vmax / step) * step;
  const left = 40, right = W - (ids.length <= 4 ? 34 : 10), top = 8, bottom = H - 20;
  const X = (t) => left + (t - t0) / (t1 - t0) * (right - left);
  const Y = (v) => bottom - v / vmax * (bottom - top);
  c.geom = { left, right, top, bottom, t0, t1, X, Y };

  const tickDigits = step >= 1 ? 0 : Math.abs(step * 10 - Math.round(step * 10)) < 1e-9 ? 1 : 2;  // 0.5 -> 1, 0.25 -> 2
  for (let v = 0; v <= vmax + 1e-9; v += step) {  // hairline grid with clean ticks
    svg.append(s("line", { x1: left, x2: right, y1: Y(v), y2: Y(v), stroke: v === 0 ? "var(--axis)" : "var(--grid)", "stroke-width": 1 }));
    svg.append(svgText(left - 6, Y(v) + 4, num(v, tickDigits), { "text-anchor": "end" }));
  }
  const maxTicks = Math.max(2, Math.min(6, Math.floor((right - left) / 56)));  // labels never touch
  const tStep = [60e3, 120e3, 300e3, 600e3, 1200e3, 1800e3, 3600e3].find((m) => (t1 - t0) / m <= maxTicks) || 7200e3;
  for (let t = Math.ceil(t0 / tStep) * tStep; t <= t1; t += tStep) {
    svg.append(svgText(X(t), H - 4, clockTime(t, false), { "text-anchor": "middle" }));
  }

  const ends = [];
  for (const id of ids) {
    let d = "", prev = null;
    let last = null;
    for (const p of view.history[id]) {
      if (p[0] < t0) continue;
      const v = p[def.index];
      if (v === null) { prev = null; continue; }
      d += `${prev === null || p[0] - prev > GAP_MS ? "M" : "L"}${X(p[0]).toFixed(1)},${Y(v).toFixed(1)}`;
      prev = p[0];
      last = p;
    }
    if (!d) continue;
    svg.append(s("path", { d, fill: "none", stroke: nodeColor(id), "stroke-width": 2, "stroke-linejoin": "round", "stroke-linecap": "round" }));
    if (last) ends.push({ id, x: X(last[0]), y: Y(last[def.index]) });
  }
  ends.sort((a, b) => a.y - b.y);
  const labelsFit = ids.length <= 4 && ends.every((e, i) => i === 0 || e.y - ends[i - 1].y >= 12);
  for (const e of ends) {
    svg.append(s("circle", { cx: e.x, cy: e.y, r: 4, fill: nodeColor(e.id), stroke: "var(--surface)", "stroke-width": 2 }));
    if (labelsFit) svg.append(svgText(e.x + 7, e.y + 4, e.id, { class: "end-label" }));
  }
  c.cursorLayer = s("g");
  svg.append(c.cursorLayer);
  drawCursor(c);
}

function nearest(points, t) {
  let lo = 0, hi = points.length - 1;
  if (hi < 0) return null;
  while (lo < hi) { const mid = (lo + hi) >> 1; if (points[mid][0] < t) lo = mid + 1; else hi = mid; }
  const best = lo > 0 && Math.abs(points[lo - 1][0] - t) < Math.abs(points[lo][0] - t) ? points[lo - 1] : points[lo];
  return Math.abs(best[0] - t) <= GAP_MS ? best : null;
}

function drawCursor(c) {
  const tip = document.getElementById("tooltip");
  if (c.cursorLayer) c.cursorLayer.replaceChildren();
  const g = c.geom;
  if (!c.cursor || !g || !c.cursorLayer) { if (!document.querySelector(".chart-svg:hover, .chart-svg:focus")) tip.hidden = true; return; }
  const t = Math.min(Math.max(c.cursor.t, g.t0), g.t1);
  const rows = [];
  let snapT = null;
  for (const id of Object.keys(view.history).sort()) {
    const p = nearest(view.history[id], t);
    if (!p) continue;
    if (snapT === null || Math.abs(p[0] - t) < Math.abs(snapT - t)) snapT = p[0];
    rows.push({ id, v: p[c.def.index] });
  }
  const x = g.X(snapT === null ? t : snapT);
  c.cursorLayer.append(s("line", { x1: x, x2: x, y1: g.top, y2: g.bottom, stroke: "var(--axis)", "stroke-width": 1 }));
  tip.replaceChildren(h("div", { class: "t-time" }, clockTime(snapT === null ? t : snapT)),
    ...(rows.length ? rows.map((r) => h("div", { class: "t-row" }, h("b", {}, r.v === null ? "–" : `${num(r.v, c.def.digits)}${c.def.unit ? " " + c.def.unit : ""}`),
      h("i", { style: `background:${nodeColor(r.id)}` }), r.id)) : [h("div", {}, "no data here")]));
  tip.hidden = false;
  const px = c.svg.getBoundingClientRect().left + x;  // beside the crosshair, on whichever side has room
  let tipLeft = px + 14;
  if (tipLeft + tip.offsetWidth > window.innerWidth - 8) tipLeft = px - tip.offsetWidth - 14;
  tip.style.left = `${Math.max(8, tipLeft)}px`;
  tip.style.top = `${Math.max(8, Math.min(c.cursor.y - 20, window.innerHeight - tip.offsetHeight - 8))}px`;
}

// ------------------------------------------------------------------ page setup

function setTheme(mode) {
  if (mode === "auto") delete document.documentElement.dataset.theme;
  else document.documentElement.dataset.theme = mode;
  document.getElementById("theme-toggle").textContent = `Theme: ${mode}`;
  try { localStorage.setItem("aeroion-theme", mode); } catch (e) { /* private window: not remembered */ }
}

function setJury(on) {
  document.body.classList.toggle("jury", on);
  document.getElementById("jury-toggle").textContent = on ? "Full view" : "Jury view";
  const url = new URL(window.location.href);
  if (on) url.searchParams.set("view", "jury"); else url.searchParams.delete("view");
  history.replaceState(null, "", url);
  renderCharts();
  renderMap();
}

function start() {
  for (const card of document.querySelectorAll(".chart-card")) {
    const def = CHARTS.find((d) => d.key === card.dataset.chart);
    charts[def.key] = createChart(card, def);
  }
  let theme = "auto";
  try { theme = localStorage.getItem("aeroion-theme") || "auto"; } catch (e) { /* default */ }
  setTheme(theme);
  document.getElementById("theme-toggle").addEventListener("click", () => {
    const order = ["auto", "light", "dark"];
    setTheme(order[(order.indexOf(document.documentElement.dataset.theme || "auto") + 1) % 3]);
  });
  document.getElementById("jury-toggle").addEventListener("click", () => setJury(!document.body.classList.contains("jury")));
  document.getElementById("lock-all").addEventListener("click", () => sendCommand("all", "LOCK"));
  setJury(new URLSearchParams(window.location.search).get("view") === "jury");
  const clock = document.getElementById("clock");
  const tick = () => { clock.textContent = clockTime(Date.now()); };
  tick();
  setInterval(tick, 1000);
  window.addEventListener("resize", () => { scheduleCharts(); renderMap(); });
  connect();
}

start();