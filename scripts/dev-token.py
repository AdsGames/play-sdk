#!/usr/bin/env python3
"""Print an adsgames.net session token for a local play server.

    JWT_SECRET=localsecret python3 scripts/dev-token.py [username]

The token is valid for one day. Send it as `Authorization: Bearer <token>`,
or set it as the `access-token` cookie in a browser.
"""

import base64
import hashlib
import hmac
import json
import os
import sys
import time
import uuid


def b64(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def main() -> None:
    secret = os.environ.get("JWT_SECRET")
    if not secret:
        sys.exit("set JWT_SECRET to the play server's secret")

    username = sys.argv[1] if len(sys.argv) > 1 else "tester"
    # Same user id for the same name, so reruns update one player
    user_id = str(uuid.uuid5(uuid.NAMESPACE_URL, f"play-dev/{username}"))

    header = b64(json.dumps({"alg": "HS256", "typ": "JWT"}).encode())
    claims = b64(
        json.dumps(
            {
                "sub": user_id,
                "username": username,
                "iss": "adsgames.net",
                "exp": int(time.time()) + 86400,
            }
        ).encode()
    )
    signature = hmac.new(secret.encode(), f"{header}.{claims}".encode(), hashlib.sha256)
    print(f"{header}.{claims}.{b64(signature.digest())}")


if __name__ == "__main__":
    main()
