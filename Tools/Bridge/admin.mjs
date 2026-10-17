// The realm's admin page (/admin) and the anonymous usage counts behind it.
//
// Counts are per day, per event and per detail (lasthavens.web_events; setup in Tools/Deploy/setup.sql): no addresses,
// no ids. The bridge records the server-side events (a browser's first visit, an account created, a connection to the
// login or world server); the page records its own (opened, loaded and how long it took, failed and why) with
// POST /stats/event. The dashboard adds live numbers from the AzerothCore accounts and characters, and a live map of
// who is online (GET /admin?live, polled by the page) over the continent maps extract-art.mjs pulls out of the game
// data (worldmap.json and worldmap-*.png in the web directory; without them the map is a list).
//
// /admin asks for WOW_BRIDGE_ADMIN_PASSWORD (bridge.env or the environment; unset = no admin page) and keeps the
// session in a signed cookie for 12 hours.

import { createHash, createHmac, randomBytes, timingSafeEqual } from "node:crypto";
import { readFileSync } from "node:fs";
import { join } from "node:path";

// Event -> allowed details ("" = none). Anything else is dropped, so the table cannot be filled with junk.
export const EVENTS = {
    visit: ["chrome", "edge", "firefox", "safari", "other"],
    new_browser: [""],
    loaded: [""],
    load_failed: ["isolation", "webgl2", "download", "other"],
    account_created: [""],
    login_connection: [""],
    world_connection: [""],
};

// The event a page report becomes, or null. ms is only kept for "loaded" (time from page open to the login screen).
export function pageEvent(body) {
    const event = typeof body?.event === "string" ? body.event : "";
    const detail = typeof body?.detail === "string" ? body.detail : "";
    if (!["visit", "loaded", "load_failed"].includes(event) || !EVENTS[event].includes(detail)) return null;
    const ms = event === "loaded" && Number.isFinite(body.ms) && body.ms > 0 && body.ms < 600_000 ? Math.round(body.ms) : 0;
    return { event, detail, ms };
}

// Where a world position lands on a continent's map, 0..1 each way, or null when off it. WorldMapArea's left/right
// bound the world Y axis and top/bottom the X axis (both run right-to-left and bottom-to-top on the map).
export function mapPoint(c, x, y) {
    const fx = (c.left - y) / (c.left - c.right), fy = (c.top - x) / (c.top - c.bottom);
    return fx >= 0 && fx <= 1 && fy >= 0 && fy <= 1 ? [fx, fy] : null;
}

const RACES = ["", "Human", "Orc", "Dwarf", "Night Elf", "Undead", "Tauren", "Gnome", "Troll", "", "Blood Elf", "Draenei"];
const CLASSES = ["", "Warrior", "Paladin", "Hunter", "Rogue", "Priest", "Death Knight", "Shaman", "Mage", "Warlock", "", "Druid"];
const CLASS_COLORS = ["", "#c69b6d", "#f48cba", "#aad372", "#fff468", "#ffffff", "#c41e3a", "#0070dd", "#3fc7eb", "#8788ee", "", "#ff7c0a"];
const HORDE = new Set([2, 5, 6, 8, 10]);
const faction = (race) => (HORDE.has(Number(race)) ? "Horde" : "Alliance");
const CONTINENT_NAMES = { Azeroth: "Eastern Kingdoms", Expansion01: "Outland" };

const escape = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);
const num = (n) => Number(n ?? 0).toLocaleString("en-US");
const hours = (seconds) => `${num(Math.round(Number(seconds ?? 0) / 3600))} h`;
const gold = (copper) => `${num(Math.floor(Number(copper ?? 0) / 10000))}g`;
const pct = (a, b) => (Number(b) ? `${Math.round((100 * Number(a)) / Number(b))}%` : "-");
const duration = (seconds) => {
    const s = Number(seconds ?? 0);
    return s >= 86400 ? `${Math.floor(s / 86400)}d ${Math.floor((s % 86400) / 3600)}h` : `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m`;
};
const date = (epoch, withTime) => {
    if (!epoch) return "never";
    const iso = new Date(Number(epoch) * 1000).toISOString();
    return withTime ? `${iso.slice(0, 10)} ${iso.slice(11, 16)}` : iso.slice(0, 10);
};

// ---- Charts (inline SVG and CSS, no scripts) -----------------------------------------------------------------------

// Bars with 4px rounded tops, anchored to the baseline.
const bar = (x, y, w, h) => {
    const r = Math.min(4, w / 2, h);
    return `M${x},${y + h}V${y + r}Q${x},${y} ${x + r},${y}H${x + w - r}Q${x + w},${y} ${x + w},${y + r}V${y + h}Z`;
};

// One metric over the days (oldest first), as a small multiple: its title, today's value, a column per day.
function columns(title, days, values, format = num, average = false) {
    const W = 280, H = 70, gap = 2, w = (W - gap * (days.length - 1)) / days.length;
    const peak = Math.max(0, ...values), max = peak || 1;
    const total = values.reduce((a, b) => a + b, 0);
    return `<figure class="mini"><figcaption><span>${title}</span><b>${format(values.at(-1))}</b></figcaption>
<svg viewBox="0 0 ${W} ${H + 14}" role="img" aria-label="${escape(title)} per day">
<line x1="0" x2="${W}" y1="${H + 0.5}" y2="${H + 0.5}" class="axis"/>
${values.map((v, i) => {
        const h = Math.max(v ? 2 : 0, (v / max) * (H - 4));
        return `<g class="col"><rect x="${i * (w + gap)}" y="0" width="${w}" height="${H}" fill="transparent"/>`
            + `<path d="${bar(i * (w + gap), H - h, w, h)}"/><title>${days[i]}: ${format(v)}</title></g>`;
    }).join("")}
<text x="0" y="${H + 12}">${days[0].slice(5)}</text><text x="${W}" y="${H + 12}" text-anchor="end">today</text>
</svg><div class="muted">${days.length} days: ${average ? "" : `${format(total)} · `}peak ${format(peak)}</div></figure>`;
}

