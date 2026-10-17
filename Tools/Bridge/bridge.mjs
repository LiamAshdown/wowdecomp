// Local bridge for the browser build of the client. One process, no dependencies:
//   /                    the page (Tools/Bridge/web) and the build output (bin/Web), with the COOP/COEP headers that
//                        SharedArrayBuffer (wasm threads) needs
//   /game-manifest.txt   "size<TAB>path" for every game file the client may read
//   /game/<path>         those files, with Range support (the client reads MPQs in blocks)
//   PUT|DELETE /userdata/<path>  persists what the client writes (WTF/, Cache/, Screenshots/) in the user-data overlay
//                        directory (never in --game); overlay files are listed in the manifest and win over --game files
//   /tcp/<ip>/<port>     WebSocket <-> TCP relay, only to the allowlisted AzerothCore auth/world addresses
//   /account/*           in-client account creation and password changes (account.mjs; DB login in bridge.env)
//
//   node Tools/Bridge/bridge.mjs [--host 127.0.0.1] [--port 8080] [--game bin/Win] [--userdata Tools/Bridge/userdata]
//                                [--web bin/Web] [--allow 127.0.0.1:3724,127.0.0.1:8085] [--trace]
//
// An --allow entry may name another target, "127.0.0.1:3724=ac-authserver:3724", so the addresses the client knows
// (realmlist.wtf and the realmlist table) stay 127.0.0.1 while the servers live elsewhere (Tools/Deploy).
//
// With WOW_BRIDGE_SECRET set (a hosted bridge) the page hands each browser a signed cookie, and each cookie gets its
// own user-data overlay (userdata/<id>/, at most USER_QUOTA bytes), the browser equivalent of a separate WoW install.
// Without it (local) there is one shared overlay.

