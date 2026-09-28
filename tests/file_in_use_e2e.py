"""File-in-use signaling across machines (protocol 4.5, file leases).

Two mounts of the same server stand in for two machines: separate processes,
separate sessions, separate volumes -- the Windows kernel cannot arbitrate
between them, so every refusal below can only come from the server.

Setup: a server at ws://127.0.0.1:17243/ with file-lease support (a file
service is NOT required -- nothing here uploads), mounted twice:

    recordfs agent-run --profile leaseA     (drive S:)
    recordfs agent-run --profile leaseB     (drive T:)

and record 50049 in workorders holding workorder-signed.pdf with its content
hydratable on both mounts. Every file handle is opened with explicit Win32
access/share modes (ctypes), the way Office and CAD open documents.

Optional: --crash <pid of mount A's recordfs> kills mount A while it holds a
file open, and asserts mount B can edit it at once (the session died, so its
leases did too). Run it last; it leaves mount A down.
"""
import argparse, asyncio, ctypes, json, os, struct, subprocess, sys, time
from ctypes import wintypes

import websockets

p = argparse.ArgumentParser()
p.add_argument("--a", default="S:")
p.add_argument("--b", default="T:")
p.add_argument("--crash", type=int, default=0, help="pid of mount A's recordfs process")
args = p.parse_args()

SRV = os.environ.get("RECORDFS_TEST_SERVER", "ws://127.0.0.1:17243/")
KEY = 50049
PDF = "workorder-signed.pdf"

GR, GW, DEL = 0x80000000, 0x40000000, 0x00010000
SHARE_R, SHARE_W, SHARE_D = 1, 2, 4
CREATE_NEW, CREATE_ALWAYS, OPEN_EXISTING = 1, 2, 3
SHARING_VIOLATION, ACCESS_DENIED = 32, 5

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = wintypes.HANDLE
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                            wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
k32.DeleteFileW.argtypes = [wintypes.LPCWSTR]
k32.MoveFileExW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD]
k32.SetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                           wintypes.DWORD]
INVALID = wintypes.HANDLE(-1).value

passed, failed = [], []


def check(name, cond, detail=""):
    (passed if cond else failed).append(name)
    print(("PASS " if cond else "FAIL ") + name + (("  -- " + str(detail)[:200]) if not cond else ""))


def open_(path, access, share, disp=OPEN_EXISTING):
    h = k32.CreateFileW(path, access, share, None, disp, 0x80, None)
    if h == INVALID:
        return None, ctypes.get_last_error()
    return h, 0


def close(*hs):
    for h in hs:
        if h:
            k32.CloseHandle(h)


def read(h, n):
    buf = ctypes.create_string_buffer(n)
    got = wintypes.DWORD()
    k32.ReadFile(h, buf, n, ctypes.byref(got), None)
    return buf.raw[:got.value]


def write(h, data):
    got = wintypes.DWORD()
    return bool(k32.WriteFile(h, data, len(data), ctypes.byref(got), None)) and got.value == len(data)


def rename_open_handle(h, new_path):
    """Rename through an OPEN handle (the handle stays open afterwards).
    new_path must be a FULL path: kernel32 resolves a bare name against the
    process's current directory, i.e. usually another volume (error 17)."""
    name = new_path.encode("utf-16-le")
    buf = struct.pack("<I4xQI", 1, 0, len(name)) + name + b"\0\0"
    raw = ctypes.create_string_buffer(buf, len(buf) + 8)
    ok = k32.SetFileInformationByHandle(h, 3, raw, len(buf))   # FileRenameInfo
    return bool(ok), ctypes.get_last_error()


def server_files():
    async def run():
        async with websockets.connect(SRV, max_size=None) as ws:
            await ws.send(json.dumps({"type": "login", "id": 1,
                                      "data": {"username": "claude",
                                               "password_plain": "claude'spassword"}}))
            while True:
                m = json.loads(await asyncio.wait_for(ws.recv(), 25))
                if m.get("type") in ("success", "failure"):
                    break
            await ws.send(json.dumps({"type": "request", "request": "list_files", "id": 9,
                                      "data": {"table": "workorders", "key_type": "id",
                                               "key": KEY}}))
            while True:
                m = json.loads(await asyncio.wait_for(ws.recv(), 25))
                if m.get("id") == 9 and m.get("type") != "progress":
                    return {x["filename"].lstrip("/"): x for x in m.get("files", [])}
    return asyncio.run(run())


