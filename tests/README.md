# Tests

End-to-end tests that drive a **live mount** against a running server. They
are not unit tests: they need a mounted drive and a reachable server, so they
live outside the build.

## rename_grace_e2e.py

Covers the former-names grace map: a record renamed while an application
holds a path must stay reachable at its **old** path, resolved by record key.

Setup:

- a server (with its file service) reachable at `ws://127.0.0.1:17243/`,
  its Lua console on `17244`, sharing the database the mount reads;
- a RecordFS agent mounting that server on `S:`;
- record `50049` in `workorders` with at least one PDF attachment.

Run: `python tests/rename_grace_e2e.py`

It renames the record through the server's Lua console, waits out the 30 s
namespace TTL, and asserts the pre-rename path still resolves — then restores
the original name on every exit path.

**The assertion that matters is `OLD_PATH_ACCEPTS_CREATE`.** Windows answers
`isdir`/read on a recently-touched path out of its own metadata cache without
ever consulting the filesystem, so those checks can pass even with the fix
removed; they are reported as informational only. Creating a *new* file
forces a real `Create` through the resolver, and is also exactly what the
Office/CAD save dance does. `CONTROL_create_under_new_name` guards against a
false pass when the ephemeral write plane is unavailable.

Verified to have teeth: with the former-names lookup compiled out, the real
assertion fails (`FileNotFoundError`) while the control still passes.

## write_back_e2e.py

The durable write-back engine: create, modify (verifying the attachment row is
*re-pointed*, not replaced — the guid must be stable), no-op save detection,
rename, nested paths, directory create/remove, and delete. Every assertion is
checked SERVER-SIDE via `list_files`, not just locally, so the Windows
metadata cache cannot fake a pass.

## app_scenarios_e2e.py

What real applications actually do:

- **the Office/CAD save dance** — a `~$` lock file on the ephemeral plane, new
  content written to a temp name, then renamed into place. That rename is a
  *promotion*: ephemeral bytes become a durable attachment.
- **a multi-MB file**, to exercise the streaming upload path.
- **a concurrent-edit conflict** — a peer re-points the row while our handle
  is open. Ours must land beside theirs as a conflict copy, and theirs must
  survive.

Both need the same setup as `rename_grace_e2e.py` and leave the record exactly
as they found it.

## table_write_gate_e2e.py

Table-level write permission enforcement, run as a RESTRICTED user (a mount
token minted for a user who can read a table but not write it). Checks that
list_tables reports can_write=false for the read-only table, add_attachment
on it is refused with a read-only reason, and the same operation on a
writable table succeeds. Needs the restricted user (in the testing DB:
rotest, read-only on quote) and its mount token in
%TEMP%
otest_tok.txt.

## eph_readonly_e2e.py

The lock-file asymmetry on a table the user may read but not write: an
ephemeral lock file is accepted and visible to other sessions, while a
durable attachment write on the same record is refused. Run as a restricted
user (rotest in the testing DB) with its mount token in %TEMP%otest_tok.txt.

## multireader_e2e.py

What a second reader sees when a peer saves the file it has open. Asserts
that an already-open handle keeps reading the version it opened (content is
immutable and hash-addressed, so the handle never follows the new blob), that
the handle metadata likewise does not change, but that a fresh stat BY PATH
picks up the new size once the file-tree TTL rolls and a fresh open reads the
new bytes.

NOTE on change notification: WinFsp raises watcher events for changes made
through THIS machine driver, so a local edit is noticed normally. A PEER
change raises none -- nothing tells this machine driver about it -- so an
application that only listens will not learn of it, while one that re-stats
the path will within the TTL. Surfacing peer changes needs
FspFileSystemNotify, which is not implemented.
