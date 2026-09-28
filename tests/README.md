# Tests

End-to-end tests that drive a **live mount** against a running server. They
are not unit tests: they need a mounted drive and a reachable server, so they
live outside the build.

## Setup

Point the tests at a server with `RECORDFS_TEST_SERVER` (default
`ws://127.0.0.1:17243/`). Anything that uploads needs a server **with its
file service**; use the deployment's own server for that (for Scheduler++
testing: `wss://testing.scheduler.techtropic.ca:7243/`), never a second
server with a separate blob store on the same database -- every row it
writes dangles for the other deployment. A local server is fine for the
tests that upload nothing (`lease_protocol_e2e.py`, `file_in_use_e2e.py`)
if it has no file service of its own.

Mount a profile per drive:

    python tests/mint_profile.py <server-url> caseA S:
    recordfs agent-run --profile caseA

`mint_profile.py` logs in with the testing login, mints a one-day mount
token, and stores it with `recordfs store-token` without printing it. Give
each mount its own `LOCALAPPDATA` when a test needs two machines (separate
log, cache, and working area). Remove with `recordfs erase-token --profile`.

The tests below that name a local console (`17244`) need a server you run
yourself; the rest take `RECORDFS_TEST_SERVER`.

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
`%TEMP%\rotest_tok.txt`.

## eph_readonly_e2e.py

The lock-file asymmetry on a table the user may read but not write: an
ephemeral lock file is accepted and visible to other sessions, while a
durable attachment write on the same record is refused. Run as a restricted
user (rotest in the testing DB) with its mount token in `%TEMP%\rotest_tok.txt`.

## multireader_e2e.py

What a second reader sees when a peer saves the file it has open. Asserts
that an already-open handle keeps reading the version it opened (content is
immutable and hash-addressed, so the handle never follows the new blob), that
the handle metadata likewise does not change, but that a fresh stat BY PATH
picks up the new size once the file-tree TTL rolls and a fresh open reads the
new bytes.

NOTE on change notification: local edits are notified by the WinFsp driver
itself. Peer edits are surfaced by the notify pump (see
peer_notify_probe.ps1). An open handle still keeps the version it opened --
that is deliberate, and matches what a real share does when a file is
replaced rather than written in place.

## peer_notify_probe.ps1 (+ peer_change.py)

A peer replaces a file over the wire, never touching this machine filesystem,
and the probe asserts a directory watcher sees a change event and a reopen
reads the new bytes. Run it with the mount up.

## dedup_transparency_e2e.py

Two attachments with byte-identical content share one blob. Editing one must
produce a new hash and re-point only that row, leaving the other file exactly
as it was -- the property that lets deduplication stay invisible to users.

## lease_protocol_e2e.py

The file-lease protocol (docs/protocol.md 4.5) at the WebSocket level, no
mount involved: two file sessions play two machines and exercise the
server's share-mode arbitration directly -- the Windows sharing matrix, the
holder named in a refusal, moves (a rename carrying its lease, and a move
onto a held name refused), renew, release, case-insensitive names, the
same-session exemption, and release when a session dies. Also asserts that
`list_tables` keeps its two meanings apart (mount root on a file session,
admin introspection on a full session). Mints its own short-lived mount
token; needs no file service.

## file_in_use_e2e.py

"File in use" through real mounts. Two mounts of one server (separate
processes and sessions, so the kernel cannot arbitrate between them -- only
the server can) stand in for two machines, and every handle is opened with
explicit Win32 access/share modes the way Office and CAD open documents:
an editor refuses a second editor (`ERROR_SHARING_VIOLATION`) but not a
read-only open; nobody can delete or rename a file someone is editing;
closing frees it immediately; mutually-sharing writers coexist; a
deny-write reader does not block a remote editor (the deliberate SMB
difference); the holder's owner file is readable by the refused machine at
once, with its bytes, and cannot be deleted or overwritten while held; a
rename onto a name another machine holds is refused; a lease follows a
rename made through an open handle. `--crash <pid>` finally kills mount A
while it holds a file and asserts mount B can edit it at once.