import { createHash, createHmac, randomBytes, timingSafeEqual } from "node:crypto";
import { createReadStream, existsSync, readdirSync, readFileSync, statSync } from "node:fs";
import { mkdir, rename, rm, writeFile } from "node:fs/promises";
import { createServer } from "node:http";
import { connect } from "node:net";
import { dirname, extname, join, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { createAccountService } from "./account.mjs";
import { createAdmin } from "./admin.mjs";
import { ALLOWED_DIRS, MAX_FILE_BYTES, userPath } from "./userdata.mjs";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
// "--name value" pairs; a flag followed by another flag (--trace) has no value. Values may contain "--" (Windows temp paths).
const args = {};
for (let i = 2; i < process.argv.length; i++) {
    const m = /^--(.+)$/.exec(process.argv[i]);
    if (m) args[m[1]] = process.argv[i + 1]?.startsWith("--") ? "" : process.argv[++i];
}
const host = args.host ?? "127.0.0.1";
const port = Number(args.port ?? 8080);
const gameDir = resolve(root, args.game ?? "bin/Win");
const userDir = resolve(root, args.userdata ?? "Tools/Bridge/userdata");
// The build output first, so a deployment can override the page files there (e.g. server-alert.txt).
const webDirs = [resolve(root, args.web ?? "bin/Web"), resolve(root, "Tools/Bridge/web")];
// "ip:port" the client asks for -> "host:port" actually connected to.
const allow = new Map((args.allow ?? "127.0.0.1:3724,127.0.0.1:8085").split(",").map((e) => {
    const [from, to = from] = e.split("=");
    return [from, to];
}));

// KEY=value lines from Tools/Bridge/bridge.env (gitignored) fill in anything not already in the environment.
const envFile = resolve(root, "Tools/Bridge/bridge.env");
if (existsSync(envFile)) {
    for (const line of readFileSync(envFile, "utf8").split(/\r?\n/)) {
        const m = /^([A-Z_]+)=(.*)$/.exec(line);
        if (m && !(m[1] in process.env)) process.env[m[1]] = m[2];
    }
}
// /admin and the anonymous per-day usage counts (admin.mjs).
const admin = createAdmin({ databaseUrl: process.env.WOW_BRIDGE_DB, password: process.env.WOW_BRIDGE_ADMIN_PASSWORD, secret: process.env.WOW_BRIDGE_SECRET, webDirs });
const accounts = createAccountService(process.env.WOW_BRIDGE_DB, admin.record);

// ---- Per-browser cookie (hosted bridge only) -----------------------------------------------------------------------

const secret = process.env.WOW_BRIDGE_SECRET ?? "";
const sign = (id) => createHmac("sha256", secret).update(id).digest("hex").slice(0, 32);
// ponytail: anyone can mint cookies, so the disk bound is per browser only; cap total userdata if strangers abuse it.
const USER_QUOTA = 128 * 1024 * 1024;

// The request's user id: "" without a secret (shared overlay), null when it has no valid cookie yet.
function userOf(req) {
    if (!secret) return "";
    const m = /(?:^|;\s*)wowuser=([0-9a-f]{32})\.([0-9a-f]{32})(?:;|$)/.exec(req.headers.cookie ?? "");
    return m && timingSafeEqual(Buffer.from(sign(m[1])), Buffer.from(m[2])) ? m[1] : null;
}

// A new id and the Set-Cookie header that carries it. Changing WOW_BRIDGE_SECRET gives every browser a fresh overlay.
function newUser() {
    const id = randomBytes(16).toString("hex");
    return [id, `wowuser=${id}.${sign(id)}; Path=/; Max-Age=31536000; HttpOnly; Secure; SameSite=Strict`];
}

// Behind the reverse proxy every connection comes from the proxy, which puts the player's address last here.
const clientAddress = (req) => req.headers["x-forwarded-for"]?.split(",").pop().trim() || req.socket.remoteAddress;

// Directories and files the client never reads from the server (it writes these locally).
const SKIP_DIRS = new Set(["cache", "logs", "screenshots", "errors"]);
const SKIP_EXT = new Set([".exe", ".pdb", ".dll", ".bak", ".log", ".ilk"]);

function walkFiles(dir, base, files, skip = () => false) {
    for (const entry of readdirSync(dir, { withFileTypes: true })) {
        const full = join(dir, entry.name);
        if (entry.isDirectory()) {
            if (!skip(dir, entry.name)) walkFiles(full, base, files, skip);
        } else if (!SKIP_EXT.has(extname(entry.name).toLowerCase())) {
            files.set(relative(base, full).split(sep).join("/"), statSync(full).size);
        }
    }
    return files;
}

const baseFiles = walkFiles(gameDir, gameDir, new Map(), (dir, name) => dir === gameDir && SKIP_DIRS.has(name.toLowerCase()));

// User-data overlays, one per user id ("" = the shared one in userDir itself): rel path -> size, plus lowercase -> rel
// so a differently-cased write reuses the existing name.
const overlays = new Map();
function overlayFor(id) {
    let o = overlays.get(id);
    if (o) return o;
    o = { dir: id ? join(userDir, id) : userDir, files: new Map(), lower: new Map(), manifest: null };
    for (const dirName of ALLOWED_DIRS) {
        const dir = join(o.dir, dirName);
        if (existsSync(dir)) walkFiles(dir, o.dir, o.files);
    }
    for (const rel of o.files.keys()) o.lower.set(rel.toLowerCase(), rel);
    overlays.set(id, o);
    return o;
}

// Overlay wins over a game file of the same path (compared case-insensitively, the game directory is a Windows one).
function getManifestText(o) {
    if (o.manifest === null) {
        const lines = [];
        for (const [p, size] of baseFiles) if (!o.lower.has(p.toLowerCase())) lines.push(`${size}\t${p}`);
        for (const [p, size] of o.files) lines.push(`${size}\t${p}`);
        o.manifest = lines.join("\n") + "\n";
    }
    return o.manifest;
}

// Resolves a /game/ path to { file, size }, or null.
function lookupGameFile(o, rel) {
    if (o.files.has(rel)) return { file: join(o.dir, rel), size: o.files.get(rel) };
    const size = baseFiles.get(rel);
    if (size === undefined || o.lower.has(rel.toLowerCase())) return null;
    return { file: join(gameDir, rel), size };
}

// PUT (whole file) / DELETE /userdata/<path>. The custom header cannot be sent cross-site without a CORS preflight,
// which this server never answers. Locally the Host check stops DNS rebinding; hosted, the cookie does (a rebound name
// has none).
async function handleUserData(req, res, rel, o) {
    const fail = (code) => res.writeHead(code, { ...ISOLATION, Connection: "close" }).end();
    if (req.method !== "PUT" && req.method !== "DELETE") return fail(405);
    if (req.headers["x-wow-client"] !== "1" || (!secret && !/^(127\.0\.0\.1|localhost):\d+$/.test(req.headers.host ?? ""))) return fail(403);
    const canonical = userPath(rel);
    if (!canonical) return fail(400);
    const key = o.lower.get(canonical.toLowerCase()) ?? canonical;
    const file = join(o.dir, key);
    try {
        if (req.method === "DELETE") {
            await rm(file, { force: true });
            o.files.delete(key);
            o.lower.delete(key.toLowerCase());
        } else {
            let room = MAX_FILE_BYTES;
            if (o.dir !== userDir) {
                let used = 0;
                for (const size of o.files.values()) used += size;
                room = Math.min(room, USER_QUOTA - used + (o.files.get(key) ?? 0));
            }
            if (Number(req.headers["content-length"] ?? 0) > room) return fail(413);
            const chunks = [];
            let n = 0;
            for await (const chunk of req) {
                n += chunk.length;
                if (n > room) return fail(413);
                chunks.push(chunk);
            }
            await mkdir(dirname(file), { recursive: true });
            const tmp = `${file}.tmp${process.pid}`;
            await writeFile(tmp, Buffer.concat(chunks));
            await rename(tmp, file);
            o.files.set(key, n);
            o.lower.set(key.toLowerCase(), key);
        }
        o.manifest = null;
        res.writeHead(204, ISOLATION).end();
    } catch (e) {
        console.error(`userdata ${req.method} ${key}: ${e.message}`);
        fail(500);
    }
}

const BLOCK_BYTES = 1 << 20;   // WebVfs kBlockSize
const MIME = { ".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm", ".css": "text/css", ".svg": "image/svg+xml", ".txt": "text/plain", ".png": "image/png", ".jpg": "image/jpeg", ".json": "application/json" };
const ISOLATION = {
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
    "Cross-Origin-Resource-Policy": "same-origin",
};

function sendFile(req, res, file, size, type, extra = {}) {
    const headers = { ...ISOLATION, "Content-Type": type, "Accept-Ranges": "bytes", "Cache-Control": "no-cache", ...extra };
    const range = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range ?? "");
    if (range) {
        const start = Number(range[1]);
        const end = Math.min(range[2] ? Number(range[2]) : size - 1, size - 1);
        if (start > end) {
            res.writeHead(416, { ...headers, "Content-Range": `bytes */${size}` }).end();
            return;
        }
        res.writeHead(206, { ...headers, "Content-Range": `bytes ${start}-${end}/${size}`, "Content-Length": end - start + 1 });
        if (req.method === "HEAD") return res.end();
        createReadStream(file, { start, end }).pipe(res);
        return;
    }
    res.writeHead(200, { ...headers, "Content-Length": size });
    if (req.method === "HEAD") return res.end();
    createReadStream(file).pipe(res);
}

