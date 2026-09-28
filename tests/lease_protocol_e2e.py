"""File-lease protocol semantics (protocol 4.5), at the WebSocket level.

No mount involved: two FILE SESSIONS on one server play two machines and
exercise the server's share-mode arbitration directly -- the Windows sharing
matrix, moves, renew, release, case folding, same-session exemption, and
release when a session dies. Also checks that list_tables keeps its two
meanings apart: the mount root on a file session, the admin introspection
verb (plain table names) on a full session.

Needs: a lease-capable server at ws://127.0.0.1:17243/ (no file service
required) and the testing login. Mints its own short-lived mount token.
"""
import os, asyncio, json, sys

import websockets

SRV = os.environ.get("RECORDFS_TEST_SERVER", "ws://127.0.0.1:17243/")
TARGET = {"table": "workorders", "key_type": "id", "key": 50049}
R, W, D = 1, 2, 4

passed, failed = [], []


def check(name, cond, detail=""):
    (passed if cond else failed).append(name)
    print(("PASS " if cond else "FAIL ") + name + (("  -- " + str(detail)[:200]) if not cond else ""))


class Session:
    def __init__(self, ws):
        self.ws, self.n = ws, 100

    @classmethod
    async def login(cls, data):
        ws = await websockets.connect(SRV, max_size=None)
        await ws.send(json.dumps({"type": "login", "id": 1, "data": data}))
        while True:
            m = json.loads(await asyncio.wait_for(ws.recv(), 25))
            if m.get("type") in ("success", "failure"):
                if m["type"] != "success":
                    raise SystemExit("login failed: " + json.dumps(data)[:60])
                return cls(ws)

    async def req(self, name, data=None):
        self.n += 1
        await self.ws.send(json.dumps({"type": "request", "request": name, "id": self.n,
                                       "data": data or {}}))
        while True:
            m = json.loads(await asyncio.wait_for(self.ws.recv(), 25))
            if m.get("id") == self.n and m.get("type") != "progress":
                return m

    async def acquire(self, path, open_id, access, share, machine):
        return await self.req("file_lease_acquire", dict(TARGET, path=path, open_id=open_id,
                                                         access=access, share=share,
                                                         machine=machine))


