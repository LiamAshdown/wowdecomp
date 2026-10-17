// Account service for the in-client account screens (WoW/Source/Glue/Web/WebAccount.cpp).
//
// The client never sends a password. Registration carries the SRP6 salt and verifier the client computed, which is
// exactly what AzerothCore stores in acore_auth.account. Signing in to manage an account is an SRP6 proof of the
// password against the stored verifier (the same exchange the auth server runs at login). A successful proof returns
// the account's details and a random token, kept only in this process, that authorises password and email changes for
// five minutes without the password being typed again.
//
// Requests are application/x-www-form-urlencoded POSTs carrying an X-Wow-Client header (so cross-site pages cannot
// send them without a CORS preflight, which the bridge never answers). Replies are plain text: "OK" or "ERR", a
// newline, then the message or fields.

import { createHash, randomBytes, timingSafeEqual } from "node:crypto";

export const N = BigInt("0x894B645E89E1535BBDAD5B8B290650530801B18EBFBF5E8FAB3C82872A3E9BB7");
export const G = 7n;

export const le = (n, size = 32) => {
    const out = Buffer.alloc(size);
    for (let i = 0; i < size; i++, n >>= 8n) out[i] = Number(n & 0xffn);
    return out;
};
export const fromLe = (buf) => buf.reduceRight((acc, byte) => (acc << 8n) | BigInt(byte), 0n);
export const sha1 = (...parts) => {
    const h = createHash("sha1");
    for (const p of parts) h.update(p);
    return h.digest();
};
export const modPow = (base, exp, mod) => {
    let result = 1n;
    base %= mod;
    for (; exp > 0n; exp >>= 1n, base = (base * base) % mod) {
        if (exp & 1n) result = (result * base) % mod;
    }
    return result;
};

// AzerothCore SRP6::SHA1Interleave
export function sessionKey(S) {
    const s = le(S);
    let p = 0;
    while (p < 32 && s[p] === 0) p++;
    p = (p + 1) >> 1;
    const even = Buffer.alloc(16 - p), odd = Buffer.alloc(16 - p);
    for (let i = p; i < 16; i++) {
        even[i - p] = s[i * 2];
        odd[i - p] = s[i * 2 + 1];
    }
    const h0 = sha1(even), h1 = sha1(odd), K = Buffer.alloc(40);
    for (let i = 0; i < 20; i++) {
        K[i * 2] = h0[i];
        K[i * 2 + 1] = h1[i];
    }
    return K;
}

export function clientProof(user, salt, A, B, K) {
    const ng = Buffer.from(sha1(le(N)).map((byte, i) => byte ^ sha1(le(G, 1))[i]));
    return sha1(ng, sha1(Buffer.from(user)), salt, le(A), le(B), K);
}

// Server half of the SRP6 exchange: the challenge's B, then the check of the client's A and M1.
export function serverPublicKey(v, b) {
    return (3n * v + modPow(G, b, N)) % N;
}

export function checkProof(user, salt, v, b, B, A, m1) {
    if (A % N === 0n) return false;
    const u = fromLe(sha1(le(A), le(B)));
    const S = modPow((A * modPow(v, u, N)) % N, b, N);
    return timingSafeEqual(clientProof(user, salt, A, B, sessionKey(S)), m1);
}

