"""F1: a row whose blob the file service does not have.

Creates a deliberately dangling attachment (valid sha256 location, bytes never
uploaded), then checks the mount reports something truthful instead of a
device error, marks the file offline, and does not re-attempt the doomed
fetch on every access.
"""
import asyncio, ctypes, hashlib, json, os, sys, time
import websockets

SRV = "ws://127.0.0.1:17243/"
KEY = 50049
ROOT = "S:\\workorders"
NAME = "dangling-check.bin"
passed, failed = [], []


def check(n, c, d=""):
    (passed if c else failed).append(n)
    print(("PASS " if c else "FAIL ") + n + (("  -- " + str(d)[:220]) if not c else ""))


_id = [300]


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


async def _with(fn):
    async with websockets.connect(SRV, max_size=None) as ws:
        await ws.send(json.dumps({"type": "login", "id": 1,
                                  "data": {"username": "claude",
                                           "password_plain": "claude'spassword"}}))
        while True:
            m = json.loads(await asyncio.wait_for(ws.recv(), 25))
            if m.get("type") in ("success", "failure"):
                break
        return await fn(ws)


CONSOLE = "ws://127.0.0.1:17244"


async def _drain(w, quiet=1.0, overall=25):
    out = []
    loop = asyncio.get_event_loop()
    dl = loop.time() + overall
    while loop.time() < dl:
        try:
            out.append(str(await asyncio.wait_for(w.recv(), timeout=quiet)))
        except asyncio.TimeoutError:
            if out:
                break
    return "\n".join(out)


async def _lua(cmd):
    async with websockets.connect(CONSOLE, max_size=None, ping_interval=None) as w:
        await _drain(w, 0.6, 6)
        await w.send(json.dumps({"type": "auth", "username": "claude",
                                 "password": "claude'spassword"}))
        await _drain(w, 0.6, 8)
        await w.send(json.dumps({"type": "command", "command": cmd}))
        return await _drain(w, 1.2, 30)


def make_dangling(rec_dir):
    """The server verifies the blob exists at add_attachment time, so a
    dangling row cannot be minted directly. It arises when the bytes go away
    UNDER a live row -- a blob GC, a row recorded while the bridge was down,
    or (most often) a database shared by two file-service deployments with
    separate storage. Reproduce it by removing the blob after the fact."""
    path = os.path.join(rec_dir, NAME)
    with open(path, "wb") as fh:
        fh.write(b"content that will be removed from the file service\n")
    time.sleep(2)

    async def f(ws):
        r = await rpc(ws, {"type": "request", "request": "list_files",
                           "data": {"table": "workorders", "key_type": "id", "key": KEY}})
        for x in r.get("files", []):
            if x["filename"].lstrip("/") == NAME:
                return x
        return None
    row = asyncio.run(_with(f))
    if not row:
        return None, None
    loc = row["location"]
    out = asyncio.run(_lua('return fsd_remove("' + loc + '")'))
    # Drop our own cached copy too, else the mount serves it locally.
    hexpart = loc.split(":", 1)[1]
    cached = os.path.join(os.environ["LOCALAPPDATA"], "RecordFS", "cache",
                          hexpart[:2], hexpart)
    try:
        os.remove(cached)
    except OSError:
        pass
    return loc, out


def remove(name):
    async def f(ws):
        r = await rpc(ws, {"type": "request", "request": "list_files",
                           "data": {"table": "workorders", "key_type": "id", "key": KEY}})
        for x in r.get("files", []):
            if x["filename"].lstrip("/") == name:
                await rpc(ws, {"type": "request", "request": "delete_attachment",
                               "data": {"table": "workorders", "key_type": "id", "key": KEY,
                                        "attachment_guid": x["guid"]}})
                return True
        return False
    return asyncio.run(_with(f))


FILE_ATTRIBUTE_OFFLINE = 0x1000


def main():
    rec = [d for d in os.listdir(ROOT) if "Lot 22" in d][0]
    path = os.path.join(ROOT, rec, NAME)

    loc, removed = make_dangling(os.path.join(ROOT, rec))
    check("setup_row_now_dangling", loc is not None and "removed" in str(removed).lower(),
          removed)
    if not loc:
        sys.exit(2)
    print("dangling location:", loc[:22] + "...")
    time.sleep(2)

    try:
        # It is listed (the row is real; hiding it would hide the problem).
        check("listed_in_directory", NAME in os.listdir(os.path.join(ROOT, rec)))

        # Opening it reports something truthful -- not a device error.
        t0 = time.time()
        errno_seen = winerr = None
        try:
            with open(path, "rb") as fh:
                fh.read(16)
            opened = True
        except OSError as e:
            opened = False
            errno_seen, winerr = e.errno, getattr(e, "winerror", None)
        first = time.time() - t0
        check("open_fails", not opened, "it should not open: the bytes are gone")
        # 1117 = ERROR_IO_DEVICE (the misleading one we are moving away from)
        check("not_reported_as_device_error", winerr != 1117,
              f"winerror={winerr} (ERROR_IO_DEVICE reads as failing hardware)")
        print(f"   open error: winerror={winerr} errno={errno_seen}  ({first:.1f}s)")

        # Negative caching: the second attempt must not repeat the round trips.
        t0 = time.time()
        try:
            open(path, "rb").read(1)
        except OSError:
            pass
        second = time.time() - t0
        check("second_attempt_is_fast", second < max(0.5, first),
              f"first={first:.2f}s second={second:.2f}s (no negative caching?)")
        print(f"   second attempt: {second:.2f}s")

        # Once known-bad the file is marked offline rather than normal. Check
        # it through ENUMERATION (FindFirstFile), which is what Explorer uses
        # to draw the badge -- a direct GetFileAttributesW opens the file and
        # so legitimately fails for content that cannot be fetched.
        attrs = None
        for entry in os.scandir(os.path.join(ROOT, rec)):
            if entry.name == NAME:
                attrs = entry.stat(follow_symlinks=False).st_file_attributes
        check("marked_offline_in_listing",
              attrs is not None and bool(attrs & FILE_ATTRIBUTE_OFFLINE),
              f"attrs={attrs}")
    finally:
        remove(NAME)
        time.sleep(1)

    print()
    print(f"{len(passed)} passed, {len(failed)} failed")
    sys.exit(1 if failed else 0)


main()
