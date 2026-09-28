"""WS-level check of the table write gate as the read-only user rotest:
list_tables can_write flags, and a rejected add_attachment on quote vs an
accepted one on workorders."""
import asyncio, hashlib, json, os, sys, time
import websockets
SRV=os.environ.get("RECORDFS_TEST_SERVER", "ws://127.0.0.1:17243/")
TOK=open("C:/Users/marcm/AppData/Local/Temp/rotest_tok.txt").read().strip()
passed,failed=[],[]
def check(n,c,d=""):
    (passed if c else failed).append(n); print(("PASS " if c else "FAIL ")+n+(("  -- "+str(d)[:200]) if not c else ""))
_id=[600]
async def rpc(ws,msg):
    _id[0]+=1; msg["id"]=_id[0]; await ws.send(json.dumps(msg))
    t0=time.time()
    while time.time()-t0<30:
        m=json.loads(await asyncio.wait_for(ws.recv(),30))
        if m.get("id")==_id[0]:
            if m.get("type")=="progress": continue
            return m
async def upload_blob(ws, body):
    t=await rpc(ws,{"type":"request","request":"get_file_token","data":{"op":"put"}})
    res=t["result"]; import ssl,http.client
    host,port=res["urls"][0].replace("https://","").split(":")
    c=http.client.HTTPSConnection(host,int(port),context=ssl._create_unverified_context(),timeout=30)
    c.request("PUT","/files",body,{"Authorization":"Bearer "+res["token"],"Content-Type":"application/octet-stream"})
    return json.loads(c.getresponse().read())["hash"], len(body)
async def find_key(ws, table, key_type):
    r=await rpc(ws,{"type":"request","request":"list_objects","data":{"table":table,"count":1}})
    objs=r.get("objects",[])
    if not objs: return None
    o=objs[0]
    return o["id"] if key_type=="id" else o["key"]
async def main():
    async with websockets.connect(SRV,max_size=None) as ws:
        await ws.send(json.dumps({"type":"login","id":1,"data":{"session_type":"file","client_token":TOK}}))
        while True:
            m=json.loads(await asyncio.wait_for(ws.recv(),20))
            if m.get("type") in ("success","failure"):
                assert m["type"]=="success"; break
        r=await rpc(ws,{"type":"request","request":"list_tables","data":{}})
        tw={t["table"]: t.get("can_write") for t in r.get("tables",[])}
        print("list_tables can_write:", tw)
        check("quote_reported_readonly", tw.get("quote") is False, tw)
        check("workorders_reported_writable", tw.get("workorders") is True, tw)
        # try to add on quote (should be denied)
        qkey=await find_key(ws,"quote","guid")
        h,sz=await upload_blob(ws, b"rotest-should-not-write "+os.urandom(16))
        r=await rpc(ws,{"type":"request","request":"add_attachment",
            "data":{"table":"quote","key_type":"guid","key":qkey,"filename":"/rotest-denied.txt","location":h,"mimetype":"text/plain","size":sz}})
        check("quote_add_denied", r.get("success") is False and "read-only" in str(r.get("error","")), r)
        # add on workorders (should succeed)
        wkey=await find_key(ws,"workorders","id")
        h2,sz2=await upload_blob(ws, b"rotest-allowed-on-workorders "+os.urandom(16))
        r=await rpc(ws,{"type":"request","request":"add_attachment",
            "data":{"table":"workorders","key_type":"id","key":wkey,"filename":"/rotest-allowed.txt","location":h2,"mimetype":"text/plain","size":sz2}})
        wo_ok = r.get("success") is True
        check("workorders_add_allowed", wo_ok, r)
        # cleanup the workorders one
        if wo_ok:
            lf=await rpc(ws,{"type":"request","request":"list_files","data":{"table":"workorders","key_type":"id","key":wkey}})
            for f in lf.get("files",[]):
                if f["filename"].lstrip("/")=="rotest-allowed.txt":
                    await rpc(ws,{"type":"request","request":"delete_attachment","data":{"table":"workorders","key_type":"id","key":wkey,"attachment_guid":f["guid"]}})
        print(); print(f"{len(passed)} passed, {len(failed)} failed")
        sys.exit(1 if failed else 0)
asyncio.run(main())