async def main():
    full = await Session.login({"username": "claude", "password_plain": "claude'spassword"})
    t = await full.req("create_client_token", {"machine": "lease-protocol-e2e", "ttl_days": 1})
    token = (t.get("result") or {}).get("token")
    if not token:
        raise SystemExit("could not mint a mount token")

    # --- list_tables: two meanings, kept apart by session type ------------
    lt = await full.req("list_tables")
    names = lt.get("tables", [])
    check("FULL_session_list_tables_is_admin_introspection",
          names and all(isinstance(x, str) for x in names) and "applied_changes" in names,
          str(names[:3]))

    a = await Session.login({"session_type": "file", "client_token": token})
    b = await Session.login({"session_type": "file", "client_token": token})
    lt = await a.req("list_tables")
    check("FILE_session_list_tables_is_mount_root",
          lt.get("tables") and all(isinstance(x, dict) and "table" in x for x in lt["tables"]),
          str(lt.get("tables", [])[:2]))

    # --- the sharing matrix -------------------------------------------------
    r = await a.acquire("/lease-proto.txt", "1", R | W, R, "machine-A")
    check("A_editor_granted", r.get("success") and r.get("granted"), r)
    r = await b.acquire("/lease-proto.txt", "1", R | W, R, "machine-B")
    check("B_editor_refused", r.get("success") and r.get("granted") is False, r)
    h = r.get("holder") or {}
    check("refusal_names_the_holder", h.get("user") == "claude" and h.get("machine") == "machine-A"
          and h.get("since"), h)
    r = await b.acquire("/lease-proto.txt", "2", R, R | W, "machine-B")
    check("B_reader_sharing_write_compatible", r.get("granted") is True, r)
    await b.req("file_lease_release", {"open_id": "2"})
    r = await b.acquire("/lease-proto.txt", "3", D, R | W | D, "machine-B")
    check("B_delete_refused_by_deny_delete", r.get("granted") is False, r)
    r = await b.acquire("/lease-proto.txt", "4", R | W, R | W, "machine-B")
    check("B_writer_refused_when_A_denies_write", r.get("granted") is False, r)

    # --- a move carries the lease ------------------------------------------
    r = await a.acquire("/lease-proto-2.txt", "1", R | W, R, "machine-A")
    check("A_moves_its_lease", r.get("granted") is True, r)
    r = await b.acquire("/lease-proto.txt", "5", R | W, R, "machine-B")
    check("old_name_free_after_move", r.get("granted") is True, r)
    r2 = await b.acquire("/lease-proto-2.txt", "6", R | W, R, "machine-B")
    check("new_name_held_after_move", r2.get("granted") is False, r2)
    r = await a.acquire("/lease-proto.txt", "1", R | W, R, "machine-A")
    check("move_onto_a_held_name_refused", r.get("granted") is False, r)
    r2 = await b.acquire("/lease-proto-2.txt", "6", R | W, R, "machine-B")
    check("refused_move_left_lease_in_place", r2.get("granted") is False, r2)
    await b.req("file_lease_release", {"open_id": "5"})

    # --- renew / release -----------------------------------------------------
    r = await a.req("file_lease_renew")
    check("renew_reports_held", r.get("success") and r.get("held") == 1, r)
    r = await a.req("file_lease_release", {"open_id": "1"})
    check("release_ok", r.get("success") is True, r)
    r = await b.acquire("/lease-proto-2.txt", "6", R | W, R, "machine-B")
    check("released_name_now_free", r.get("granted") is True, r)
    await b.req("file_lease_release", {"open_id": "6"})
    r = await a.req("file_lease_release", {"open_id": "1"})
    check("double_release_reports_not_held", r.get("success") is False, r)

    # --- names compare case-insensitively ----------------------------------
    r = await a.acquire("/LEASE-PROTO-3.TXT", "7", R | W, R, "machine-A")
    r2 = await b.acquire("/lease-proto-3.txt", "8", R | W, R, "machine-B")
    check("case_insensitive_paths", r.get("granted") is True and r2.get("granted") is False, (r, r2))

    # --- one session never collides with itself (its kernel arbitrates) ----
    r = await b.acquire("/lease-proto-4.txt", "9", R | W, 0, "machine-B")
    r2 = await b.acquire("/lease-proto-4.txt", "10", R | W, 0, "machine-B")
    check("same_session_exempt", r.get("granted") is True and r2.get("granted") is True, (r, r2))
    await b.req("file_lease_release", {"open_id": "9"})
    await b.req("file_lease_release", {"open_id": "10"})

    # --- validation -----------------------------------------------------------
    r = await a.req("file_lease_acquire", dict(TARGET, path="/x.txt", access=R | W, share=R))
    check("missing_open_id_rejected", r.get("success") is False, r)
    r = await a.req("file_lease_acquire", {"table": "workorders", "key_type": "id",
                                           "key": 999999999, "path": "/x.txt", "open_id": "z",
                                           "access": 3, "share": 1})
    check("unknown_record_rejected", r.get("success") is False, r)

    # --- a dead session's leases die with it --------------------------------
    await a.ws.close()
    await asyncio.sleep(1.0)
    r = await b.acquire("/lease-proto-3.txt", "11", R | W, R, "machine-B")
    check("closed_session_released", r.get("granted") is True, r)
    await b.req("file_lease_release", {"open_id": "11"})
    await b.ws.close()
    await full.ws.close()


asyncio.run(main())
print()
print(f"{len(passed)} passed, {len(failed)} failed")
sys.exit(1 if failed else 0)
