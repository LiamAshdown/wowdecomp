// The realm's admin page (/admin) and the anonymous usage counts behind it.
//
// Counts are per day, per event and per detail (lasthavens.web_events; setup in Tools/Deploy/setup.sql): no addresses,
// no ids. The bridge records the server-side events (a browser's first visit, an account created, a connection to the
// login or world server); the page records its own (opened, loaded and how long it took, failed and why) with
// POST /stats/event. The dashboard adds live numbers from the AzerothCore accounts and characters.
//
// /admin asks for WOW_BRIDGE_ADMIN_PASSWORD (bridge.env or the environment; unset = no admin page) and keeps the
// session in a signed cookie for 12 hours.

import { createHash, createHmac, randomBytes, timingSafeEqual } from "node:crypto";

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

const RACES = ["", "Human", "Orc", "Dwarf", "Night Elf", "Undead", "Tauren", "Gnome", "Troll", "", "Blood Elf", "Draenei"];
const CLASSES = ["", "Warrior", "Paladin", "Hunter", "Rogue", "Priest", "Death Knight", "Shaman", "Mage", "Warlock", "", "Druid"];
const escape = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);
const hours = (seconds) => `${(Number(seconds ?? 0) / 3600).toFixed(1)} h`;
const date = (epoch, withTime) => {
    if (!epoch) return "never";
    const iso = new Date(Number(epoch) * 1000).toISOString();
    return withTime ? `${iso.slice(0, 10)} ${iso.slice(11, 16)}` : iso.slice(0, 10);
};

