"""Is the content-addressed dedup invisible to users?

Two attachments with byte-identical content share ONE blob. Editing one must
not disturb the other: a write produces a NEW hash and re-points only the row
that was written. This is the property that makes dedup safe to be invisible.
"""
import asyncio, hashlib, json, os, sys, time
import websockets

SRV = "ws://127.0.0.1:17243/"
KEY = 50049
ROOT = "S:\\workorders"
A, B = "dedup-a.txt", "dedup-b.txt"
SAME = b"identical content in two different attachments\n" + b"q" * 300
passed, failed = [], []


def check(n, c, d=""):
    (passed if c else failed).append(n)
    print(("PASS " if c else "FAIL ") + n + (("  -- " + str(d)[:200]) if not c else ""))


_id = [4000]


async def rpc(ws, msg):
    _id[0] += 1
    msg["id"] = _id[0]
    await ws.send(json.dumps(msg))
    t0 = time.time()
    while time.time() - t0 < 30:
        m = json.loads(await asyncio.wait_for(ws.recv(), 30))
        if m.get("id") == _id[0]:
            if m.get("type") == "progress":
                continue
            return m


def files():
    async def f(ws):
        await ws.send(json.dumps({"type": "login", "id": 1,
                                  "data": {"username": "claude",
                                           "password_plain": "claude'spassword"}}))
        while True:
            m = json.loads(await asyncio.wait_for(ws.recv(), 25))
            if m.get("type") in ("success", "failure"):
                break
        r = await rpc(ws, {"type": "request", "request": "list_files",
                           "data": {"table": "workorders", "key_type": "id", "key": KEY}})
        return {x["filename"].lstrip("/"): x for x in r.get("files", [])
                if not x.get("ephemeral")}

    async def run():
        async with websockets.connect(SRV, max_size=None) as ws:
            return await f(ws)
    return asyncio.run(run())


def main():
    rec = [d for d in os.listdir(ROOT) if "Lot 22" in d][0]
    pa, pb = os.path.join(ROOT, rec, A), os.path.join(ROOT, rec, B)

    for p in (pa, pb):
        with open(p, "wb") as f:
            f.write(SAME)
    time.sleep(3)
    rows = files()
    ra, rb = rows.get(A), rows.get(B)
    check("both_created", ra is not None and rb is not None, sorted(rows))
    if not (ra and rb):
        sys.exit(1)
    check("DEDUP_share_one_blob", ra["location"] == rb["location"],
          f"{ra['location'][:20]} vs {rb['location'][:20]}")
    print(f"shared blob: {ra['location'][:24]}...")

    # edit ONE of them
    edited = SAME + b"\nONLY A WAS EDITED"
    with open(pa, "wb") as f:
        f.write(edited)
    time.sleep(3)
    rows = files()
    ra2, rb2 = rows.get(A), rows.get(B)
    check("edited_row_repointed",
          ra2["location"] == "sha256:" + hashlib.sha256(edited).hexdigest(), ra2["location"])
    check("OTHER_ROW_UNTOUCHED", rb2["location"] == rb["location"],
          f"b moved from {rb['location'][:16]} to {rb2['location'][:16]}")
    check("other_file_still_reads_original", open(pb, "rb").read() == SAME)
    check("edited_file_reads_new", open(pa, "rb").read() == edited)

    for p in (pa, pb):
        try:
            os.remove(p)
        except OSError:
            pass
    time.sleep(2)
    print()
    print(f"{len(passed)} passed, {len(failed)} failed")
    sys.exit(1 if failed else 0)


main()