const server = createServer(async (req, res) => {
    let url, path;
    try {
        url = new URL(req.url, "http://x");
        path = decodeURIComponent(url.pathname);
    } catch {
        return res.writeHead(400, ISOLATION).end();
    }

    if ("trace" in args) console.log(req.method, path, req.headers.range ?? "", req.headers["cache-control"] ?? "", req.headers.pragma ?? "");

    // The page is always the first request, so it is where a browser gets its cookie.
    let user = userOf(req), extra = {};
    if (user === null && path === "/") {
        [user, extra["Set-Cookie"]] = newUser();
        admin.record("new_browser");
    }
    // Page files (one path segment: the page's images, scripts, the link-preview image) need no cookie, so link previews
    // can fetch them; game data, user data, accounts and the relay do.
    const pageFile = /^\/[\w.-]+$/.test(path) && path !== "/game-manifest.txt";
    if (user === null && !pageFile) return res.writeHead(403, ISOLATION).end();

    if (await admin.handle(req, res, path, clientAddress(req))) {
        return;
    }

    if (path.startsWith("/account/") && await accounts(req, res, path, clientAddress(req))) {
        return;
    }

    if (path === "/game-manifest.txt") {
        res.writeHead(200, { ...ISOLATION, "Content-Type": "text/plain", "Cache-Control": "no-store" }).end(getManifestText(overlayFor(user)));
        return;
    }

    if (path.startsWith("/userdata/")) return handleUserData(req, res, path.slice("/userdata/".length), overlayFor(user));

    // /blocks/<path>/<size>-<index>.bin: one 1 MB block of a shipped game file (Data/ only, never a user-data file),
    // cacheable for a year by the browser and the CDN (.bin is an extension Cloudflare caches). WebVfs fetches these and
    // falls back to /game/ Range requests. ponytail: the URL only changes with the file's size; replacing a file with
    // a same-sized one needs a cache purge.
    if (path.startsWith("/blocks/")) {
        const m = /^\/blocks\/(Data\/.+)\/(\d+)-(\d+)\.bin$/i.exec(path);
        const size = m ? baseFiles.get(m[1]) : undefined;
        const start = m ? Number(m[3]) * BLOCK_BYTES : 0;
        if (size === undefined || size !== Number(m[2]) || start >= size) return res.writeHead(404, ISOLATION).end();
        const end = Math.min(start + BLOCK_BYTES, size) - 1;
        res.writeHead(200, { ...ISOLATION, "Content-Type": "application/octet-stream", "Content-Length": end - start + 1,
            "Cache-Control": "public, max-age=31536000, immutable" });
        if (req.method === "HEAD") return res.end();
        return createReadStream(join(gameDir, m[1]), { start, end }).pipe(res);
    }

    if (path.startsWith("/game/")) {
        // Only manifest entries are served, so no path can escape the game or user-data directory.
        const found = lookupGameFile(overlayFor(user), path.slice("/game/".length));
        if (!found) return res.writeHead(404, ISOLATION).end();
        return sendFile(req, res, found.file, found.size, "application/octet-stream");
    }

    const name = path === "/" ? "index.html" : path.slice(1);
    if (!/^[\w.-]+$/.test(name)) return res.writeHead(404, ISOLATION).end();
    for (const dir of webDirs) {
        try {
            const st = statSync(join(dir, name));
            // Revalidated on every load (no-cache), so a 68 MB Wow.wasm is only sent again when it changed. The CDN may
            // weaken the tag when it compresses.
            const etag = `"${st.size.toString(36)}-${Math.trunc(st.mtimeMs).toString(36)}"`;
            if ((req.headers["if-none-match"] ?? "").replace(/^W\//, "") === etag) {
                return res.writeHead(304, { ...ISOLATION, ETag: etag, "Cache-Control": "no-cache", ...extra }).end();
            }
            return sendFile(req, res, join(dir, name), st.size, MIME[extname(name)] ?? "application/octet-stream", { ...extra, ETag: etag, "X-File-Size": st.size });
        } catch {}
    }
    res.writeHead(404, ISOLATION).end();
});

// ---- WebSocket <-> TCP relay ---------------------------------------------------------------------------------------

function wsFrame(opcode, payload) {
    const len = payload.length;
    const head = len < 126 ? Buffer.from([0x80 | opcode, len])
        : len < 65536 ? Buffer.from([0x80 | opcode, 126, len >> 8, len & 0xff])
        : Buffer.concat([Buffer.from([0x80 | opcode, 127]), (() => { const b = Buffer.alloc(8); b.writeBigUInt64BE(BigInt(len)); return b; })()]);
    return Buffer.concat([head, payload]);
}

// Parses complete client frames out of buf; returns the unconsumed tail.
function wsParse(buf, onFrame) {
    while (buf.length >= 2) {
        const opcode = buf[0] & 0x0f;
        let len = buf[1] & 0x7f;
        let off = 2;
        if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
        else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
        const masked = buf[1] & 0x80;
        const total = off + (masked ? 4 : 0) + len;
        if (buf.length < total) break;
        const payload = Buffer.from(buf.subarray(off + (masked ? 4 : 0), total));
        if (masked) {
            const mask = buf.subarray(off, off + 4);
            for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
        }
        onFrame(opcode, payload);
        buf = buf.subarray(total);
    }
    return buf;
}

server.on("upgrade", (req, socket) => {
    const m = /^\/tcp\/([\d.]+)\/(\d+)$/.exec(new URL(req.url, "http://x").pathname);
    const target = m && allow.get(`${m[1]}:${m[2]}`);
    if (!target || userOf(req) === null || !req.headers["sec-websocket-key"]) {
        socket.end("HTTP/1.1 403 Forbidden\r\n\r\n");
        return;
    }

    const accept = createHash("sha1").update(req.headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
    const protocol = (req.headers["sec-websocket-protocol"] ?? "").split(",")[0].trim();
    // The client's world connection goes to the realm's port (8085), its login connection to the login server's.
    admin.record(m[2] === "8085" ? "world_connection" : "login_connection");
    socket.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        + `Sec-WebSocket-Accept: ${accept}\r\n` + (protocol ? `Sec-WebSocket-Protocol: ${protocol}\r\n` : "") + "\r\n");
    socket.setNoDelay(true);

    const split = target.lastIndexOf(":");
    const tcp = connect(Number(target.slice(split + 1)), target.slice(0, split));
    tcp.setNoDelay(true);
    let pending = Buffer.alloc(0);
    const closeBoth = () => { tcp.destroy(); socket.destroy(); };

    tcp.on("data", (data) => socket.write(wsFrame(0x2, data)));
    tcp.on("close", () => { if (!socket.destroyed) socket.end(wsFrame(0x8, Buffer.alloc(0))); });
    tcp.on("error", closeBoth);
    socket.on("data", (data) => {
        pending = wsParse(Buffer.concat([pending, data]), (opcode, payload) => {
            if (opcode === 0x2 || opcode === 0x0 || opcode === 0x1) tcp.write(payload);
            else if (opcode === 0x8) closeBoth();
            else if (opcode === 0x9) socket.write(wsFrame(0xa, payload));
        });
    });
    socket.on("close", () => tcp.destroy());
    socket.on("error", closeBoth);
});

server.listen(port, host, () => {
    console.log(`bridge: http://${host}:${port}/  game=${gameDir} (${baseFiles.size} files)  userdata=${userDir}${secret ? " (per browser)" : ""}`
        + `  relay=${[...allow].map(([a, b]) => a === b ? a : `${a}=${b}`).join(",")}`);
});