// Horizontal bars: [label, value, color?]. Value text stays in ink; the colour swatch carries identity.
// Percentages are of base: all rows by default.
function hbars(rows, format = num, base = rows.reduce((a, r) => a + Number(r[1]), 0)) {
    const max = Math.max(1, ...rows.map((r) => Number(r[1])));
    const total = base;
    if (!rows.length) return `<p class="muted">Nothing yet</p>`;
    return `<div class="hbars">${rows.map(([label, value, color]) => `<div class="hbar" title="${escape(label)}: ${format(value)} (${pct(value, total)})">
<span class="label">${color ? `<i style="background:${color}"></i>` : ""}${escape(label)}</span>
<span class="track"><span class="fill" style="width:${(100 * Number(value)) / max}%${color ? `;background:${color}` : ""}"></span></span>
<span class="value">${format(value)} <small>${pct(value, total)}</small></span></div>`).join("")}</div>`;
}

const sparkline = (values) => {
    const max = Math.max(1, ...values), step = 100 / Math.max(1, values.length - 1);
    return `<svg class="spark" viewBox="0 0 100 24" preserveAspectRatio="none" aria-hidden="true"><polyline points="${values
        .map((v, i) => `${(i * step).toFixed(1)},${(22 - (v / max) * 20).toFixed(1)}`).join(" ")}"/></svg>`;
};

const kpi = (label, value, sub = "", spark = null, id = "") =>
    `<div class="kpi"${id ? ` id="${id}"` : ""}><span>${label}</span><b>${escape(value)}</b>${sub ? `<small>${sub}</small>` : ""}${spark ? sparkline(spark) : ""}</div>`;

// A table the page can sort (click a heading) and, with filter, search. Cells: [html, numeric?, class?, sortValue?].
const table = (head, rows, { filter } = {}) => `${filter ? `<input class="filter" type="search" placeholder="Filter ${filter}…" aria-label="Filter ${filter}">` : ""}
<div class="scroll"><table class="sortable"><thead><tr>${head.map(([label, n]) => `<th${n ? ' class="n"' : ""}>${label}</th>`).join("")}</tr></thead><tbody>
${rows.length ? rows.map((r) => `<tr>${r.map(([value, n, cls, sort]) => `<td class="${n ? "n " : ""}${cls ?? ""}"${sort !== undefined ? ` data-v="${escape(sort)}"` : ""}>${value}</td>`).join("")}</tr>`).join("\n")
        : `<tr><td class="muted" colspan="${head.length}">Nothing yet</td></tr>`}</tbody></table></div>`;

const charName = (c) => `<span class="cls" style="color:${CLASS_COLORS[c.class] || "inherit"}">${escape(c.name)}</span>`;
const factionTag = (race) => `<span class="tag ${faction(race).toLowerCase()}">${faction(race)}</span>`;

// ---- Page ----------------------------------------------------------------------------------------------------------

