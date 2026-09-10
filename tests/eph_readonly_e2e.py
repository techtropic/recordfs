"""The lock-file asymmetry on a read-only table.

A user who may READ a table but not write it should still be able to create
an ephemeral lock file there (so an editor knows the record is open), while
durable attachment writes stay refused. Run as rotest (read-only on quote).
"""
import asyncio, json, os, sys, time
import websockets

SRV = "ws://127.0.0.1:17243/"
passed, failed = [], []


def check(n, c, d=""):
    (passed if c else failed).append(n)
    print(("PASS " if c else "FAIL ") + n + (("  -- " + str(d)[:200]) if not c else ""))


_id = [3000]


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


async def main():
    tok = open(os.path.join(os.environ["TEMP"], "rotest_tok.txt")).read().strip()
    async with websockets.connect(SRV, max_size=None) as ws:
        await ws.send(json.dumps({"type": "login", "id": 1,
                                  "data": {"session_type": "file", "client_token": tok}}))
        while True:
            m = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if m.get("type") in ("success", "failure"):
                assert m["type"] == "success", m
                break

        r = await rpc(ws, {"type": "request", "request": "list_tables", "data": {}})
        tw = {t["table"]: t.get("can_write") for t in r.get("tables", [])}
        check("quote_is_readonly_for_this_user", tw.get("quote") is False, tw)

        objs = (await rpc(ws, {"type": "request", "request": "list_objects",
                               "data": {"table": "quote", "count": 1}})).get("objects", [])
        if not objs:
            print("no quote records visible")
            sys.exit(2)
        key = objs[0]["key"]
        tgt = {"table": "quote", "key_type": "guid", "key": key}

        # a lock file IS allowed on a read-only table
        r = await rpc(ws, {"type": "request", "request": "ephemeral_put",
                           "data": {**tgt, "path": "/drawing.dwl", "data": "bG9ja2VkIGJ5IHJvdGVzdA=="}})
        check("lock_file_allowed_on_readonly_table", r.get("success") is True, r)

        # and other clients can see it
        r = await rpc(ws, {"type": "request", "request": "list_files", "data": tgt})
        eph = [f for f in r.get("files", []) if f.get("ephemeral")]
        check("lock_file_visible_to_others",
              any(f["filename"] == "/drawing.dwl" for f in eph), eph)

        # but a durable attachment write is still refused
        r = await rpc(ws, {"type": "request", "request": "add_attachment",
                           "data": {**tgt, "filename": "/should-not-exist.txt",
                                    "location": "sha256:" + "0" * 64,
                                    "mimetype": "text/plain", "size": 1}})
        check("durable_write_still_refused", r.get("success") is False, r)

        await rpc(ws, {"type": "request", "request": "ephemeral_delete",
                       "data": {**tgt, "path": "/drawing.dwl"}})

    print()
    print(f"{len(passed)} passed, {len(failed)} failed")
    sys.exit(1 if failed else 0)

asyncio.run(main())
