"""Case-insensitive names, and opens that must not download anything.

The drive is case-insensitive like any Windows volume, so a path typed in a
different case must reach the stored file -- and a save through it must not
create a second attachment beside the first. Opens that touch no bytes
(rename, delete, stat) must not hydrate the file.

Setup: a mount of a server WITH a file service (write paths upload), on --a
(default S:), with its cache directory passed as --cache (the mount's
%LOCALAPPDATA%\\RecordFS\\cache), and record 50049 in workorders holding
workorder-signed.pdf. The no-download case borrows the largest daemon-backed
jpg on record --donor (default 50065) by adding a second row that points at
the same content, and removes it again. Every assertion that matters is
checked server-side via list_files.
"""
import argparse, asyncio, ctypes, json, os, sys, time
from ctypes import wintypes

import websockets

p = argparse.ArgumentParser()
p.add_argument("--server", default="wss://testing.scheduler.techtropic.ca:7243/")
p.add_argument("--a", default="S:")
p.add_argument("--cache", required=True)
p.add_argument("--donor", type=int, default=50065)
args = p.parse_args()

KEY = 50049
PDF = "workorder-signed.pdf"
GR, GW = 0x80000000, 0x40000000
CREATE_NEW, OPEN_ALWAYS = 1, 4
ERROR_FILE_EXISTS, ERROR_ALREADY_EXISTS = 80, 183

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = wintypes.HANDLE
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                            wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
INVALID = wintypes.HANDLE(-1).value

passed, failed = [], []


def check(name, cond, detail=""):
    (passed if cond else failed).append(name)
    print(("PASS " if cond else "FAIL ") + name + (("  -- " + str(detail)[:200]) if not cond else ""))


class Srv:
    def __init__(self):
        self.n = 100
        self.loop = asyncio.new_event_loop()
        self.ws = self.loop.run_until_complete(self._login())

    async def _login(self):
        ws = await websockets.connect(args.server, max_size=None)
        await ws.send(json.dumps({"type": "login", "id": 1, "data": {
            "username": "claude", "password_plain": "claude'spassword"}}))
        while True:
            m = json.loads(await asyncio.wait_for(ws.recv(), 25))
            if m.get("type") in ("success", "failure"):
                return ws

    def req(self, name, data):
        self.n += 1
        i = self.n

        async def go():
            await self.ws.send(json.dumps({"type": "request", "request": name, "id": i,
                                           "data": data}))
            while True:
                m = json.loads(await asyncio.wait_for(self.ws.recv(), 25))
                if m.get("id") == i and m.get("type") != "progress":
                    return m
        return self.loop.run_until_complete(go())

    def close(self):
        self.loop.run_until_complete(self.ws.close())
        self.loop.close()

    def files(self, key=KEY):
        r = self.req("list_files", {"table": "workorders", "key_type": "id", "key": key})
        return [f for f in r.get("files", []) if not f.get("ephemeral")]

    def named(self, name, key=KEY):
        return [f for f in self.files(key) if f["filename"].lstrip("/").lower() == name.lower()]


def cached(location):
    h = location.split(":", 1)[1]
    return os.path.exists(os.path.join(args.cache, h[:2], h))


def until(fn, timeout=15.0, step=0.5):
    t0 = time.time()
    while time.time() - t0 < timeout:
        r = fn()
        if r:
            return r
        time.sleep(step)
    return None