const STYLE = `
:root { color-scheme: dark; --bg: #0c0d10; --panel: #15171c; --panel2: #1b1e24; --line: #262a32; --ink: #e6e1d6; --ink2: #a39e93;
  --muted: #6f6b63; --gold: #e8c766; --gold2: #8a6d1f; --good: #6fcf7f; --alliance: #4a8fe7; --horde: #e5484d; }
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--ink); font: 14px/1.45 system-ui, -apple-system, Segoe UI, sans-serif; }
a { color: var(--gold); text-decoration: none; }
.shell { display: grid; grid-template-columns: 210px 1fr; min-height: 100vh; }
nav { position: sticky; top: 0; height: 100vh; padding: 20px 12px; background: #101216; border-right: 1px solid var(--line); overflow-y: auto; }
nav .brand { font: 600 20px Georgia, serif; color: var(--gold); padding: 0 10px 18px; }
nav .brand small { display: block; font: 11px system-ui, sans-serif; color: var(--muted); letter-spacing: .08em; text-transform: uppercase; }
nav a { display: block; padding: 7px 10px; border-radius: 6px; color: var(--ink2); }
nav a:hover, nav a.on { background: var(--panel2); color: var(--ink); }
main { padding: 0 24px 64px; min-width: 0; }
header.top { position: sticky; top: 0; z-index: 5; display: flex; align-items: center; gap: 16px; flex-wrap: wrap; padding: 14px 0;
  background: color-mix(in srgb, var(--bg) 88%, transparent); backdrop-filter: blur(8px); border-bottom: 1px solid var(--line); }
header.top h1 { margin: 0; font: 600 18px system-ui, sans-serif; } header.top .grow { flex: 1; }
.live { display: inline-flex; align-items: center; gap: 6px; color: var(--ink2); font-size: 12px; }
.live::before { content: ""; width: 8px; height: 8px; border-radius: 50%; background: var(--good); box-shadow: 0 0 0 0 var(--good); animation: pulse 2s infinite; }
.live.off::before { background: var(--horde); animation: none; }
@keyframes pulse { 0% { box-shadow: 0 0 0 0 rgba(111,207,127,.6); } 70% { box-shadow: 0 0 0 8px rgba(111,207,127,0); } 100% { box-shadow: 0 0 0 0 rgba(111,207,127,0); } }
.pill { font-size: 12px; color: var(--ink2); background: var(--panel); border: 1px solid var(--line); border-radius: 99px; padding: 3px 10px; }
section { scroll-margin-top: 70px; margin-top: 28px; }
h2 { margin: 0 0 12px; font: 600 13px system-ui, sans-serif; color: var(--gold); text-transform: uppercase; letter-spacing: .08em; }
h3 { margin: 0 0 10px; font: 600 13px system-ui, sans-serif; color: var(--ink2); }
.muted { color: var(--muted); font-size: 12px; }
.kpis { display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 10px; }
.kpi { position: relative; background: var(--panel); border: 1px solid var(--line); border-radius: 10px; padding: 12px 14px; overflow: hidden; }
.kpi span { display: block; color: var(--ink2); font-size: 12px; } .kpi b { display: block; font-size: 24px; font-weight: 600; margin-top: 2px; font-variant-numeric: tabular-nums; }
.kpi small { color: var(--muted); font-size: 11px; }
.spark { display: block; width: 100%; height: 24px; margin-top: 6px; } .spark polyline { fill: none; stroke: var(--gold); stroke-width: 2; vector-effect: non-scaling-stroke; }
.grid { display: grid; gap: 12px; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); }
.grid4 { grid-template-columns: repeat(auto-fill, minmax(230px, 1fr)); }
.card { background: var(--panel); border: 1px solid var(--line); border-radius: 10px; padding: 14px 16px; min-width: 0; }
.mini { margin: 0; background: var(--panel); border: 1px solid var(--line); border-radius: 10px; padding: 12px 14px; }
.mini figcaption { display: flex; justify-content: space-between; align-items: baseline; margin-bottom: 8px; }
.mini figcaption span { color: var(--ink2); font-size: 12px; } .mini figcaption b { font-size: 18px; }
.mini svg { display: block; width: 100%; height: auto; } .mini path { fill: var(--gold); } .mini .col:hover path { fill: #fff0b8; }
.mini text { fill: var(--muted); font-size: 10px; } .axis { stroke: var(--line); }
.hbars { display: grid; gap: 6px; }
.hbar { display: grid; grid-template-columns: minmax(90px, 34%) 1fr auto; align-items: center; gap: 10px; font-size: 13px; }
.hbar .label { display: flex; align-items: center; gap: 6px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.hbar i { width: 8px; height: 8px; border-radius: 2px; flex: none; }
.hbar .track { height: 10px; background: var(--panel2); border-radius: 0 4px 4px 0; }
.hbar .fill { display: block; height: 100%; background: var(--gold); border-radius: 0 4px 4px 0; min-width: 2px; }
.hbar .value { font-variant-numeric: tabular-nums; text-align: right; } .hbar small { color: var(--muted); }
.split { display: flex; height: 26px; border-radius: 6px; overflow: hidden; gap: 2px; margin: 4px 0 8px; }
.split div { display: flex; align-items: center; padding: 0 10px; font-size: 12px; font-weight: 600; color: #fff; min-width: 2px; white-space: nowrap; }
.funnel .hbar .fill { background: linear-gradient(90deg, var(--gold2), var(--gold)); }
.scroll { overflow: auto; max-height: 520px; border: 1px solid var(--line); border-radius: 10px; }
table { width: 100%; border-collapse: collapse; font-variant-numeric: tabular-nums; }
th, td { text-align: left; padding: 7px 10px; border-bottom: 1px solid var(--line); white-space: nowrap; }
thead th { position: sticky; top: 0; background: var(--panel2); color: var(--ink2); font-weight: 500; font-size: 12px; cursor: pointer; user-select: none; }
thead th:hover { color: var(--ink); } th[aria-sort=ascending]::after { content: " ▲"; } th[aria-sort=descending]::after { content: " ▼"; }
tbody tr:hover { background: var(--panel); } td.n, th.n { text-align: right; }
.online { color: var(--good); } .tag { font-size: 11px; padding: 1px 7px; border-radius: 99px; border: 1px solid; }
.tag.alliance { color: var(--alliance); } .tag.horde { color: var(--horde); }
.filter { width: 100%; max-width: 320px; margin-bottom: 8px; }
input, button { font: inherit; padding: 7px 11px; border-radius: 6px; border: 1px solid var(--line); background: var(--panel); color: inherit; }
button { background: var(--gold2); border-color: var(--gold2); color: #fff; cursor: pointer; }
.map-wrap { display: grid; grid-template-columns: minmax(0, 1fr) 300px; gap: 12px; }
.tabs { display: flex; gap: 4px; flex-wrap: wrap; margin-bottom: 10px; }
.tabs button { background: var(--panel); border-color: var(--line); color: var(--ink2); padding: 5px 12px; }
.tabs button.on { background: var(--panel2); color: var(--gold); border-color: var(--gold2); }
.map { position: relative; aspect-ratio: 1002 / 668; border-radius: 10px; overflow: hidden; border: 1px solid var(--line); background: #1a1712 center / 100% 100%; }
.map .dot { position: absolute; width: 11px; height: 11px; margin: -5.5px 0 0 -5.5px; border-radius: 50%; border: 2px solid #0c0d10; cursor: pointer; transition: left 1s linear, top 1s linear; }
.map .dot.alliance { background: var(--alliance); } .map .dot.horde { background: var(--horde); }
.map .dot::after { content: ""; position: absolute; inset: -6px; border-radius: 50%; border: 2px solid currentColor; opacity: 0; animation: ping 2.4s infinite; }
.map .dot.alliance { color: var(--alliance); } .map .dot.horde { color: var(--horde); }
@keyframes ping { 0% { transform: scale(.4); opacity: .8; } 100% { transform: scale(1.6); opacity: 0; } }
.map .empty { position: absolute; inset: 0; display: grid; place-items: center; color: var(--ink2); text-shadow: 0 1px 2px #000; }
.map .empty[hidden] { display: none; }
.legend { display: flex; gap: 14px; font-size: 12px; color: var(--ink2); margin-top: 8px; }
.legend i { display: inline-block; width: 9px; height: 9px; border-radius: 50%; margin-right: 5px; vertical-align: -1px; }
#tip { position: fixed; z-index: 20; pointer-events: none; background: #0b0c0f; border: 1px solid var(--line); border-radius: 8px; padding: 8px 10px;
  font-size: 12px; box-shadow: 0 8px 24px rgba(0,0,0,.5); display: none; }
#tip b { display: block; font-size: 13px; }
.who { max-height: 470px; overflow: auto; }
.who .row { display: flex; justify-content: space-between; gap: 8px; padding: 6px 4px; border-bottom: 1px solid var(--line); font-size: 13px; cursor: default; }
.who .row:hover { background: var(--panel2); } .who .row small { color: var(--muted); display: block; }
.login { max-width: 360px; margin: 18vh auto; background: var(--panel); border: 1px solid var(--line); border-radius: 12px; padding: 28px; }
.login h1 { font: 600 22px Georgia, serif; color: var(--gold); margin: 0 0 16px; } .login form { display: grid; gap: 10px; }
@media (max-width: 900px) {
  .shell { grid-template-columns: 1fr; } .map-wrap { grid-template-columns: 1fr; }
  nav { position: sticky; z-index: 6; height: auto; display: flex; gap: 4px; overflow-x: auto; padding: 8px 12px; border-right: 0; border-bottom: 1px solid var(--line); }
  nav .brand { display: none; } nav a { white-space: nowrap; } main { padding: 0 16px 48px; } header.top { top: 49px; }
}`;

