"""What a SECOND reader sees when someone else saves the file it has open.

Answers, concretely:
  - does an already-open handle follow the new content hash?
  - does the handle's own metadata (fstat) change?
  - does a fresh stat BY PATH see the new size/mtime?
  - does a fresh open get the new bytes?
"""
import asyncio, hashlib, http.client, json, os, ssl, sys, time
import websockets

SRV = "ws://127.0.0.1:17243/"
KEY = 50049
ROOT = "S:\\workorders"
NAME = "multiread-check.txt"
V1 = b"VERSION-ONE original content\n" + b"a" * 200
V2 = b"VERSION-TWO peer edited this, and it is a different length\n" + b"b" * 900

passed, failed = [], []
def check(n, c, d=""):
    (passed if c else failed).append(n)
    print(("PASS " if c else "FAIL ") + n + (("  -- " + str(d)[:200]) if not c else ""))

_id = [1000]
async def rpc(ws, msg):
    _id[0] += 1
    msg["id"] = _id[0]
    await ws.send(json.dumps(msg))
    t0 = time.time()
    while time.time() - t0 < 40:
        m = json.loads(await asyncio.wait_for(ws.recv(), 40))
        if m.get("id") == _id[0]:
            if m.get("type") == "progress":
                continue
            return m

async def _sess(fn):
    async with websockets.connect(SRV, max_size=None) as ws:
        await ws.send(json.dumps({"type": "login", "id": 1,
                                  "data": {"username": "claude",
                                           "password_plain": "claude'spassword"}}))
        while True:
            m = json.loads(await asyncio.wait_for(ws.recv(), 25))
            if m.get("type") in ("success", "failure"):
                break
        return await fn(ws)

def files():
    async def f(ws):
        r = await rpc(ws, {"type": "request", "request": "list_files",
                           "data": {"table": "workorders", "key_type": "id", "key": KEY}})
        return {x["filename"].lstrip("/"): x for x in r.get("files", [])
                if not x.get("ephemeral")}
    return asyncio.run(_sess(f))

def peer_replace(guid, body):
    """A DIFFERENT client uploads new content and re-points the row."""
    async def f(ws):
        t = await rpc(ws, {"type": "request", "request": "get_file_token",
                           "data": {"op": "put"}})
        res = t["result"]
        host, port = res["urls"][0].replace("https://", "").split(":")
        c = http.client.HTTPSConnection(host, int(port),
                                        context=ssl._create_unverified_context(), timeout=30)
        c.request("PUT", "/files", body,
                  {"Authorization": "Bearer " + res["token"],
                   "Content-Type": "application/octet-stream"})
        j = json.loads(c.getresponse().read())
        u = await rpc(ws, {"type": "request", "request": "update_attachment_location",
                           "data": {"attachment_guid": guid, "location": j["hash"],
                                    "size": j["size"]}})
        return u.get("success"), j["hash"]
    return asyncio.run(_sess(f))

def main():
    rec = [d for d in os.listdir(ROOT) if "Lot 22" in d][0]
    path = os.path.join(ROOT, rec, NAME)

    with open(path, "wb") as f:
        f.write(V1)
    time.sleep(2)
    row = files().get(NAME)
    check("setup_created", row is not None and row["size"] == len(V1), row)
    if not row:
        sys.exit(1)
    guid, h1 = row["guid"], row["location"]
    print(f"v1 hash {h1[:20]}... size {row['size']}")

    # READER holds the file open, as a viewer would
    reader = open(path, "rb")
    first = reader.read(16)
    check("reader_open_sees_v1", first == V1[:16], first)
    size_at_open = os.fstat(reader.fileno()).st_size

    # a peer saves a new version
    ok, h2 = peer_replace(guid, V2)
    check("peer_saved", ok is True and h2 != h1, (ok, h2[:20]))
    print(f"v2 hash {h2[:20]}... size {len(V2)}")

    # 1. the already-open handle
    reader.seek(0)
    via_handle = reader.read()
    check("OPEN_HANDLE_still_reads_v1", via_handle == V1,
          f"got {len(via_handle)} bytes, expected {len(V1)}")
    check("OPEN_HANDLE_fstat_unchanged", os.fstat(reader.fileno()).st_size == size_at_open,
          f"{os.fstat(reader.fileno()).st_size} vs {size_at_open}")

    # 2. a fresh stat BY PATH, after the namespace TTL rolls
    print("waiting out the 10s file-tree TTL...")
    new_size = None
    for _ in range(12):
        time.sleep(2)
        try:
            new_size = os.stat(path).st_size
            if new_size == len(V2):
                break
        except OSError:
            pass
    check("FRESH_STAT_sees_new_size", new_size == len(V2),
          f"stat says {new_size}, peer wrote {len(V2)}")

    # 3. a fresh open gets the new bytes
    with open(path, "rb") as f2:
        via_new = f2.read()
    check("FRESH_OPEN_reads_v2", via_new == V2,
          f"got {len(via_new)} bytes, expected {len(V2)}")

    reader.close()

    # cleanup
    try:
        os.remove(path)
    except OSError as e:
        print("cleanup:", e)
    time.sleep(2)
    check("cleanup", NAME not in files())

    print()
    print(f"{len(passed)} passed, {len(failed)} failed")
    sys.exit(1 if failed else 0)

main()
