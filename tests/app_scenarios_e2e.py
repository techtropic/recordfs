"""The scenarios real applications produce: the Office/CAD save dance
(temp file promoted into place), a multi-MB file, and a concurrent-edit
conflict."""
import hashlib, json, os, sys, time, asyncio, websockets
SRV="ws://127.0.0.1:17243/"; KEY=50049; ROOT="S:\workorders"
passed, failed = [], []
def check(n,c,d=""):
    (passed if c else failed).append(n)
    print(("PASS " if c else "FAIL ")+n+(("  -- "+str(d)[:230]) if not c else ""))
_id=[900]
async def _rpc(ws,msg):
    _id[0]+=1; msg["id"]=_id[0]; await ws.send(json.dumps(msg))
    t0=time.time()
    while time.time()-t0<40:
        m=json.loads(await asyncio.wait_for(ws.recv(),40))
        if m.get("id")==_id[0]:
            if m.get("type")=="progress": continue
            return m
async def _sess(fn):
    async with websockets.connect(SRV,max_size=None) as ws:
        await ws.send(json.dumps({"type":"login","id":1,"data":{"username":"claude","password_plain":"claude'spassword"}}))
        while True:
            m=json.loads(await asyncio.wait_for(ws.recv(),25))
            if m.get("type") in ("success","failure"): break
        return await fn(ws)
def files(include_eph=False):
    async def f(ws):
        r=await _rpc(ws,{"type":"request","request":"list_files",
                         "data":{"table":"workorders","key_type":"id","key":KEY}})
        return {x["filename"].lstrip("/"): x for x in r.get("files",[])
                if include_eph or not x.get("ephemeral")}
    return asyncio.run(_sess(f))
def upload_and_repoint(guid, body):
    """Simulate ANOTHER client changing the file while ours is open."""
    async def f(ws):
        t=await _rpc(ws,{"type":"request","request":"get_file_token","data":{"op":"put"}})
        res=t["result"]
        import ssl, urllib.request, http.client
        url=res["urls"][0].replace("https://","")
        host,port=url.split(":")
        ctx=ssl._create_unverified_context()
        c=http.client.HTTPSConnection(host,int(port),context=ctx,timeout=30)
        c.request("PUT","/files",body,{"Authorization":"Bearer "+res["token"],
                                       "Content-Type":"application/octet-stream"})
        r=c.getresponse(); j=json.loads(r.read())
        u=await _rpc(ws,{"type":"request","request":"update_attachment_location",
                         "data":{"attachment_guid":guid,"location":j["hash"],"size":j["size"]}})
        return u.get("success"), j["hash"]
    return asyncio.run(_sess(f))

def main():
    rec=[d for d in os.listdir(ROOT) if "Lot 22" in d][0]; D=os.path.join(ROOT,rec)

    # ---- 1. Office/CAD save dance: lock file, temp file, rename into place
    lock=os.path.join(D,"~$book.xlsx")
    with open(lock,"wb") as f: f.write(b"lock")
    eph=files(include_eph=True)
    check("DANCE_lock_is_ephemeral", eph.get("~$book.xlsx",{}).get("ephemeral") is True,
          {k:v.get("ephemeral") for k,v in eph.items()})

    payload=b"XLSX-LIKE BODY " + os.urandom(4096)
    tmp=os.path.join(D,"ver1234.tmp")
    with open(tmp,"wb") as f: f.write(payload)
    final=os.path.join(D,"book.xlsx")
    os.rename(tmp, final)                      # ephemeral -> durable PROMOTION
    time.sleep(2)
    srv=files()
    e=srv.get("book.xlsx")
    check("DANCE_promoted_to_durable", e is not None, sorted(srv))
    if e:
        check("DANCE_promoted_bytes_match",
              e["location"]=="sha256:"+hashlib.sha256(payload).hexdigest(), e["location"])
    check("DANCE_temp_not_durable", "ver1234.tmp" not in srv, sorted(srv))
    check("DANCE_reads_back", os.path.exists(final) and open(final,"rb").read()==payload)
    os.remove(lock)

    # ---- 2. multi-MB file (streamed upload, not buffered)
    big=os.urandom(5*1024*1024)
    bigp=os.path.join(D,"big.bin")
    t0=time.time()
    with open(bigp,"wb") as f: f.write(big)
    dt=time.time()-t0
    time.sleep(3)
    srv=files()
    b=srv.get("big.bin")
    check("LARGE_uploaded", b is not None and b["size"]==len(big), b["size"] if b else "missing")
    if b:
        check("LARGE_hash_matches", b["location"]=="sha256:"+hashlib.sha256(big).hexdigest())
    check("LARGE_reads_back", open(bigp,"rb").read()==big)
    print(f"   (5 MB write took {dt:.1f}s)")

    # ---- 3. CONFLICT: our handle is open when someone else re-points the row
    conf=os.path.join(D,"shared.txt")
    with open(conf,"wb") as f: f.write(b"original")
    time.sleep(2)
    guid=files()["shared.txt"]["guid"]
    fh=open(conf,"r+b")                       # copy-up happens here
    fh.write(b"MINE-mine-mine")
    ok,_=upload_and_repoint(guid, b"THEIRS-theirs")   # concurrent change
    check("CONFLICT_peer_write_ok", ok is True)
    fh.close()                                 # our save lands now
    time.sleep(2)
    srv=files()
    conflicts=[k for k in srv if k.startswith("shared (conflict")]
    check("CONFLICT_copy_created", len(conflicts)==1, sorted(srv))
    check("CONFLICT_peer_version_kept",
          srv.get("shared.txt",{}).get("location")=="sha256:"+hashlib.sha256(b"THEIRS-theirs").hexdigest(),
          srv.get("shared.txt",{}).get("location"))

    # cleanup
    for name in ["book.xlsx","big.bin","shared.txt"]+conflicts:
        try: os.remove(os.path.join(D,name))
        except OSError as e: print("   cleanup", name, e)
    time.sleep(2)
    left=files()
    check("CLEANUP_only_original_left", set(left)=={"workorder-signed.pdf"}, sorted(left))
    print()
    print(f"{len(passed)} passed, {len(failed)} failed")
    sys.exit(1 if failed else 0)
main()
