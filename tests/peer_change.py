"""Replace peerwatch.txt's content the way another machine would: upload a
new blob and re-point the attachment row. Never touches this machine's FS."""
import asyncio, http.client, json, ssl, time
import websockets

SRV = "ws://127.0.0.1:17243/"
KEY = 50049
NAME = "peerwatch.txt"
NEW = b"PEER REPLACED THIS CONTENT AND IT IS MUCH LONGER THAN THE SEED " + b"z" * 500
_id = [2000]


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


async def main():
    async with websockets.connect(SRV, max_size=None) as ws:
        await ws.send(json.dumps({"type": "login", "id": 1,
                                  "data": {"username": "claude",
                                           "password_plain": "claude'spassword"}}))
        while True:
            m = json.loads(await asyncio.wait_for(ws.recv(), 25))
            if m.get("type") in ("success", "failure"):
                break
        r = await rpc(ws, {"type": "request", "request": "list_files",
                           "data": {"table": "workorders", "key_type": "id", "key": KEY}})
        row = next((x for x in r.get("files", [])
                    if x["filename"].lstrip("/") == NAME), None)
        if not row:
            print("peer: file not found")
            return
        t = await rpc(ws, {"type": "request", "request": "get_file_token",
                           "data": {"op": "put"}})
        res = t["result"]
        host, port = res["urls"][0].replace("https://", "").split(":")
        c = http.client.HTTPSConnection(host, int(port),
                                        context=ssl._create_unverified_context(), timeout=30)
        c.request("PUT", "/files", NEW,
                  {"Authorization": "Bearer " + res["token"],
                   "Content-Type": "application/octet-stream"})
        j = json.loads(c.getresponse().read())
        u = await rpc(ws, {"type": "request", "request": "update_attachment_location",
                           "data": {"attachment_guid": row["guid"],
                                    "location": j["hash"], "size": j["size"]}})
        print(f"peer: re-pointed to {j['hash'][:20]}... size {j['size']} ok={u.get('success')}")

asyncio.run(main())
