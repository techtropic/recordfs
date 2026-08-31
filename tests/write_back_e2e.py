"""P2 write-back: create, modify, rename, delete through the mounted drive,
each verified SERVER-SIDE (list_files) not just locally."""
import hashlib, json, os, sys, time, asyncio, websockets

SRV="ws://127.0.0.1:17243/"; KEY=50049
ROOT="S:\workorders"
passed, failed = [], []
def check(n,c,d=""):
    (passed if c else failed).append(n)
    print(("PASS " if c else "FAIL ")+n+(("  -- "+str(d)[:200]) if not c else ""))

_id=[500]
async def rpc(ws,msg):
    _id[0]+=1; msg["id"]=_id[0]; await ws.send(json.dumps(msg))
    t0=time.time()
    while time.time()-t0<30:
        m=json.loads(await asyncio.wait_for(ws.recv(),30))
        if m.get("id")==_id[0]:
            if m.get("type")=="progress": continue
            return m
async def server_files():
    async with websockets.connect(SRV,max_size=None) as ws:
        await ws.send(json.dumps({"type":"login","id":1,"data":{"username":"claude","password_plain":"claude'spassword"}}))
        while True:
            m=json.loads(await asyncio.wait_for(ws.recv(),25))
            if m.get("type") in ("success","failure"): break
        r=await rpc(ws,{"type":"request","request":"list_files",
                        "data":{"table":"workorders","key_type":"id","key":KEY}})
        return {f["filename"].lstrip("/"): f for f in r.get("files",[]) if not f.get("ephemeral")}

def sha(p):
    h=hashlib.sha256()
    with open(p,"rb") as f:
        for c in iter(lambda: f.read(65536), b""): h.update(c)
    return h.hexdigest()

def main():
    rec = [d for d in os.listdir(ROOT) if "Lot 22" in d][0]
    D = os.path.join(ROOT, rec)
    print("record dir:", rec)

    # ---- 1. CREATE a durable file through the drive
    body = b"RecordFS P2 write-back test\n" + os.urandom(512)
    newp = os.path.join(D, "p2-created.txt")
    with open(newp, "wb") as f: f.write(body)
    time.sleep(2)
    srv = asyncio.run(server_files())
    e = srv.get("p2-created.txt")
    check("CREATE_registered_server_side", e is not None, sorted(srv))
    if e:
        check("CREATE_content_addressed", e["location"].startswith("sha256:"), e["location"])
        check("CREATE_hash_matches_bytes",
              e["location"] == "sha256:"+hashlib.sha256(body).hexdigest(), e["location"])
        check("CREATE_size_correct", e["size"] == len(body), e["size"])
    check("CREATE_readable_back", os.path.exists(newp) and open(newp,"rb").read() == body)

    # ---- 2. MODIFY an existing durable file
    body2 = body + b"\nAPPENDED BY THE SECOND SAVE"
    with open(newp, "wb") as f: f.write(body2)
    time.sleep(2)
    srv = asyncio.run(server_files())
    e2 = srv.get("p2-created.txt")
    check("MODIFY_repointed", e2 is not None and
          e2["location"] == "sha256:"+hashlib.sha256(body2).hexdigest(),
          e2["location"] if e2 else "missing")
    check("MODIFY_size_updated", e2 and e2["size"] == len(body2), e2["size"] if e2 else "")
    check("MODIFY_guid_stable", e2 and e and e2["guid"] == e["guid"], "guid changed = new row, not a re-point")
    check("MODIFY_readable_back", open(newp,"rb").read() == body2)

    # ---- 3. NO-OP save (same bytes) must not churn
    loc_before = e2["location"]
    with open(newp, "wb") as f: f.write(body2)
    time.sleep(2)
    srv = asyncio.run(server_files())
    check("NOOP_save_no_change", srv["p2-created.txt"]["location"] == loc_before)

    # ---- 4. RENAME (the back half of every app's save dance)
    ren = os.path.join(D, "p2-renamed.txt")
    os.rename(newp, ren)
    time.sleep(2)
    srv = asyncio.run(server_files())
    check("RENAME_new_name_present", "p2-renamed.txt" in srv, sorted(srv))
    check("RENAME_old_name_gone", "p2-created.txt" not in srv, sorted(srv))
    check("RENAME_content_preserved",
          srv.get("p2-renamed.txt",{}).get("location") == loc_before)

    # ---- 5. NESTED path create
    sub = os.path.join(D, "p2sub")
    try: os.makedirs(sub, exist_ok=True)
    except OSError as ex: print("  (mkdir:", ex, ")")
    nested = os.path.join(sub, "nested.txt")
    ok_nested = True
    try:
        with open(nested,"wb") as f: f.write(b"nested body")
    except OSError as ex:
        ok_nested = False; print("  nested create failed:", ex)
    time.sleep(2)
    srv = asyncio.run(server_files())
    check("NESTED_create", ok_nested and "p2sub/nested.txt" in srv, sorted(srv))

    # ---- 6. DELETE (soft delete server-side)
    os.remove(ren)
    if ok_nested:
        try: os.remove(nested)
        except OSError: pass
    time.sleep(2)
    srv = asyncio.run(server_files())
    check("DELETE_removed_server_side", "p2-renamed.txt" not in srv, sorted(srv))

    # remove the directory too, so the suite leaves the record as it found it
    try: os.rmdir(sub)
    except OSError as ex: print("   rmdir:", ex)
    time.sleep(2)
    srv = asyncio.run(server_files())
    check("DIR_removed", "p2sub" not in srv, sorted(srv))

    # ---- 7. the original attachment is untouched throughout
    check("ORIGINAL_untouched", "workorder-signed.pdf" in srv, sorted(srv))
    print()
    print(f"{len(passed)} passed, {len(failed)} failed")
    sys.exit(1 if failed else 0)

main()
