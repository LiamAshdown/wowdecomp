"""Smoke test for a 3.3.5a (12340) server: SRP6 logon, realm list, and world auth (CMSG_AUTH_SESSION ->
SMSG_AUTH_RESPONSE through the encrypted header stream). Exits non-zero on failure.

    python Tools/AzerothCore/login_probe.py <username> <password> [--host 127.0.0.1] [--port 3724]

Credentials can also come from Tools/AzerothCore/test-account.env (WOW_TEST_USER / WOW_TEST_PASS).
"""

import argparse
import hashlib
import hmac
import os
import socket
import struct
import sys

from create_account import G, N


def sha1(*parts):
    h = hashlib.sha1()
    for p in parts:
        h.update(p)
    return h.digest()


def le(n, size=32):
    return n.to_bytes(size, "little")


def from_le(b):
    return int.from_bytes(b, "little")


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf += chunk
    return buf


def session_key(S):
    # AzerothCore SRP6::SHA1Interleave
    s = le(S)
    p = 0
    while p < 32 and s[p] == 0:
        p += 1
    p = (p + 1) // 2
    h0 = sha1(s[0::2][p:])
    h1 = sha1(s[1::2][p:])
    return bytes(b for pair in zip(h0, h1) for b in pair)


def logon(host, port, user, password):
    user = user.upper()
    sock = socket.create_connection((host, port), timeout=10)
    body = (b"WoW\0" + bytes([3, 3, 5]) + struct.pack("<H", 12340) + b"68x\0" + b"niW\0" + b"SUne"
            + struct.pack("<II", 0, 0x0100007F) + bytes([len(user)]) + user.encode())
    sock.sendall(bytes([0x00, 0x08]) + struct.pack("<H", len(body)) + body)

    hdr = recv_exact(sock, 3)
    if hdr[2] != 0:
        raise RuntimeError(f"logon challenge failed, error {hdr[2]}")
    B = from_le(recv_exact(sock, 32))
    g_len = recv_exact(sock, 1)[0]
    g = from_le(recv_exact(sock, g_len))
    n_len = recv_exact(sock, 1)[0]
    n = from_le(recv_exact(sock, n_len))
    salt = recv_exact(sock, 32)
    recv_exact(sock, 16)
    if recv_exact(sock, 1)[0] != 0:
        raise RuntimeError("account has 2FA/security flags; probe does not support them")
    assert (g, n) == (G, N)

    a = from_le(os.urandom(32))
    A = pow(G, a, N)
    u = from_le(sha1(le(A), le(B)))
    x = from_le(sha1(salt, sha1(f"{user}:{password.upper()}".encode())))
    S = pow((B - 3 * pow(G, x, N)) % N, a + u * x, N)
    K = session_key(S)
    ng = bytes(p ^ q for p, q in zip(sha1(le(N)), sha1(le(G, 1))))
    M1 = sha1(ng, sha1(user.encode()), salt, le(A), le(B), K)

    sock.sendall(bytes([0x01]) + le(A) + M1 + bytes(20) + bytes([0, 0]))
    resp = recv_exact(sock, 2)
    if resp[1] != 0:
        raise RuntimeError(f"logon proof rejected, error {resp[1]} (wrong password?)")
    M2 = recv_exact(sock, 20)
    recv_exact(sock, 10)
    if M2 != sha1(le(A), M1, K):
        raise RuntimeError("server proof mismatch")

    sock.sendall(bytes([0x10]) + bytes(4))
    cmd, size = struct.unpack("<BH", recv_exact(sock, 3))
    data = recv_exact(sock, size)
    count = struct.unpack_from("<H", data, 4)[0]
    off, realms = 6, []
    for _ in range(count):
        off += 3
        name_end = data.index(b"\0", off)
        addr_end = data.index(b"\0", name_end + 1)
        realms.append((data[off:name_end].decode(), data[name_end + 1:addr_end].decode()))
        off = addr_end + 1 + 4 + 3
    sock.close()
    return K, realms


class Arc4:
    def __init__(self, key):
        self.S = list(range(256))
        j = 0
        for i in range(256):
            j = (j + self.S[i] + key[i % len(key)]) % 256
            self.S[i], self.S[j] = self.S[j], self.S[i]
        self.i = self.j = 0
        self.crypt(bytes(1024))

    def crypt(self, data):
        out = bytearray()
        S = self.S
        for c in data:
            self.i = (self.i + 1) % 256
            self.j = (self.j + S[self.i]) % 256
            S[self.i], S[self.j] = S[self.j], S[self.i]
            out.append(c ^ S[(S[self.i] + S[self.j]) % 256])
        return bytes(out)


def world_auth(host, port, user, K):
    sock = socket.create_connection((host, port), timeout=10)
    size, opcode = struct.unpack("<HH", recv_exact(sock, 4))
    size = socket.ntohs(size)
    payload = recv_exact(sock, size - 2)
    if opcode != 0x1EC:
        raise RuntimeError(f"expected SMSG_AUTH_CHALLENGE, got {opcode:#x}")
    server_seed = payload[4:8]

    client_seed = os.urandom(4)
    digest = sha1(user.upper().encode(), bytes(4), client_seed, server_seed, K)
    body = (struct.pack("<II", 12340, 0) + user.upper().encode() + b"\0" + struct.pack("<I", 0) + client_seed
            + struct.pack("<IIIQ", 0, 0, 1, 0) + digest + struct.pack("<I", 0))
    sock.sendall(struct.pack(">H", len(body) + 4) + struct.pack("<I", 0x1ED) + body)

    decrypt = Arc4(hmac.new(bytes.fromhex("CC98AE04E897EACA12DDC09342915357"), K, hashlib.sha1).digest())
    hdr = decrypt.crypt(recv_exact(sock, 4))
    size = struct.unpack(">H", hdr[:2])[0]
    opcode = struct.unpack("<H", hdr[2:])[0]
    body = recv_exact(sock, size - 2)
    sock.close()
    if opcode != 0x1EE:
        raise RuntimeError(f"expected SMSG_AUTH_RESPONSE, got {opcode:#x}")
    return body[0]


def main():
    env = {}
    env_path = os.path.join(os.path.dirname(__file__), "test-account.env")
    if os.path.exists(env_path):
        env = dict(l.strip().split("=", 1) for l in open(env_path) if "=" in l and not l.startswith("#"))
    ap = argparse.ArgumentParser()
    ap.add_argument("username", nargs="?", default=env.get("WOW_TEST_USER"))
    ap.add_argument("password", nargs="?", default=env.get("WOW_TEST_PASS"))
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=3724)
    args = ap.parse_args()
    if not args.username or not args.password:
        raise SystemExit("need username and password")

    K, realms = logon(args.host, args.port, args.username, args.password)
    print("logon ok, realms:", realms)
    if not realms:
        raise SystemExit("no realms")
    host, port = realms[0][1].rsplit(":", 1)
    result = world_auth(host, int(port), args.username, K)
    print(f"world auth result {result:#x}", "(AUTH_OK)" if result == 0x0C else "")
    sys.exit(0 if result == 0x0C else 1)


if __name__ == "__main__":
    main()