def until(fn, timeout=6.0, step=0.2):
    t0 = time.time()
    while time.time() - t0 < timeout:
        r = fn()
        if r:
            return r, time.time() - t0
        time.sleep(step)
    return None, time.time() - t0


def main():
    root_a, root_b = args.a + "\\workorders", args.b + "\\workorders"
    rec = [d for d in os.listdir(root_a) if "Lot 22" in d][0]
    da, db = os.path.join(root_a, rec), os.path.join(root_b, rec)
    pa, pb = os.path.join(da, PDF), os.path.join(db, PDF)
    before = server_files().get(PDF)
    check("fixture_present", before is not None, "no " + PDF)
    if not before:
        sys.exit(1)

    # --- 1. an editor blocks a second editor -------------------------------
    # Excel/Word/AutoCAD open documents read+write, sharing READ only.
    ha, e = open_(pa, GR | GW, SHARE_R)
    check("A_opens_for_edit", ha is not None, e)
    hb, e = open_(pb, GR | GW, SHARE_R)
    check("B_edit_refused_SHARING_VIOLATION", hb is None and e == SHARING_VIOLATION, e)
    close(hb)

    # --- 2. ...but may still open it read-only (the "Read-Only" button) -----
    hb, e = open_(pb, GR, SHARE_R | SHARE_W)
    check("B_readonly_open_allowed", hb is not None, e)
    if hb:
        check("B_readonly_reads_content", read(hb, 4) == b"%PDF")
    close(hb)

    # --- 3/4. nobody deletes or renames a file someone is editing ----------
    ok = k32.DeleteFileW(pb)
    e = ctypes.get_last_error()
    check("B_delete_refused", not ok and e == SHARING_VIOLATION, e)
    ok = k32.MoveFileExW(pb, pb + ".moved", 0)
    e = ctypes.get_last_error()
    check("B_rename_refused", not ok and e == SHARING_VIOLATION, e)
    now = server_files().get(PDF)
    check("row_untouched_server_side", now is not None and now["guid"] == before["guid"]
          and now["location"] == before["location"], now)

    # --- 5. closing frees it for the other machine, immediately -----------
    close(ha)
    hb, took = until(lambda: open_(pb, GR | GW, SHARE_R)[0], timeout=5.0)
    check("released_when_A_closes", hb is not None, "still refused after 5s")
    print(f"     (B got it {took:.2f}s after A closed)")
    ha, e = open_(pa, GR | GW, SHARE_R)
    check("now_A_is_refused_while_B_edits", ha is None and e == SHARING_VIOLATION, e)
    close(ha, hb)

    # --- 6. writers that BOTH share write coexist (the Windows rules) -----
    ha, e1 = open_(pa, GR | GW, SHARE_R | SHARE_W)
    hb, e2 = open_(pb, GR | GW, SHARE_R | SHARE_W)
    check("mutually_sharing_writers_coexist", ha is not None and hb is not None, (e1, e2))
    close(ha, hb)

    # --- 7. a deny-write READER does not block a remote editor ------------
    # Deliberate deviation from SMB: every reader holds an immutable snapshot,
    # so the editor's save cannot disturb it (docs/protocol.md 4.5).
    ha, e1 = open_(pa, GR, SHARE_R)
    hb, e2 = open_(pb, GR | GW, SHARE_R)
    check("deny_write_reader_does_not_block_remote_editor", ha is not None and hb is not None,
          (e1, e2))
    close(ha, hb)

    # --- 8. the owner file tells B who has it -------------------------------
    # B's listing of the record is cached BEFORE the owner file exists, so
    # seeing it right after the refusal proves the refusal refreshed it.
    os.listdir(db)
    owner_leaf = "~$" + PDF
    oa, ob = os.path.join(da, owner_leaf), os.path.join(db, owner_leaf)
    owner = b"\x06claude" + b" " * 47 + b"\x06\x00c\x00l\x00a\x00u\x00d\x00e\x00"
    ha, e1 = open_(pa, GR | GW, SHARE_R)
    ho, e2 = open_(oa, GR | GW, SHARE_R, CREATE_NEW)
    check("A_holds_doc_and_owner_file", ha is not None and ho is not None, (e1, e2))
    check("A_writes_owner_file", ho is not None and write(ho, owner))
    hb, e = open_(pb, GR | GW, SHARE_R)
    check("B_refused_the_document", hb is None and e == SHARING_VIOLATION, e)
    close(hb)
    try:
        with open(ob, "rb") as f:     # no waiting: the refusal refreshed B
            got = f.read()
    except OSError as ex:
        got = ex
    check("B_reads_owner_file_immediately", got == owner, got)
    ok = k32.DeleteFileW(ob)
    e = ctypes.get_last_error()
    check("B_cannot_delete_a_held_owner_file", not ok and e == SHARING_VIOLATION, e)
    hb, e = open_(ob, GR | GW, SHARE_R, CREATE_ALWAYS)
    check("B_cannot_overwrite_a_held_owner_file", hb is None and e == SHARING_VIOLATION, e)
    close(hb, ho, ha)
    try:
        os.remove(oa)                  # the editor removes its own owner file on close
    except OSError:
        pass

    # --- 9. rename onto a name another machine holds is refused ------------
    src, dst = "lease-src.tmp", "lease-dst.tmp"
    with open(os.path.join(da, src), "wb") as f:
        f.write(b"A's temp")
    hb, e = open_(os.path.join(db, dst), GR | GW, SHARE_R, CREATE_NEW)
    check("B_holds_target_name", hb is not None, e)
    if hb:
        write(hb, b"B's file")
    ok = k32.MoveFileExW(os.path.join(da, src), os.path.join(da, dst), 1)  # REPLACE_EXISTING
    e = ctypes.get_last_error()
    check("rename_over_a_held_target_refused", not ok and e == ACCESS_DENIED, e)
    close(hb)
    try:
        with open(os.path.join(da, src), "rb") as f:
            check("source_intact_after_refused_rename", f.read() == b"A's temp")
    except OSError as ex:
        check("source_intact_after_refused_rename", False, ex)

    # --- 10. the lease FOLLOWS a rename made through an open handle --------
    ha, e = open_(os.path.join(da, src), GR | GW | DEL, SHARE_R)
    check("A_opens_temp_for_edit_and_delete", ha is not None, e)
    ok, e = rename_open_handle(ha, os.path.join(da, "lease-moved.tmp")) if ha else (False, -1)
    check("A_renames_through_its_open_handle", ok, e)
    hb, _ = until(lambda: (lambda r: r if r[1] == SHARING_VIOLATION or r[0] else None)(
        open_(os.path.join(db, "lease-moved.tmp"), GR | GW, SHARE_R)), timeout=8.0)
    check("new_name_is_held_by_A", hb is not None and hb[0] is None and hb[1] == SHARING_VIOLATION,
          hb)
    if hb and hb[0]:
        close(hb[0])
    close(ha)
    for leaf in (src, dst, "lease-moved.tmp"):
        try:
            os.remove(os.path.join(da, leaf))
        except OSError:
            pass

    # --- 11. (optional) a crashed holder frees its files at once -----------
    if args.crash:
        ha, e = open_(pa, GR | GW, SHARE_R)
        check("A_holds_before_crash", ha is not None, e)
        hb, e = open_(pb, GR | GW, SHARE_R)
        check("B_refused_before_crash", hb is None and e == SHARING_VIOLATION, e)
        subprocess.run(["taskkill", "/F", "/PID", str(args.crash)], capture_output=True)
        hb, took = until(lambda: open_(pb, GR | GW, SHARE_R)[0], timeout=10.0)
        check("crashed_holder_released", hb is not None, "still refused after 10s")
        print(f"     (B got it {took:.2f}s after A's process died)")
        close(hb)

    print()
    print(f"{len(passed)} passed, {len(failed)} failed")
    sys.exit(1 if failed else 0)


main()
