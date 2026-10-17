"""Create (or reset) an AzerothCore account for local testing.

Computes the SRP6 salt and verifier the way AzerothCore stores them (acore_auth.account.salt / .verifier, 32 bytes
little-endian each) and writes them through the ac-database container, so no MySQL client or SOAP access is needed.

    python Tools/AzerothCore/create_account.py <username> <password> [--email addr]
"""

import argparse
import hashlib
import os
import subprocess

# The WoW 3.3.5a SRP6 group, same as AzerothCore's Acore::Crypto::SRP6.
N = int("894B645E89E1535BBDAD5B8B290650530801B18EBFBF5E8FAB3C82872A3E9BB7", 16)
G = 7


def make_registration(username, password, salt=None):
    salt = salt or os.urandom(32)
    inner = hashlib.sha1(f"{username.upper()}:{password.upper()}".encode()).digest()
    x = int.from_bytes(hashlib.sha1(salt + inner).digest(), "little")
    verifier = pow(G, x, N).to_bytes(32, "little")
    return salt, verifier


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("username")
    ap.add_argument("password")
    ap.add_argument("--email", default="")
    ap.add_argument("--container", default="ac-database")
    args = ap.parse_args()

    user = args.username.upper()
    if not (3 <= len(user) <= 16 and user.isalnum()):
        raise SystemExit("username must be 3-16 letters/digits")
    if not (1 <= len(args.password) <= 16):
        raise SystemExit("password must be 1-16 characters")
    if "'" in args.email or "\\" in args.email:
        raise SystemExit("bad email")

    salt, verifier = make_registration(user, args.password)
    sql = (
        "INSERT INTO acore_auth.account (username, salt, verifier, email, reg_mail, expansion) "
        f"VALUES ('{user}', X'{salt.hex()}', X'{verifier.hex()}', '{args.email}', '{args.email}', 2) "
        "ON DUPLICATE KEY UPDATE salt = VALUES(salt), verifier = VALUES(verifier);"
    )
    subprocess.run(
        ["docker", "exec", "-i", args.container, "mysql", "-uroot", "-ppassword"],
        input=sql.encode(), check=True, stderr=subprocess.DEVNULL,
    )
    print(f"account {user} ready")


if __name__ == "__main__":
    main()