const USER = /^[A-Za-z0-9]{3,16}$/;
const HEX32 = /^[0-9a-fA-F]{64}$/;
const EMAIL = /^[^\s@'"\\]{1,64}@[^\s@'"\\]{1,190}\.[A-Za-z]{2,24}$/;

// ponytail: in-memory per-address limits; enough for a local bridge, needs a shared store if this ever scales out.
const limits = new Map();
function limited(key, max, windowMs) {
    const now = Date.now();
    const hits = (limits.get(key) ?? []).filter((t) => now - t < windowMs);
    hits.push(now);
    limits.set(key, hits);
    return hits.length > max;
}

const sessions = new Map();
const tokens = new Map();
const TOKEN_MS = 5 * 60_000;

const EXPANSIONS = ["Classic", "The Burning Crusade", "Wrath of the Lich King"];
const when = (epoch, withTime) => {
    const iso = new Date(epoch * 1000).toISOString();
    return withTime ? `${iso.slice(0, 10)} ${iso.slice(11, 16)} UTC` : iso.slice(0, 10);
};

// The eight display lines of the details view, in order: account, email, joined, last login, last IP, expansion, ban
// status, characters. Dates arrive as unix seconds (0 or null when unset); ban is an active account_banned row or null.
export function accountDetails(row, ban, characters) {
    let banned = "Not banned";
    if (ban) banned = ban.unbandate === ban.bandate ? "Permanently banned" : `Banned until ${when(ban.unbandate, false)}`;
    return [
        row.username,
        row.email || "(none)",
        row.joined ? when(row.joined, true) : "Unknown",
        row.lastLogin ? when(row.lastLogin, true) : "Never",
        row.lastIp || "Unknown",
        EXPANSIONS[row.expansion] ?? `Expansion ${row.expansion}`,
        banned,
        String(characters ?? 0),
    ];
}

export function createAccountService(databaseUrl) {
    let pool;
    const db = async () => {
        if (!databaseUrl) throw new Error("The account service is not configured (Tools/Bridge/bridge.env).");
        if (!pool) {
            const mysql = await import("mysql2/promise");
            pool = mysql.createPool({ uri: databaseUrl, connectionLimit: 4 });
        }
        return pool;
    };

    async function register(form, address) {
        const user = (form.get("user") ?? "").toUpperCase();
        const salt = form.get("salt") ?? "", verifier = form.get("verifier") ?? "", email = form.get("email") ?? "";
        if (!USER.test(user)) return "ERR\nAccount names are 3 to 16 letters or digits.";
        if (!HEX32.test(salt) || !HEX32.test(verifier)) return "ERR\nMalformed request.";
        if (email && !EMAIL.test(email)) return "ERR\nThat email address does not look valid.";
        if (limited(`register:${address}`, 5, 3600_000)) return "ERR\nToo many accounts created recently. Try again later.";

        const pool = await db();
        try {
            const [result] = await pool.execute(
                "INSERT INTO account (username, salt, verifier, email, reg_mail, expansion, joindate) VALUES (?, ?, ?, ?, ?, 2, NOW())",
                [user, Buffer.from(salt, "hex"), Buffer.from(verifier, "hex"), email, email]);
            await pool.execute("INSERT INTO realmcharacters (realmid, acctid, numchars) SELECT id, ?, 0 FROM realmlist", [result.insertId]);
        } catch (error) {
            if (error.code === "ER_DUP_ENTRY") return "ERR\nThat account name is already taken.";
            throw error;
        }
        return `OK\nAccount ${user} created. You can log in now.`;
    }

    // Unknown accounts get a random challenge too, so the reply does not reveal which names exist.
    async function challenge(form, address) {
        const user = (form.get("user") ?? "").toUpperCase();
        if (!USER.test(user)) return "ERR\nAccount names are 3 to 16 letters or digits.";
        if (limited(`challenge:${address}`, 20, 600_000)) return "ERR\nToo many attempts. Try again later.";

        const [rows] = await (await db()).execute("SELECT id, salt, verifier FROM account WHERE username = ?", [user]);
        const row = rows[0];
        const salt = row ? Buffer.from(row.salt) : randomBytes(32);
        const v = row ? fromLe(Buffer.from(row.verifier)) : fromLe(randomBytes(32)) % N;
        const b = fromLe(randomBytes(32)) % N;
        const B = serverPublicKey(v, b);
        const id = randomBytes(16).toString("hex");

        sessions.set(id, { user, accountId: row?.id, salt, v, b, B, expires: Date.now() + 60_000 });
        return `OK\n${salt.toString("hex")}\n${le(B).toString("hex")}\n${id}`;
    }

    // The token's account, or null once it has expired.
    function redeem(form) {
        const id = form.get("token") ?? "";
        const entry = tokens.get(id);
        if (entry && entry.expires < Date.now()) tokens.delete(id);
        return entry && entry.expires >= Date.now() ? entry : null;
    }
    const EXPIRED = "ERR\nYour session expired. Please sign in again.";

    async function details(accountId) {
        const pool = await db();
        const [[row]] = await pool.execute(
            "SELECT username, email, UNIX_TIMESTAMP(joindate) AS joined, UNIX_TIMESTAMP(last_login) AS lastLogin, last_ip AS lastIp, expansion FROM account WHERE id = ?", [accountId]);
        const [[ban]] = await pool.execute(
            "SELECT bandate, unbandate FROM account_banned WHERE id = ? AND active = 1 AND (unbandate = bandate OR unbandate > UNIX_TIMESTAMP()) ORDER BY unbandate = bandate DESC, unbandate DESC LIMIT 1", [accountId]);
        const [[chars]] = await pool.execute("SELECT SUM(numchars) AS total FROM realmcharacters WHERE acctid = ?", [accountId]);
        return accountDetails(row, ban ?? null, Number(chars?.total ?? 0)).join("\n");
    }

    // Proof of the password in; token and details out.
    async function login(form, address) {
        const session = sessions.get(form.get("id") ?? "");
        sessions.delete(form.get("id") ?? "");
        const a = form.get("a") ?? "", m1 = form.get("m1") ?? "";
        if (!session || session.expires < Date.now()) return "ERR\nThe request expired. Please try again.";
        if (!HEX32.test(a) || !/^[0-9a-fA-F]{40}$/.test(m1)) return "ERR\nMalformed request.";
        if (limited(`login:${address}`, 10, 600_000)) return "ERR\nToo many attempts. Try again later.";

        const A = fromLe(Buffer.from(a, "hex"));
        const valid = checkProof(session.user, session.salt, session.v, session.b, session.B, A, Buffer.from(m1, "hex"));
        if (!session.accountId || !valid) return "ERR\nThe account name or password is incorrect.";

        for (const [id, t] of tokens) if (t.expires < Date.now()) tokens.delete(id);
        const token = randomBytes(24).toString("hex");
        tokens.set(token, { accountId: session.accountId, user: session.user, expires: Date.now() + TOKEN_MS });
        return `OK\n${token}\n${await details(session.accountId)}`;
    }

    async function info(form) {
        const t = redeem(form);
        return t ? `OK\n${await details(t.accountId)}` : EXPIRED;
    }

    async function changeEmail(form) {
        const t = redeem(form), email = form.get("email") ?? "";
        if (!t) return EXPIRED;
        if (!EMAIL.test(email)) return "ERR\nThat email address does not look valid.";
        await (await db()).execute("UPDATE account SET email = ? WHERE id = ?", [email, t.accountId]);
        return "OK\nEmail changed.";
    }

    // The verifier is bound to the account name, so a client that hashed for another name is refused.
    async function changePassword(form) {
        const t = redeem(form), salt = form.get("salt") ?? "", verifier = form.get("verifier") ?? "";
        if (!t) return EXPIRED;
        if (!HEX32.test(salt) || !HEX32.test(verifier) || (form.get("user") ?? "").toUpperCase() !== t.user) return "ERR\nMalformed request.";
        await (await db()).execute("UPDATE account SET salt = ?, verifier = ? WHERE id = ?", [Buffer.from(salt, "hex"), Buffer.from(verifier, "hex"), t.accountId]);
        return "OK\nPassword changed.";
    }

    const routes = {
        "/account/register": register, "/account/challenge": challenge, "/account/login": login,
        "/account/info": info, "/account/email": changeEmail, "/account/password": changePassword,
    };

    return async function handle(req, res, path, address = req.socket.remoteAddress) {
        const route = routes[path];
        if (!route) return false;
        const reply = (status, text) => res.writeHead(status, { "Content-Type": "text/plain", "Cross-Origin-Resource-Policy": "same-origin" }).end(text);
        if (req.method !== "POST" || req.headers["x-wow-client"] !== "1") {
            reply(403, "ERR\nForbidden.");
            return true;
        }

        let body = "";
        for await (const chunk of req) {
            body += chunk;
            if (body.length > 4096) return reply(413, "ERR\nRequest too large."), true;
        }

        try {
            reply(200, await route(new URLSearchParams(body), address));
        } catch (error) {
            console.error("account service:", error.message);
            reply(200, `ERR\n${error.message.startsWith("The account service") ? error.message : "The account service failed. See the bridge log."}`);
        }
        return true;
    };
}
