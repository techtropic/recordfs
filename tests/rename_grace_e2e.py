"""Regression: a record renamed under a mounted drive stays reachable by its
former path (recordfs former-names grace map). Renames via the scdsrv Lua
console, restores on every exit path."""
import asyncio, hashlib, json, os, sys, time
import websockets

CONSOLE = "ws://127.0.0.1:17244"
ROOT = "S:\workorders"
KEY = 50049
passed, failed = [], []
def check(n, c, d=""):
    (passed if c else failed).append(n)
    print(("PASS " if c else "FAIL ") + n + (("  -- " + str(d)[:200]) if not c else ""))

async def drain(w, quiet=1.0, overall=25):
    out=[]; loop=asyncio.get_event_loop(); dl=loop.time()+overall
    while loop.time()<dl:
        try: out.append(str(await asyncio.wait_for(w.recv(), timeout=quiet)))
        except asyncio.TimeoutError:
            if out: break
    return "\n".join(out)

async def lua(cmd):
    async with websockets.connect(CONSOLE, max_size=None, ping_interval=None) as w:
        await drain(w, 0.6, 6)
        await w.send(json.dumps({"type":"auth","username":"claude","password":"claude'spassword"}))
        await drain(w, 0.6, 8)
        await w.send(json.dumps({"type":"command","command":cmd}))
        return await drain(w, 1.2, 30)

def last_result(out):
    """The scdsrv console streams PLAIN TEXT: `=> <value>` lines (the client
    agent console is the JSON one). Return the last result, unquoted."""
    val = None
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("=> "):
            val = line[3:].strip()
        elif '"text"' in line:                      # tolerate a JSON console
            try:
                j = json.loads(line)
                t = j.get("text", "")
                if t.startswith("=> "): val = t[3:].strip()
            except Exception:
                pass
    if val and len(val) >= 2 and val[0] == '"' and val[-1] == '"':
        val = val[1:-1]
    return val

def sha(p):
    h=hashlib.sha256()
    with open(p,"rb") as f:
        for c in iter(lambda: f.read(65536), b""): h.update(c)
    return h.hexdigest()

GET_DISPLAY = f'local o = get_object("workorders", {KEY}) return o and o:get_display() or "nil"'
def SET_LOT(v):
    return (f'local o = get_object("workorders", {KEY}) if not o then return "no object" end '
            f'local e = o:edit(false) e:set_field("addresslot", "{v}") '
            f'return "saved=" .. tostring(save(e))')

async def main():
    orig = last_result(await lua(GET_DISPLAY))
    print("original display:", repr(orig))
    if not orig or orig == "nil":
        print("could not read display:", out[:300]); sys.exit(2)

    old_dir = os.path.join(ROOT, orig)
    check("old_dir_present", os.path.isdir(old_dir), old_dir)
    if not os.path.isdir(old_dir): sys.exit(1)
    pdfs = [f for f in os.listdir(old_dir) if f.lower().endswith(".pdf")]
    check("record_has_pdf", bool(pdfs), os.listdir(old_dir))
    if not pdfs: sys.exit(1)
    baseline = sha(os.path.join(old_dir, pdfs[0]))
    print("baseline sha:", baseline[:16], "file:", pdfs[0])

    orig_lot = orig.split(" - ")[-1]
    new_lot = orig_lot + " ZZ"
    try:
        print("renaming via console...")
        print("  ", last_result(await lua(SET_LOT(new_lot))))
        newdisp = last_result(await lua(GET_DISPLAY))
        print("new display:", repr(newdisp))
        check("display_changed", newdisp and newdisp != orig, f"{orig!r} -> {newdisp!r}")
        if not newdisp or newdisp == orig: sys.exit(1)

        new_dir = os.path.join(ROOT, newdisp)
        print("waiting out the 30s namespace TTL...")
        appeared=False
        for _ in range(25):
            time.sleep(3)
            try:
                if os.path.isdir(new_dir): appeared=True; break
            except OSError: pass
        check("new_name_appears", appeared, new_dir)

        # NOTE: os.path.isdir()/read on a path already touched this session can
        # be answered from the Windows metadata cache WITHOUT reaching our
        # filesystem, so those two are reported but prove nothing on their own.
        print("  (cache-assisted, informational):",
              "isdir=", os.path.isdir(old_dir),
              "read_ok=", (lambda: (sha(os.path.join(old_dir, pdfs[0])) == baseline)
                           if os.path.isdir(old_dir) else False)())

        # THE TEETH: creating a NEW file forces a real Create through our
        # resolver -- it cannot be served from any cache -- and it is exactly
        # what the Office/CAD save dance does in the record's directory.
        def try_create(d, leaf):
            path = os.path.join(d, leaf)
            try:
                with open(path, "wb") as f: f.write(b"teeth")
                try: os.remove(path)
                except OSError: pass
                return True, ""
            except OSError as e:
                return False, f"{type(e).__name__}: {e}"

        ctl_ok, ctl_err = try_create(new_dir, "~$teeth.tmp")
        check("CONTROL_create_under_new_name", ctl_ok, ctl_err)
        if not ctl_ok:
            print("   control failed -> the ephemeral write plane is unavailable;")
            print("   the old-path result below would be inconclusive.")
        old_ok, old_err = try_create(old_dir, "~$teeth.tmp")
        check("OLD_PATH_ACCEPTS_CREATE (the real assertion)", old_ok, old_err)
    finally:
        print("restoring...")
        print("  ", last_result(await lua(SET_LOT(orig_lot))))
        print("display now:", repr(last_result(await lua(GET_DISPLAY))))

    print()
    print(f"{len(passed)} passed, {len(failed)} failed")
    sys.exit(1 if failed else 0)

asyncio.run(main())