Setup (no file service needed -- nothing uploads, which keeps a dev server
on a shared database from owning a blob store): two stored profiles for the
same server, mounted with `recordfs agent-run --profile leaseA` (S:) and
`--profile leaseB` (T:); record 50049 holding `workorder-signed.pdf` with
content both mounts can hydrate. Running each mount with its own
`LOCALAPPDATA` gives each a separate log, cache and working area, like two
machines.

## excel_in_use_e2e.ps1

The same property with the real application: invisible Excel instances on
mount A and mount B. A opens the workbook read-write; B must get it
READ-ONLY, see who holds it (from Excel's own owner file, over the
ephemeral plane), and get write access once A closes (`ChangeFileAccess`,
the "Notify" path). Only Excel processes the script started are ever
stopped. Measured: B's Excel is refused the owner file (`~$<name>`) first
and falls back to read-only from that -- the lock-file lease is load-bearing,
not just the document's.

## case_and_metadata_e2e.py

Names on the drive are case-insensitive, as on any Windows volume: an
upper-case path reads and stats the stored file, the listing keeps the
stored spelling, `CREATE_NEW` on an existing name in another case collides,
`OPEN_ALWAYS` through another case opens the SAME attachment (no second
row), a file created under `CASEDIR\` lands in the existing `CaseDir`
folder, `mkdir` of an existing folder in another case is refused, and a
case-only rename renames rather than deleting the file. Then opens that
touch no bytes: renaming, stat-ing and deleting a file whose content is not
cached must succeed without downloading it (it borrows the largest jpg on
record `--donor` by adding a second row pointing at the same content, and
evicts that blob from the mount's cache first).

    python tests/case_and_metadata_e2e.py --server <url> --cache <mount's cache dir>

Verified to have teeth against the previous build: the upper-case path is
"not found", both collision checks create empty duplicate rows instead, a
file cannot be created in a differently-cased folder, a case-only rename is
refused, and renaming the 2.8 MB donor downloads all of it.

## update_check_e2e.py (+ update_feed_mock.py)

The updater's decisions, through `recordfs update-check` -- a dry run that
downloads and verifies exactly as the service does and changes nothing --
against `update_feed_mock.py`, a stand-in for the GitHub release API. A newer
release is offered; an unsigned package is refused; so is a package validly
signed by the RecordFS publisher that is not RecordFS (pass any such MSI,
e.g. a Scheduler++ installer, as `--signed-other`); so are a size or SHA-256
mismatch. Older, pre-release, suffixed (`-rc1`) and asset-less releases are
not updates; a 404 feed explains itself; plain HTTP is refused off loopback.

    python tests/update_check_e2e.py <dir with RecordFS-<ver>.msi> --exe <older recordfs.exe>
                                     [--signed-other <signed non-RecordFS .msi>]

Build test packages with `-p:RecordFSVersion=0.2.0` and `0.2.1` on the
`SetupRecordFS` target; copy the OpenSSL DLLs next to the `--exe`.

## update_sandbox_e2e.py (+ update_sandbox/)

Install, automatic update and uninstall, end to end, in Windows Sandbox -- a
disposable VM, so nothing is installed on the machine running it. The
sandbox installs the prerequisites and the OLD package with site settings
(drive R:, a custom label), mounts it against `--server`, holds a file open
on the drive, and has the updater service install the NEW package from a
local mock feed (`update_sandbox/mock.ps1`). Asserted: the update waited for
the open file and started only after it closed; the new version is
installed; the drive came back; drive letter and label were kept; the
staging folder is admin-writable only; uninstall removes the service,
staging folder, updater state and autostart.

    python tests/update_sandbox_e2e.py --pkgs <dir> --from 0.2.0 --to 0.2.1

Needs Windows Sandbox enabled (the window shows while it runs, 5-10
minutes, and closes itself) and `SetupRecordFSBundle\redist` populated.

Windows Sandbox does not load WinFsp's kernel driver (the agent logs
`STATUS_NO_SUCH_DEVICE`, 0xc000000e), so there the drive never mounts and the
open-file deferral and remount checks are reported as skipped. Everything
about the update itself is still exercised: verification, the service
stopping and restarting around msiexec, agents stepping aside and coming
back, kept settings, the staging ACL, and uninstall. The live-drive checks
need a real or Hyper-V machine. The first sandbox start on a machine can take
several minutes.