export function createAdmin({ databaseUrl, password, secret }) {
    secret ||= randomBytes(32).toString("hex");
    let pool;
    const db = async () => {
        if (!pool) {
            const mysql = await import("mysql2/promise");
            pool = mysql.createPool({ uri: databaseUrl, connectionLimit: 2 });
        }
        return pool;
    };

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

    const page = (title, body) => `<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><meta name="robots" content="noindex">
<title>${escape(title)}</title><style>
:root { color-scheme: dark; }
body { margin: 0; padding: 24px 16px 48px; background: #101114; color: #d8d4cb; font: 14px/1.45 system-ui, sans-serif; }
main { max-width: 1100px; margin: 0 auto; }
h1 { margin: 0 0 4px; font: 600 22px Georgia, serif; color: #e8c766; } h2 { margin: 28px 0 8px; font-size: 15px; color: #e8c766; }
.muted { color: #8a867d; font-size: 12px; }
.cards { display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 10px; margin-top: 16px; }
.card { background: #181a1f; border: 1px solid #262930; border-radius: 6px; padding: 10px 12px; }
.card b { display: block; font-size: 22px; color: #f1ead8; } .card span { color: #8a867d; font-size: 12px; }
.scroll { overflow-x: auto; } table { width: 100%; border-collapse: collapse; font-variant-numeric: tabular-nums; }
th, td { text-align: left; padding: 5px 8px; border-bottom: 1px solid #23262c; white-space: nowrap; }
th { color: #8a867d; font-weight: 500; } td.n, th.n { text-align: right; }
.online { color: #7fd17f; } form { display: flex; gap: 8px; margin-top: 16px; }
input, button { font: inherit; padding: 6px 10px; border-radius: 4px; border: 1px solid #30333b; background: #181a1f; color: inherit; }
button { background: #8a6d1f; border-color: #8a6d1f; color: #fff; cursor: pointer; }
</style></head><body><main>${body}</main></body></html>`;

    const loginPage = (message) => page("Lasthavens admin", `<h1>Lasthavens admin</h1>
<form method="post" action="/admin"><input type="password" name="password" placeholder="Admin password" autofocus required>
<button>Sign in</button></form>${message ? `<p class="muted">${escape(message)}</p>` : ""}`);

    const table = (head, rows) => `<div class="scroll"><table><tr>${head.map(([label, num]) => `<th${num ? ' class="n"' : ""}>${label}</th>`).join("")}</tr>
${rows.length ? rows.map((r) => `<tr>${r.map(([value, num, cls]) => `<td class="${num ? "n " : ""}${cls ?? ""}">${value}</td>`).join("")}</tr>`).join("\n")
        : `<tr><td class="muted" colspan="${head.length}">Nothing yet</td></tr>`}</table></div>`;

    async function dashboard() {
        const p = await db();
        const one = async (sql, args = []) => (await p.query(sql, args))[0][0] ?? {};
        const all = async (sql, args = []) => (await p.query(sql, args))[0];

        const accounts = await one(`SELECT COUNT(*) AS total, SUM(joindate >= UTC_DATE()) AS today,
            SUM(joindate >= UTC_DATE() - INTERVAL 6 DAY) AS week, SUM(online > 0) AS online FROM acore_auth.account`);
        const chars = await one(`SELECT COUNT(*) AS total, SUM(online) AS online,
            SUM(logout_time >= UNIX_TIMESTAMP(UTC_DATE())) + SUM(online) AS today, SUM(totaltime) AS played FROM acore_characters.characters`);
        const events = await all(`SELECT event, detail, SUM(count) AS count, SUM(total_ms) AS ms FROM lasthavens.web_events
            WHERE day >= UTC_DATE() - INTERVAL 6 DAY GROUP BY event, detail`);
        const week = (event) => events.filter((e) => e.event === event).reduce((n, e) => n + Number(e.count), 0);
        const loaded = events.find((e) => e.event === "loaded");

        // Days come back as text: the driver would turn DATE into a local-midnight Date, a day off in UTC.
        const daily = await all(`SELECT DATE_FORMAT(day, '%Y-%m-%d') AS day, event, SUM(count) AS count, SUM(total_ms) AS ms FROM lasthavens.web_events
            WHERE day >= UTC_DATE() - INTERVAL 13 DAY GROUP BY day, event ORDER BY day DESC`);
        const signupsByDay = await all(`SELECT DATE_FORMAT(joindate, '%Y-%m-%d') AS day, COUNT(*) AS count FROM acore_auth.account
            WHERE joindate >= UTC_DATE() - INTERVAL 13 DAY GROUP BY day`);
        const days = [...Array(14)].map((_, i) => new Date(Date.now() - i * 86400_000).toISOString().slice(0, 10));
        const on = (day, event) => daily.find((r) => r.day === day && r.event === event);

        const signups = await all(`SELECT a.username, UNIX_TIMESTAMP(a.joindate) AS joined, UNIX_TIMESTAMP(a.last_login) AS lastLogin, a.online,
            COUNT(c.guid) AS characters, COALESCE(SUM(c.totaltime), 0) AS played
            FROM acore_auth.account a LEFT JOIN acore_characters.characters c ON c.account = a.id
            GROUP BY a.id ORDER BY a.joindate DESC LIMIT 50`);
        const online = await all(`SELECT c.name, c.level, c.race, c.class, a.username FROM acore_characters.characters c
            JOIN acore_auth.account a ON a.id = c.account WHERE c.online = 1 ORDER BY c.level DESC`);
        const top = await all(`SELECT c.name, c.level, c.race, c.class, c.totaltime, c.logout_time, c.online, a.username
            FROM acore_characters.characters c JOIN acore_auth.account a ON a.id = c.account
            ORDER BY c.level DESC, c.totaltime DESC LIMIT 50`);
        const classes = await all("SELECT class, COUNT(*) AS count FROM acore_characters.characters GROUP BY class ORDER BY count DESC");

        const card = (value, label) => `<div class="card"><b>${escape(value ?? 0)}</b><span>${label}</span></div>`;
        const avgLoad = loaded && Number(loaded.count) ? `${(Number(loaded.ms) / Number(loaded.count) / 1000).toFixed(1)} s` : "-";

        return page("Lasthavens admin", `<h1>Lasthavens admin</h1>
<div class="muted">Live from the realm database; website counts are anonymous per-day totals. Times are UTC.</div>
<div class="cards">
${card(chars.online ?? 0, "Characters online now")}${card(accounts.total, "Accounts")}${card(accounts.today ?? 0, "New accounts today")}
${card(accounts.week ?? 0, "New accounts, 7 days")}${card(chars.total, "Characters")}${card(chars.today ?? 0, "Characters played today")}
${card(hours(chars.played), "Total time played")}${card(week("visit"), "Page opens, 7 days")}${card(week("new_browser"), "New visitors, 7 days")}
${card(week("loaded"), "Reached login, 7 days")}${card(avgLoad, "Average load time, 7 days")}${card(week("load_failed"), "Failed loads, 7 days")}
</div>

<h2>Online now</h2>
${table([["Character"], ["Level", 1], ["Race"], ["Class"], ["Account"]],
        online.map((c) => [[escape(c.name), 0, "online"], [c.level, 1], [RACES[c.race] ?? c.race], [CLASSES[c.class] ?? c.class], [escape(c.username)]]))}

<h2>Last 14 days</h2>
${table([["Day"], ["Page opens", 1], ["New visitors", 1], ["Reached login", 1], ["Avg load", 1], ["Failed loads", 1], ["New accounts", 1], ["Logins", 1], ["World entries", 1]],
        days.map((day) => {
            const l = on(day, "loaded");
            return [[day], [on(day, "visit")?.count ?? 0, 1], [on(day, "new_browser")?.count ?? 0, 1], [l?.count ?? 0, 1],
                [l && Number(l.count) ? `${(Number(l.ms) / Number(l.count) / 1000).toFixed(1)} s` : "-", 1], [on(day, "load_failed")?.count ?? 0, 1],
                [signupsByDay.find((s) => s.day === day)?.count ?? 0, 1], [on(day, "login_connection")?.count ?? 0, 1],
                [on(day, "world_connection")?.count ?? 0, 1]];
        }))}

<h2>Recent accounts</h2>
${table([["Account"], ["Created"], ["Last login"], ["Characters", 1], ["Played", 1]],
        signups.map((a) => [[escape(a.username), 0, a.online ? "online" : ""], [date(a.joined, true)], [date(a.lastLogin, true)], [a.characters, 1], [hours(a.played), 1]]))}

<h2>Characters</h2>
${table([["Character"], ["Level", 1], ["Race"], ["Class"], ["Played", 1], ["Last seen"], ["Account"]],
        top.map((c) => [[escape(c.name), 0, c.online ? "online" : ""], [c.level, 1], [RACES[c.race] ?? c.race], [CLASSES[c.class] ?? c.class],
            [hours(c.totaltime), 1], [c.online ? "online now" : date(c.logout_time, true)], [escape(c.username)]]))}

<h2>Browsers (7 days) and classes</h2>
${table([["Browser"], ["Page opens", 1]], events.filter((e) => e.event === "visit").sort((a, b) => b.count - a.count).map((e) => [[e.detail], [e.count, 1]]))}
<br>${table([["Failed load reason"], ["Count", 1]], events.filter((e) => e.event === "load_failed").map((e) => [[e.detail], [e.count, 1]]))}
<br>${table([["Class"], ["Characters", 1]], classes.map((c) => [[CLASSES[c.class] ?? c.class], [c.count, 1]]))}`);
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

        if (!signedIn(req)) return send(200, loginPage("")), true;
        try {
            send(200, await dashboard());
        } catch (error) {
            console.error("admin:", error.message);
            send(500, page("Lasthavens admin", `<h1>Lasthavens admin</h1><p>The dashboard query failed: ${escape(error.message)}</p>`));
        }
        return true;
    }

    return { handle, record };
}