const page = (title, body) => `<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><meta name="robots" content="noindex">
<link rel="icon" href="/favicon.svg" type="image/svg+xml"><title>${escape(title)}</title><style>${STYLE}</style></head><body>${body}</body></html>`;

const loginPage = (message) => page("Lasthavens admin", `<div class="login"><h1>Lasthavens admin</h1>
<form method="post" action="/admin"><input type="password" name="password" placeholder="Admin password" autofocus required>
<button>Sign in</button></form>${message ? `<p class="muted">${escape(message)}</p>` : ""}</div>`);

// Sorting, filtering, the nav highlight and the live map. Data arrives from GET /admin?live every 10 seconds.
const SCRIPT = `
for (const t of document.querySelectorAll("table.sortable")) {
  t.tHead.addEventListener("click", (e) => {
    const th = e.target.closest("th"); if (!th) return;
    const i = th.cellIndex, asc = th.getAttribute("aria-sort") !== "ascending", n = th.classList.contains("n");
    for (const h of th.parentNode.children) h.removeAttribute("aria-sort");
    th.setAttribute("aria-sort", asc ? "ascending" : "descending");
    const val = (r) => { const c = r.cells[i]; const v = c?.dataset.v ?? c?.textContent ?? ""; return n || c?.dataset.v ? parseFloat(String(v).replace(/[^0-9.-]/g, "")) || 0 : v.toLowerCase(); };
    const rows = [...t.tBodies[0].rows].sort((a, b) => (val(a) > val(b) ? 1 : val(a) < val(b) ? -1 : 0) * (asc ? 1 : -1));
    t.tBodies[0].append(...rows);
  });
}
for (const f of document.querySelectorAll("input.filter")) {
  const rows = f.nextElementSibling.querySelectorAll("tbody tr");
  f.addEventListener("input", () => { const q = f.value.toLowerCase(); for (const r of rows) r.hidden = !r.textContent.toLowerCase().includes(q); });
}
const links = [...document.querySelectorAll("nav a")];
const io = new IntersectionObserver((es) => { for (const e of es) if (e.isIntersecting) for (const a of links) a.classList.toggle("on", a.hash === "#" + e.target.id); },
  { rootMargin: "-40% 0px -55% 0px" });
document.querySelectorAll("main section").forEach((s) => io.observe(s));

const WORLD = JSON.parse(document.getElementById("worldData").textContent);
const COLORS = ${JSON.stringify(CLASS_COLORS)}, CLASSES = ${JSON.stringify(CLASSES)}, RACES = ${JSON.stringify(RACES)}, HORDE = [2, 5, 6, 8, 10];
const $ = (id) => document.getElementById(id), tip = $("tip");
let players = [], shown = WORLD.continents[0]?.map;
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; };
const side = (p) => (HORDE.includes(p.race) ? "horde" : "alliance");
${mapPoint}
function showTip(e, p) {
  tip.replaceChildren(el("b", "", p.name), el("div", "", "Level " + p.level + " " + RACES[p.race] + " " + CLASSES[p.class]), el("div", "muted", p.zoneName + " · " + p.account));
  tip.firstChild.style.color = COLORS[p.class] || ""; tip.style.display = "block";
  tip.style.left = Math.min(e.clientX + 14, innerWidth - tip.offsetWidth - 8) + "px"; tip.style.top = (e.clientY + 14) + "px";
}
function render() {
  const tabs = $("tabs"), map = $("map");
  if (tabs) {
    tabs.replaceChildren(...WORLD.continents.map((c) => {
      const b = el("button", c.map === shown ? "on" : "", c.title + " (" + players.filter((p) => p.map === c.map).length + ")");
      b.onclick = () => { shown = c.map; render(); }; return b;
    }));
    const c = WORLD.continents.find((c) => c.map === shown);
    map.style.backgroundImage = "url(/" + c.file + ")";
    const keep = new Map([...map.querySelectorAll(".dot")].map((d) => [d.dataset.name, d]));
    for (const p of players.filter((p) => p.map === c.map)) {
      const at = mapPoint(c, p.x, p.y); if (!at) continue;
      const d = keep.get(p.name) ?? map.appendChild(el("div")); keep.delete(p.name);
      d.className = "dot " + side(p); d.dataset.name = p.name; d.style.left = at[0] * 100 + "%"; d.style.top = at[1] * 100 + "%";
      d.onmousemove = (e) => showTip(e, p); d.onmouseleave = () => (tip.style.display = "none");
    }
    for (const d of keep.values()) d.remove();
    $("mapEmpty").hidden = players.some((p) => p.map === c.map);
  }
  const who = $("who");
  who.replaceChildren(...(players.length ? players : [null]).map((p) => {
    if (!p) return el("p", "muted", "Nobody is online.");
    const r = el("div", "row"), l = el("div"), n = el("span", "", p.name);
    n.style.color = COLORS[p.class] || ""; l.append(n, el("small", "", p.zoneName + (WORLD.continents.some((c) => c.map === p.map) ? "" : " (instance)")));
    r.append(l, el("span", "muted", p.level + " " + CLASSES[p.class]));
    r.onmousemove = (e) => showTip(e, p); r.onmouseleave = () => (tip.style.display = "none");
    r.onclick = () => { if (WORLD.continents.some((c) => c.map === p.map)) { shown = p.map; render(); } };
    return r;
  }));
  $("onlineNow").querySelector("b").textContent = players.length.toLocaleString("en-US");
  const a = players.filter((p) => side(p) === "alliance").length;
  $("whoCount").textContent = players.length + " online · " + a + " Alliance · " + (players.length - a) + " Horde";
}
async function tick() {
  try {
    const r = await fetch("/admin?live", { cache: "no-store", credentials: "same-origin" });
    if (r.status !== 200) throw new Error(r.status);
    players = (await r.json()).players;
    if (!players.some((p) => p.map === shown)) shown = (WORLD.continents.find((c) => players.some((p) => p.map === c.map)) ?? WORLD.continents[0])?.map;
    render();
    $("live").className = "live"; $("live").textContent = "Live · " + new Date().toLocaleTimeString();
  } catch { $("live").className = "live off"; $("live").textContent = "Live view paused"; }
}
tick(); setInterval(tick, 10000);
`;

