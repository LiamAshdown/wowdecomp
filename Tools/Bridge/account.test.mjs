// node Tools/Bridge/account.test.mjs — checks the server half of SRP6 against a client computed the way the game
// client (and Tools/AzerothCore/login_probe.py, verified against AzerothCore) does it.
import assert from "node:assert/strict";
import { randomBytes } from "node:crypto";
import { G, N, accountDetails, checkProof, clientProof, fromLe, le, modPow, serverPublicKey, sessionKey, sha1 } from "./account.mjs";

const user = "WEBTEST", password = "CORRECT HORSE";
const salt = randomBytes(32);
const x = fromLe(sha1(salt, sha1(Buffer.from(`${user}:${password}`))));
const v = modPow(G, x, N);

function login(pass) {
    const b = fromLe(randomBytes(32)) % N;
    const B = serverPublicKey(v, b);
    const a = fromLe(randomBytes(32));
    const A = modPow(G, a, N);
    const u = fromLe(sha1(le(A), le(B)));
    const xp = fromLe(sha1(salt, sha1(Buffer.from(`${user}:${pass}`))));
    const S = modPow(((B - 3n * modPow(G, xp, N)) % N + N) % N, a + u * xp, N);
    return checkProof(user, salt, v, b, B, A, clientProof(user, salt, A, B, sessionKey(S)));
}

assert.equal(login(password), true, "correct password is accepted");
assert.equal(login("WRONG"), false, "wrong password is rejected");
assert.equal(checkProof(user, salt, v, 5n, serverPublicKey(v, 5n), N, Buffer.alloc(20)), false, "A = N is rejected");

const row = { username: "WEBTEST", email: "", joined: 1700000000, lastLogin: 0, lastIp: "127.0.0.1", expansion: 2 };
assert.deepEqual(accountDetails(row, null, 3), ["WEBTEST", "(none)", "2023-11-14 22:13 UTC", "Never", "127.0.0.1", "Wrath of the Lich King", "Not banned", "3"]);
assert.equal(accountDetails(row, { bandate: 5, unbandate: 5 }, 0)[6], "Permanently banned");
assert.equal(accountDetails(row, { bandate: 5, unbandate: 1700000000 }, 0)[6], "Banned until 2023-11-14");
console.log("account.test: ok");
