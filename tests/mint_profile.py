"""Test setup: mint a mount token and store it as a RecordFS credential profile.

    python tests/mint_profile.py <ws[s]-url> <profile> <drive> [--machine NAME]
                                 [--user claude --password ...] [--ttl-days 1]

Logs in with a PASSWORD on a full session (the testing login by default),
asks the server for a short-lived mount token, and hands it straight to
`recordfs store-token` (user-scoped DPAPI) -- the token is never printed.
Then mount it with `recordfs agent-run --profile <profile>`. Remove with
`recordfs erase-token --profile <profile>`; the token itself expires.
"""
import argparse, asyncio, json, os, subprocess, sys, time

import websockets

p = argparse.ArgumentParser()
p.add_argument("url")
p.add_argument("profile")
p.add_argument("drive")
p.add_argument("--machine", default=None)
p.add_argument("--user", default="claude")
p.add_argument("--password", default="claude'spassword")
p.add_argument("--ttl-days", type=int, default=1)
p.add_argument("--exe", default=os.path.join(os.path.dirname(__file__), "..", "x64", "Release",
                                             "recordfs.exe"))
p.add_argument("--token-file", default=None,
               help="write the token to this file instead of storing a profile (for a test "
                    "machine that stores it itself, e.g. update_sandbox_e2e.py); delete it after")
args = p.parse_args()


async def mint():
    async with websockets.connect(args.url, max_size=None) as ws:
        await ws.send(json.dumps({"type": "login", "id": 1, "data": {
            "username": args.user, "password_plain": args.password}}))
        while True:
            m = json.loads(await asyncio.wait_for(ws.recv(), 30))
            if m.get("type") in ("success", "failure"):
                break
        if m.get("type") != "success":
            sys.exit("login failed")
        await ws.send(json.dumps({"type": "request", "request": "create_client_token", "id": 2,
                                  "data": {"machine": args.machine or "test-" + args.profile,
                                           "ttl_days": args.ttl_days}}))
        t0 = time.time()
        while time.time() - t0 < 30:
            m = json.loads(await asyncio.wait_for(ws.recv(), 30))
            if m.get("id") == 2 and m.get("type") != "progress":
                break
        tok = (m.get("result") or {}).get("token")
        if not tok:
            sys.exit("mint failed: " + str(m.get("error")))
        return tok


token = asyncio.run(mint())
if args.token_file:
    with open(args.token_file, "w") as f:
        f.write(token)
    print("token written to", args.token_file)
    sys.exit(0)
r = subprocess.run([args.exe, "store-token", "--profile", args.profile, "--server", args.url,
                    "--token", token, "--drive", args.drive], capture_output=True, text=True)
print("stored profile", args.profile, "->", args.url, args.drive, "rc", r.returncode)
sys.exit(r.returncode)