export function createAdmin({ databaseUrl, password, secret, webDirs = [], pool }) {
    secret ||= randomBytes(32).toString("hex");
    const db = async () => {
        if (!pool) {
            const mysql = await import("mysql2/promise");
            pool = mysql.createPool({ uri: databaseUrl, connectionLimit: 2 });
        }
        return pool;
    };

    // worldmap.json from extract-art.mjs: continent bounds and area names. Read again until it exists.
    let world = null;
    const worldData = () => {
        for (const dir of world ? [] : webDirs) {
            try {
                const data = JSON.parse(readFileSync(join(dir, "worldmap.json"), "utf8"));
                data.continents.sort((a, b) => a.map - b.map).forEach((c) => (c.title = CONTINENT_NAMES[c.name] ?? c.name));
                world = data;
                break;
            } catch {}
        }
        return world ?? { continents: [], zones: {} };
    };
    const zoneName = (id) => worldData().zones[id] ?? (Number(id) ? `Zone ${id}` : "Unknown");

    // ponytail: in-memory per-address limits, as account.mjs.
    const hits = new Map();
    const limited = (key, max, windowMs) => {
        const now = Date.now();
        const recent = (hits.get(key) ?? []).filter((t) => now - t < windowMs);
        recent.push(now);
        hits.set(key, recent);
        return recent.length > max;
    };

    async function record(event, detail = "", ms = 0) {
        if (!databaseUrl || !EVENTS[event]?.includes(detail)) return;
        try {
            await (await db()).execute(
                "INSERT INTO lasthavens.web_events (day, event, detail, count, total_ms) VALUES (UTC_DATE(), ?, ?, 1, ?) "
                + "ON DUPLICATE KEY UPDATE count = count + 1, total_ms = total_ms + VALUES(total_ms)", [event, detail, ms]);
        } catch (error) {
            console.error("stats:", error.message);
        }
    }

    const sign = (expires) => createHmac("sha256", secret).update(`admin:${expires}`).digest("hex");
    const signedIn = (req) => {
        const m = /(?:^|;\s*)wowadmin=(\d+)\.([0-9a-f]{64})(?:;|$)/.exec(req.headers.cookie ?? "");
        return !!m && Number(m[1]) > Date.now() && timingSafeEqual(Buffer.from(sign(m[1])), Buffer.from(m[2]));
    };
    const samePassword = (given) => timingSafeEqual(createHash("sha256").update(given).digest(), createHash("sha256").update(password).digest());

    async function live() {
        const [rows] = await (await db()).query(`SELECT c.name, c.level, c.race, c.class, c.map, c.zone, c.position_x AS x, c.position_y AS y,
            a.username FROM acore_characters.characters c JOIN acore_auth.account a ON a.id = c.account WHERE c.online = 1 ORDER BY c.level DESC, c.name`);
        return {
            players: rows.map((r) => ({ name: r.name, level: r.level, race: r.race, class: r.class, map: r.map, x: r.x, y: r.y,
                zoneName: zoneName(r.zone), account: r.username })),
        };
    }

    async function dashboard() {
        const p = await db();
        // One failed query (a table this realm lacks) blanks its panel, not the page.
        const failed = [];
        const all = async (sql) => {
            try {
                return (await p.query(sql))[0];
            } catch (error) {
                failed.push(error.message);
                return [];
            }
        };
        const one = async (sql) => (await all(sql))[0] ?? {};
        const C = "acore_characters.characters";

        const [accounts, chars, events, daily, signupsByDay, uptime, bans, auctions, quests, levels, demo, zones,
            richest, killers, achievers, guilds, signups, top] = await Promise.all([
            one(`SELECT COUNT(*) AS total, SUM(joindate >= UTC_DATE()) AS today, SUM(joindate >= UTC_DATE() - INTERVAL 6 DAY) AS week,
                SUM(online > 0) AS online, SUM(last_login >= UTC_TIMESTAMP() - INTERVAL 7 DAY) AS active7,
                SUM(last_login >= UTC_TIMESTAMP() - INTERVAL 30 DAY) AS active30 FROM acore_auth.account`),
            one(`SELECT COUNT(*) AS total, SUM(online) AS online, COUNT(DISTINCT account) AS accounts,
                SUM(online OR logout_time >= UNIX_TIMESTAMP() - 86400) AS day1, SUM(online OR logout_time >= UNIX_TIMESTAMP() - 7 * 86400) AS day7,
                SUM(online OR logout_time >= UNIX_TIMESTAMP() - 30 * 86400) AS day30, SUM(totaltime) AS played, SUM(money) AS money,
                SUM(level >= 80) AS maxed, AVG(level) AS avgLevel, SUM(totalKills) AS kills FROM ${C}`),
            all(`SELECT event, detail, SUM(count) AS count, SUM(total_ms) AS ms FROM lasthavens.web_events
                WHERE day >= UTC_DATE() - INTERVAL 6 DAY GROUP BY event, detail`),
            // Days come back as text: the driver would turn DATE into a local-midnight Date, a day off in UTC.
            all(`SELECT DATE_FORMAT(day, '%Y-%m-%d') AS day, event, SUM(count) AS count, SUM(total_ms) AS ms FROM lasthavens.web_events
                WHERE day >= UTC_DATE() - INTERVAL 13 DAY GROUP BY day, event`),
            all(`SELECT DATE_FORMAT(joindate, '%Y-%m-%d') AS day, COUNT(*) AS count FROM acore_auth.account
                WHERE joindate >= UTC_DATE() - INTERVAL 13 DAY GROUP BY day`),
            one(`SELECT (SELECT uptime FROM acore_auth.uptime ORDER BY starttime DESC LIMIT 1) AS current,
                (SELECT maxplayers FROM acore_auth.uptime ORDER BY starttime DESC LIMIT 1) AS peakNow, MAX(maxplayers) AS peak,
                SUM(starttime >= UNIX_TIMESTAMP() - 14 * 86400) AS restarts FROM acore_auth.uptime`),
            one("SELECT COUNT(*) AS active FROM acore_auth.account_banned WHERE active = 1"),
            one("SELECT COUNT(*) AS count, COALESCE(SUM(buyoutprice), 0) AS buyout FROM acore_characters.auctionhouse"),
            one("SELECT COUNT(*) AS count FROM acore_characters.character_queststatus_rewarded"),
            all(`SELECT IF(level >= 80, 8, level DIV 10) AS bucket, COUNT(*) AS count FROM ${C} GROUP BY bucket`),
            all(`SELECT race, class, gender, COUNT(*) AS count FROM ${C} GROUP BY race, class, gender`),
            all(`SELECT zone, COUNT(*) AS count, SUM(online) AS online FROM ${C} GROUP BY zone ORDER BY count DESC LIMIT 12`),
            all(`SELECT name, level, race, class, money FROM ${C} ORDER BY money DESC LIMIT 10`),
            all(`SELECT name, level, race, class, totalKills FROM ${C} WHERE totalKills > 0 ORDER BY totalKills DESC LIMIT 10`),
            all(`SELECT c.name, c.level, c.race, c.class, COUNT(*) AS count FROM acore_characters.character_achievement a
                JOIN ${C} c ON c.guid = a.guid GROUP BY a.guid ORDER BY count DESC LIMIT 10`),
            all(`SELECT g.name, l.name AS leader, l.race, COUNT(m.guid) AS members, COALESCE(MAX(c.level), 0) AS topLevel,
                COALESCE(SUM(c.online), 0) AS online, g.BankMoney AS bank FROM acore_characters.guild g
                LEFT JOIN acore_characters.guild_member m ON m.guildid = g.guildid LEFT JOIN ${C} c ON c.guid = m.guid
                LEFT JOIN ${C} l ON l.guid = g.leaderguid GROUP BY g.guildid ORDER BY members DESC LIMIT 15`),
            all(`SELECT a.username, UNIX_TIMESTAMP(a.joindate) AS joined, UNIX_TIMESTAMP(a.last_login) AS lastLogin, a.online, a.expansion,
                COUNT(c.guid) AS characters, COALESCE(MAX(c.level), 0) AS topLevel, COALESCE(SUM(c.totaltime), 0) AS played
                FROM acore_auth.account a LEFT JOIN ${C} c ON c.account = a.id GROUP BY a.id ORDER BY a.joindate DESC LIMIT 200`),
            all(`SELECT c.name, c.level, c.race, c.class, c.gender, c.zone, c.money, c.totaltime, c.logout_time, c.online, c.totalKills,
                a.username FROM ${C} c JOIN acore_auth.account a ON a.id = c.account ORDER BY c.level DESC, c.totaltime DESC LIMIT 500`),
        ]);

        const week = (event) => events.filter((e) => e.event === event).reduce((n, e) => n + Number(e.count), 0);
        const loaded = events.find((e) => e.event === "loaded");
        const avgLoad = loaded && Number(loaded.count) ? `${(Number(loaded.ms) / Number(loaded.count) / 1000).toFixed(1)} s` : "-";
        const days = [...Array(14)].map((_, i) => new Date(Date.now() - (13 - i) * 86400_000).toISOString().slice(0, 10));
        const series = (event) => days.map((day) => Number(daily.find((r) => r.day === day && r.event === event)?.count ?? 0));
        const signupSeries = days.map((day) => Number(signupsByDay.find((s) => s.day === day)?.count ?? 0));
        const loadSeries = days.map((day) => {
            const r = daily.find((r) => r.day === day && r.event === "loaded");
            return r && Number(r.count) ? Number(r.ms) / Number(r.count) / 1000 : 0;
        });

        // Demographics from one race x class x gender count.
        const sum = (pick) => {
            const out = new Map();
            for (const d of demo) out.set(pick(d), (out.get(pick(d)) ?? 0) + Number(d.count));
            return [...out].sort((a, b) => b[1] - a[1]);
        };
        const byClass = sum((d) => d.class), byRace = sum((d) => d.race), byFaction = new Map(sum((d) => faction(d.race)));
        const byGender = new Map(sum((d) => (Number(d.gender) ? "Female" : "Male")));
        const alliance = byFaction.get("Alliance") ?? 0, horde = byFaction.get("Horde") ?? 0;
        const LEVEL_BUCKETS = ["1-9", "10-19", "20-29", "30-39", "40-49", "50-59", "60-69", "70-79", "80"];

        const w = worldData();
        const worldJson = JSON.stringify({ continents: w.continents }).replace(/</g, "\\u003c");
        const leader = (rows, value, format = num) => table([["#", 1], ["Character"], ["Level", 1], [value[0], 1]],
            rows.map((c, i) => [[i + 1, 1], [charName(c)], [c.level, 1], [format(c[value[1]]), 1, "", c[value[1]]]]));

        const nav = [["overview", "Overview"], ["live", "Live map"], ["traffic", "Traffic"], ["players", "Players"], ["world", "World"],
            ["economy", "Economy"], ["leaders", "Leaderboards"], ["guilds", "Guilds"], ["accounts", "Accounts"], ["characters", "Characters"], ["website", "Website"]];

        return page("Lasthavens admin", `<div class="shell">
<nav><div class="brand">Lasthavens<small>Realm admin</small></div>${nav.map(([id, label]) => `<a href="#${id}">${label}</a>`).join("")}</nav>
<main>
<header class="top"><h1>Dashboard</h1><span id="live" class="live">Live</span><span class="grow"></span>
<span class="pill">Uptime ${uptime.current ? duration(uptime.current) : "-"}</span><span class="pill">${new Date().toISOString().slice(0, 16).replace("T", " ")} UTC</span>
<a class="pill" href="/admin?logout">Sign out</a></header>
${failed.length ? `<p class="muted">Some panels are empty: ${escape([...new Set(failed)].join("; "))}</p>` : ""}

<section id="overview"><h2>Overview</h2><div class="kpis">
${kpi("Online now", num(chars.online), `peak ${num(uptime.peakNow)} this uptime · ${num(uptime.peak)} all-time`, null, "onlineNow")}
${kpi("Accounts", num(accounts.total), `+${num(accounts.today)} today · +${num(accounts.week)} this week`, signupSeries)}
${kpi("Characters", num(chars.total), `${num(chars.maxed)} at level 80 · avg level ${Number(chars.avgLevel ?? 0).toFixed(1)}`)}
${kpi("Active players", num(chars.day7), `${num(chars.day1)} in 24h · ${num(chars.day30)} in 30d`)}
${kpi("World entries, 7 days", num(week("world_connection")), `${num(week("login_connection"))} logins`, series("world_connection"))}
${kpi("Page opens, 7 days", num(week("visit")), `${num(week("new_browser"))} new visitors`, series("visit"))}
${kpi("Load success, 7 days", pct(week("loaded"), week("loaded") + week("load_failed")), `${num(week("load_failed"))} failed · avg ${avgLoad}`, series("loaded"))}
${kpi("Time played", hours(chars.played), `${num(chars.accounts)} accounts with characters`)}
${kpi("Gold in circulation", gold(chars.money), `${num(auctions.count)} auctions · ${gold(auctions.buyout)} buyout`)}
${kpi("Quests completed", num(quests.count), `${num(chars.kills)} honorable kills`)}
${kpi("Retention", pct(accounts.active7, accounts.total), `of accounts logged in this week · ${pct(accounts.active30, accounts.total)} in 30d`)}
${kpi("Server", uptime.current ? duration(uptime.current) : "-", `${num(uptime.restarts)} restarts in 14 days · ${num(bans.active)} active bans`)}
</div></section>

<section id="live"><h2>Live map</h2><div class="map-wrap"><div class="card">
${w.continents.length ? `<div class="tabs" id="tabs"></div><div class="map" id="map"><div class="empty" id="mapEmpty">Nobody on this continent</div></div>
<div class="legend"><span><i style="background:var(--alliance)"></i>Alliance</span><span><i style="background:var(--horde)"></i>Horde</span><span>Updates every 10 s · hover a dot for details</span></div>`
        : `<p class="muted">No world maps yet: run <code>node Tools/Bridge/extract-art.mjs --out &lt;web dir&gt;</code> for the map. Players are listed alongside.</p>`}
</div><div class="card"><h3 id="whoCount">Online</h3><div class="who" id="who"></div></div></div></section>

<section id="traffic"><h2>Traffic, last 14 days</h2><div class="grid grid4">
${columns("Page opens", days, series("visit"))}${columns("New visitors", days, series("new_browser"))}
${columns("Reached login screen", days, series("loaded"))}${columns("Accounts created", days, signupSeries)}
${columns("Logins", days, series("login_connection"))}${columns("World entries", days, series("world_connection"))}
${columns("Average load time", days, loadSeries, (v) => (v ? `${Number(v).toFixed(1)} s` : "-"), true)}${columns("Failed loads", days, series("load_failed"))}
</div>
<div class="grid" style="margin-top:12px"><div class="card funnel"><h3>Funnel, 7 days</h3>
${hbars([["Opened the page", week("visit")], ["Reached login", week("loaded")], ["Created an account", week("account_created")], ["Entered the world", week("world_connection")]], num, week("visit"))}
<p class="muted">Percentages are of page opens; logins and world entries count every session, not people.</p></div></div></section>

<section id="players"><h2>Players</h2><div class="grid">
<div class="card"><h3>Faction balance</h3><div class="split">
<div style="flex:${alliance || 1};background:var(--alliance)">Alliance ${pct(alliance, alliance + horde)}</div>
<div style="flex:${horde || (alliance ? 0 : 1)};background:var(--horde)">Horde ${pct(horde, alliance + horde)}</div></div>
<p class="muted">${num(alliance)} Alliance · ${num(horde)} Horde characters</p>
<h3 style="margin-top:14px">Gender</h3>${hbars([["Male", byGender.get("Male") ?? 0], ["Female", byGender.get("Female") ?? 0]])}
<h3 style="margin-top:14px">Activity</h3>${hbars([["Played in 24h", chars.day1], ["Played in 7 days", chars.day7], ["Played in 30 days", chars.day30], ["All characters", chars.total]])}</div>
<div class="card"><h3>Classes</h3>${hbars(byClass.map(([c, n]) => [CLASSES[c] ?? `Class ${c}`, n, CLASS_COLORS[c]]))}</div>
<div class="card"><h3>Races</h3>${hbars(byRace.map(([r, n]) => [RACES[r] ?? `Race ${r}`, n, HORDE.has(Number(r)) ? "var(--horde)" : "var(--alliance)"]))}</div>
<div class="card"><h3>Level distribution</h3>${hbars(LEVEL_BUCKETS.map((label, i) => [label, Number(levels.find((l) => Number(l.bucket) === i)?.count ?? 0)]))}</div>
</div></section>

<section id="world"><h2>World</h2><div class="grid"><div class="card"><h3>Where characters are (last known zone)</h3>
${hbars(zones.map((z) => [zoneName(z.zone), z.count]))}</div>
<div class="card"><h3>Zones by players online</h3>${hbars(zones.filter((z) => Number(z.online)).map((z) => [zoneName(z.zone), z.online]))}</div></div></section>

<section id="economy"><h2>Economy</h2><div class="grid">
<div class="card"><h3>Richest characters</h3>${leader(richest, ["Gold", "money"], gold)}</div>
<div class="card"><h3>Totals</h3>${hbars([["Character gold", Math.floor(Number(chars.money ?? 0) / 10000)], ["Auction buyouts", Math.floor(Number(auctions.buyout) / 10000)],
            ["Guild banks", Math.floor(guilds.reduce((n, g) => n + Number(g.bank ?? 0), 0) / 10000)]], (v) => `${num(v)}g`)}
<p class="muted">Average ${gold(Number(chars.money ?? 0) / Math.max(1, Number(chars.total)))} per character.</p></div></div></section>

<section id="leaders"><h2>Leaderboards</h2><div class="grid">
<div class="card"><h3>Most achievements</h3>${leader(achievers, ["Achievements", "count"])}</div>
<div class="card"><h3>Most honorable kills</h3>${leader(killers, ["Kills", "totalKills"])}</div></div></section>

<section id="guilds"><h2>Guilds</h2>${table([["Guild"], ["Leader"], ["Faction"], ["Members", 1], ["Online", 1], ["Top level", 1], ["Bank", 1]],
            guilds.map((g) => [[escape(g.name)], [escape(g.leader ?? "-")], [g.race ? factionTag(g.race) : "-"], [g.members, 1], [g.online, 1, Number(g.online) ? "online" : ""],
                [g.topLevel, 1], [gold(g.bank), 1, "", g.bank]]))}</section>

<section id="accounts"><h2>Accounts</h2>${table([["Account"], ["Created"], ["Last login"], ["Expansion"], ["Characters", 1], ["Top level", 1], ["Played", 1]],
            signups.map((a) => [[escape(a.username), 0, a.online ? "online" : ""], [date(a.joined, true)], [date(a.lastLogin, true)],
                [["Classic", "TBC", "WotLK"][a.expansion] ?? a.expansion], [a.characters, 1], [a.topLevel, 1], [hours(a.played), 1, "", a.played]]), { filter: "accounts" })}</section>

<section id="characters"><h2>Characters</h2>${table([["Character"], ["Level", 1], ["Race"], ["Class"], ["Faction"], ["Zone"], ["Gold", 1], ["Kills", 1], ["Played", 1], ["Last seen"], ["Account"]],
            top.map((c) => [[charName(c), 0, ""], [c.level, 1], [RACES[c.race] ?? c.race], [CLASSES[c.class] ?? c.class], [factionTag(c.race)], [escape(zoneName(c.zone))],
                [gold(c.money), 1, "", c.money], [num(c.totalKills), 1], [hours(c.totaltime), 1, "", c.totaltime],
                [c.online ? "online now" : date(c.logout_time, true), 0, c.online ? "online" : "", c.online ? "9999999999" : c.logout_time], [escape(c.username)]]), { filter: "characters" })}</section>

<section id="website"><h2>Website, 7 days</h2><div class="grid">
<div class="card"><h3>Browsers</h3>${hbars(events.filter((e) => e.event === "visit").sort((a, b) => b.count - a.count).map((e) => [e.detail, e.count]))}</div>
<div class="card"><h3>Failed load reasons</h3>${hbars(events.filter((e) => e.event === "load_failed").sort((a, b) => b.count - a.count).map((e) => [e.detail, e.count]))}</div></div>
<h3 style="margin-top:16px">By day</h3>
${table([["Day"], ["Page opens", 1], ["New visitors", 1], ["Reached login", 1], ["Avg load", 1], ["Failed loads", 1], ["New accounts", 1], ["Logins", 1], ["World entries", 1]],
            [...days].reverse().map((day, r) => {
                const i = 13 - r;
                return [[day], [series("visit")[i], 1], [series("new_browser")[i], 1], [series("loaded")[i], 1],
                    [loadSeries[i] ? `${loadSeries[i].toFixed(1)} s` : "-", 1], [series("load_failed")[i], 1], [signupSeries[i], 1],
                    [series("login_connection")[i], 1], [series("world_connection")[i], 1]];
            }))}
<p class="muted">Website counts are anonymous per-day totals; realm numbers are live from the database. Times are UTC.</p></section>
</main></div><div id="tip"></div>
<script type="application/json" id="worldData">${worldJson}</script><script>${SCRIPT}</script>`);
    }

    async function readBody(req, limit) {
        let body = "";
        for await (const chunk of req) {
            body += chunk;
            if (body.length > limit) return null;
        }
        return body;
    }

    // Handles /admin and /stats/event; false for anything else.
    async function handle(req, res, path, address) {
        const send = (status, html, headers = {}) => res.writeHead(status, {
            "Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store", "X-Frame-Options": "DENY",
            "Cross-Origin-Resource-Policy": "same-origin", ...headers,
        }).end(html);

        if (path === "/stats/event") {
            if (req.method === "POST" && !limited(`stats:${address}`, 30, 60_000)) {
                const body = await readBody(req, 512);
                let parsed = null;
                try {
                    parsed = pageEvent(JSON.parse(body ?? ""));
                } catch {}
                if (parsed) await record(parsed.event, parsed.detail, parsed.ms);
            }
            res.writeHead(204, { "Cache-Control": "no-store" }).end();
            return true;
        }

        if (path !== "/admin") return false;
        if (!password || !databaseUrl) return send(404, page("Not found", "<p>Not found.</p>")), true;

        if (req.method === "POST") {
            const form = new URLSearchParams((await readBody(req, 1024)) ?? "");
            if (limited(`admin:${address}`, 5, 600_000)) return send(429, loginPage("Too many attempts. Try again in a few minutes.")), true;
            if (!samePassword(form.get("password") ?? "")) return send(401, loginPage("Wrong password.")), true;
            const expires = Date.now() + 12 * 3600_000;
            return send(303, "", { Location: "/admin", "Set-Cookie": `wowadmin=${expires}.${sign(expires)}; Path=/admin; Max-Age=43200; HttpOnly; Secure; SameSite=Strict` }), true;
        }

        const query = new URL(req.url, "http://x").searchParams;
        if (query.has("logout")) return send(303, "", { Location: "/admin", "Set-Cookie": "wowadmin=; Path=/admin; Max-Age=0; HttpOnly; Secure; SameSite=Strict" }), true;
        if (!signedIn(req)) return query.has("live") ? send(401, "") : send(200, loginPage("")), true;
        try {
            if (query.has("live")) {
                res.writeHead(200, { "Content-Type": "application/json", "Cache-Control": "no-store", "Cross-Origin-Resource-Policy": "same-origin" })
                    .end(JSON.stringify(await live()));
            } else {
                send(200, await dashboard());
            }
        } catch (error) {
            console.error("admin:", error.message);
            send(500, page("Lasthavens admin", `<div class="login"><h1>Lasthavens admin</h1><p>The dashboard query failed: ${escape(error.message)}</p></div>`));
        }
        return true;
    }

    return { handle, record };
}