def main():
    srv = Srv()
    root = args.a + "\\workorders"
    rec = [d for d in os.listdir(root) if "Lot 22" in d][0]
    d = os.path.join(root, rec)
    before = srv.named(PDF)
    check("fixture_present", len(before) == 1, before)
    if len(before) != 1:
        sys.exit(1)

    # --- reads through another spelling --------------------------------------
    upper = os.path.join(d, PDF.upper())
    try:
        with open(upper, "rb") as f:
            head = f.read(4)
    except OSError as e:
        head = e
    check("UPPER_CASE_PATH_READS", head == b"%PDF", head)
    try:
        size = os.stat(upper).st_size
    except OSError as e:
        size = e
    check("UPPER_CASE_PATH_STATS", size == before[0]["size"], size)
    names = os.listdir(d)
    check("listing_keeps_stored_case", PDF in names and PDF.upper() not in names, names)

    # --- CREATE_NEW on an existing name, in another case -----------------------
    h = k32.CreateFileW(upper, GR | GW, 0, None, CREATE_NEW, 0x80, None)
    e = ctypes.get_last_error()
    if h != INVALID:
        k32.CloseHandle(h)
    check("CREATE_NEW_on_existing_name_collides", h == INVALID and e == ERROR_FILE_EXISTS, e)

    # --- OPEN_ALWAYS through another case opens the SAME attachment ------------
    h = k32.CreateFileW(os.path.join(d, "Workorder-Signed.PDF"), GR | GW, 1, None, OPEN_ALWAYS,
                        0x80, None)
    e = ctypes.get_last_error()
    check("OPEN_ALWAYS_opens_existing", h != INVALID and e == ERROR_ALREADY_EXISTS, e)
    if h != INVALID:
        k32.CloseHandle(h)
    time.sleep(1.0)
    after = srv.named(PDF)
    check("NO_DUPLICATE_ROW", len(after) == 1 and after[0]["guid"] == before[0]["guid"]
          and after[0]["location"] == before[0]["location"], after)

    # --- new files land in the folder's stored spelling -------------------------
    os.mkdir(os.path.join(d, "CaseDir"))
    with open(os.path.join(d, "CASEDIR", "note.txt"), "wb") as f:
        f.write(b"filed under CaseDir")
    time.sleep(1.5)
    rows = [f["filename"].lstrip("/") for f in srv.files()]
    check("NEW_FILE_USES_STORED_FOLDER_SPELLING", "CaseDir/note.txt" in rows
          and not any(r.startswith("CASEDIR/") for r in rows), [r for r in rows if "note" in r])
    try:
        os.mkdir(os.path.join(d, "casedir"))
        mk = "created a second folder"
    except FileExistsError:
        mk = "exists"
    except OSError as ex:
        mk = ex
    check("mkdir_existing_folder_other_case_refused", mk == "exists", mk)
    check("one_folder_in_listing", [n for n in os.listdir(d) if n.lower() == "casedir"] == ["CaseDir"],
          os.listdir(d))

    # --- a case-only rename renames; it must not delete the file ---------------
    a = os.path.join(d, "case-rename.txt")
    with open(a, "wb") as f:
        f.write(b"rename me by case only")
    time.sleep(1.5)
    orig = srv.named("case-rename.txt")
    os.rename(a, os.path.join(d, "Case-Rename.txt"))
    time.sleep(1.0)
    now = srv.named("case-rename.txt")
    check("CASE_ONLY_RENAME_KEEPS_THE_FILE", len(now) == 1 and orig and
          now[0]["guid"] == orig[0]["guid"] and now[0]["filename"].lstrip("/") == "Case-Rename.txt",
          now)
    try:
        with open(os.path.join(d, "Case-Rename.txt"), "rb") as f:
            check("renamed_content_intact", f.read() == b"rename me by case only")
    except OSError as ex:
        check("renamed_content_intact", False, ex)

    # --- rename / stat / delete download nothing -------------------------------
    donors = [f for f in srv.files(args.donor)
              if f["location"].startswith("sha256:") and f["filename"].lower().endswith(".jpg")]
    donor = max(donors, key=lambda f: f["size"]) if donors else None
    if donor and cached(donor["location"]):
        # Content-addressed: dropping a cached copy only costs a re-fetch.
        h = donor["location"].split(":", 1)[1]
        os.remove(os.path.join(args.cache, h[:2], h))
    check("donor_blob_available_and_uncached", donor is not None and not cached(donor["location"]),
          "no daemon-backed jpg on the donor record")
    if donor:
        r = srv.req("add_attachment", {"table": "workorders", "key_type": "id", "key": KEY,
                                       "filename": "/nohydrate.jpg", "location": donor["location"],
                                       "mimetype": "image/jpeg", "size": donor["size"]})
        check("donor_row_added", r.get("success") is True, r)
        src, dst = os.path.join(d, "nohydrate.jpg"), os.path.join(d, "nohydrate-moved.jpg")
        until(lambda: os.path.exists(src))
        guid = (srv.named("nohydrate.jpg") or [{}])[0].get("guid")
        try:
            os.stat(src)
            os.rename(src, dst)
            ren = "ok"
        except OSError as ex:
            ren = ex
        check("RENAME_WITHOUT_CONTENT_SUCCEEDS", ren == "ok", ren)
        moved = srv.named("nohydrate-moved.jpg")
        check("renamed_server_side", len(moved) == 1 and moved[0]["guid"] == guid, moved)
        check("RENAME_DOWNLOADED_NOTHING", not cached(donor["location"]),
              f"{donor['size']} bytes fetched")
        try:
            os.remove(dst)
            rm = "ok"
        except OSError as ex:
            rm = ex
        check("DELETE_WITHOUT_CONTENT_SUCCEEDS", rm == "ok", rm)
        time.sleep(1.0)
        check("deleted_server_side", not srv.named("nohydrate-moved.jpg"), srv.named("nohydrate-moved.jpg"))
        check("DELETE_DOWNLOADED_NOTHING", not cached(donor["location"]))
        for leftover in srv.named("nohydrate.jpg") + srv.named("nohydrate-moved.jpg"):
            srv.req("delete_attachment", {"table": "workorders", "key_type": "id", "key": KEY,
                                          "attachment_guid": leftover["guid"]})

    # --- leave the record as found ----------------------------------------------
    for leaf in ("Case-Rename.txt", "case-rename.txt", os.path.join("CaseDir", "note.txt")):
        try:
            os.remove(os.path.join(d, leaf))
        except OSError:
            pass
    try:
        os.rmdir(os.path.join(d, "CaseDir"))
    except OSError:
        pass
    time.sleep(1.5)
    left = [f["filename"] for f in srv.files() if f["filename"].lstrip("/").lower() != PDF]
    check("record_left_as_found", not left, left)
    srv.close()

    print()
    print(f"{len(passed)} passed, {len(failed)} failed")
    sys.exit(1 if failed else 0)


main()
