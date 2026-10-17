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
//                                [--web bin/Web] [--allow 127.0.0.1:3724,127.0.0.1:8085]
//
// An --allow entry may name another target, "127.0.0.1:3724=ac-authserver:3724", so the addresses the client knows
// (realmlist.wtf and the realmlist table) stay 127.0.0.1 while the servers live elsewhere (Tools/Deploy).
//
// With WOW_BRIDGE_INVITE set (a hosted bridge) every request needs the cookie that /?invite=<code> hands out, and
// each cookie gets its own user-data overlay (userdata/<id>/), the browser equivalent of a separate WoW install.
// Without it (local) there is one shared overlay and no gate.

import { createHash, createHmac, randomBytes, timingSafeEqual } from "node:crypto";
import { createReadStream, existsSync, readdirSync, readFileSync, statSync } from "node:fs";
import { mkdir, rename, rm, writeFile } from "node:fs/promises";
import { createServer } from "node:http";
import { connect } from "node:net";
import { dirname, extname, join, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { createAccountService } from "./account.mjs";
import { ALLOWED_DIRS, MAX_FILE_BYTES, userPath } from "./userdata.mjs";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const args = Object.fromEntries(process.argv.slice(2).join(" ").split("--").filter(Boolean).map((a) => a.trim().split(/\s+/)));
const host = args.host ?? "127.0.0.1";
const port = Number(args.port ?? 8080);
const gameDir = resolve(root, args.game ?? "bin/Win");
const userDir = resolve(root, args.userdata ?? "Tools/Bridge/userdata");
const webDirs = [resolve(root, "Tools/Bridge/web"), resolve(root, args.web ?? "bin/Web")];
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
const accounts = createAccountService(process.env.WOW_BRIDGE_DB);

// ---- Invite gate (hosted bridge only) ------------------------------------------------------------------------------

const invite = process.env.WOW_BRIDGE_INVITE ?? "";
const sign = (id) => createHmac("sha256", invite).update(id).digest("hex").slice(0, 32);
const sameSecret = (a, b) => timingSafeEqual(createHash("sha256").update(a).digest(), createHash("sha256").update(b).digest());

// The request's user id: "" when there is no gate (shared overlay), null when the gate refuses it.
function userOf(req) {
    if (!invite) return "";
    const m = /(?:^|;\s*)wowuser=([0-9a-f]{32})\.([0-9a-f]{32})(?:;|$)/.exec(req.headers.cookie ?? "");
    return m && sameSecret(sign(m[1]), m[2]) ? m[1] : null;
}

// Changing WOW_BRIDGE_INVITE signs every existing cookie out.
function grantInvite(res) {
    const id = randomBytes(16).toString("hex");
    res.writeHead(302, {
        ...ISOLATION, Location: "/",
        "Set-Cookie": `wowuser=${id}.${sign(id)}; Path=/; Max-Age=31536000; HttpOnly; Secure; SameSite=Strict`,
    }).end();
}

const REFUSED = "<!doctype html><meta charset=utf-8><title>Lasthavens</title>"
    + "<body style='background:#111;color:#ccc;font:16px sans-serif;text-align:center;padding-top:20vh'>"
    + "This realm is invite only. Open the invite link you were sent.";

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
// which this server never answers. Locally the Host check stops DNS rebinding; behind the gate the cookie does (a
// rebound name has none).
async function handleUserData(req, res, rel, o) {
    const fail = (code) => res.writeHead(code, { ...ISOLATION, Connection: "close" }).end();
    if (req.method !== "PUT" && req.method !== "DELETE") return fail(405);
    if (req.headers["x-wow-client"] !== "1" || (!invite && !/^(127\.0\.0\.1|localhost):\d+$/.test(req.headers.host ?? ""))) return fail(403);
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
            if (Number(req.headers["content-length"] ?? 0) > MAX_FILE_BYTES) return fail(413);
            const chunks = [];
            let n = 0;
            for await (const chunk of req) {
                n += chunk.length;
                if (n > MAX_FILE_BYTES) return fail(413);
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

const MIME = { ".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm", ".css": "text/css" };
const ISOLATION = {
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
    "Cross-Origin-Resource-Policy": "same-origin",
};

function sendFile(req, res, file, size, type) {
    const headers = { ...ISOLATION, "Content-Type": type, "Accept-Ranges": "bytes", "Cache-Control": "no-cache" };
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

    const user = userOf(req);
    if (user === null) {
        const code = url.searchParams.get("invite");
        if (path === "/" && code && sameSecret(code, invite)) return grantInvite(res);
        return res.writeHead(403, { ...ISOLATION, "Content-Type": "text/html" }).end(REFUSED);
    }

    if (path.startsWith("/account/") && await accounts(req, res, path, clientAddress(req))) {
        return;
    }

    if (path === "/game-manifest.txt") {
        res.writeHead(200, { ...ISOLATION, "Content-Type": "text/plain", "Cache-Control": "no-store" }).end(getManifestText(overlayFor(user)));
        return;
    }

    if (path.startsWith("/userdata/")) return handleUserData(req, res, path.slice("/userdata/".length), overlayFor(user));

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
            return sendFile(req, res, join(dir, name), st.size, MIME[extname(name)] ?? "application/octet-stream");
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
    console.log(`bridge: http://${host}:${port}/  game=${gameDir} (${baseFiles.size} files)  userdata=${userDir}${invite ? " (per invite)" : ""}`
        + `  relay=${[...allow].map(([a, b]) => a === b ? a : `${a}=${b}`).join(",")}`);
});
