# The File-Session Protocol

**Version 1.2 — 2026-09-11** (adds table discovery, §4.0, and file leases —
cross-machine "file in use" — §4.5; 1.1 added the ephemeral plane, §4.4).
License: **CC-BY-4.0** — copy, implement, and adapt freely with attribution.
This document specifies everything a file-mounting client (such as RecordFS)
needs to interoperate with a compliant server. The reference server
implementation is Scheduler++ (`scdsrv` + its file-storage daemon `scdfsd`);
this document is the interface — a client built from it shares no code with
the server.

A client speaks two planes:

| Plane | Transport | Carries | Auth |
| --- | --- | --- | --- |
| Namespace | WebSocket, JSON text frames | record listings, display names, file listings, token management | mount token (login), then session |
| Bytes | HTTPS to the file-storage daemon | content-addressed blobs `sha256:<hex>` | per-operation grant token; server-vouched certificate fingerprint pin |

## 1. WebSocket framing

Connect to the server's WebSocket endpoint (e.g. `ws://host:7243/`). All
frames are JSON text. Client→server frames carry a client-chosen numeric
`id`; the matching response carries the same `id`. Requests after login use:

```json
{ "type": "request", "request": "<name>", "id": 7, "data": { ... } }
```

Responses echo `id` and add request-specific fields. Clients MUST correlate
by `id` and MUST ignore unknown fields in any frame (forward compatibility).
File sessions receive **no unsolicited frames** — every server frame answers
a client request. Frame order between distinct requests is not guaranteed.

Failure surfaces, in order of specificity:

- `{"type":"error","id":N,"details":"403 Forbidden: '<request>' not permitted on a file session"}`
  — request outside the file-session allowlist (§3).
- `{"success": false, "error": "<reason>", "id": N}` — request-level failure
  on requests that report `success`.
- `{"error": "<reason>", "id": N}` — lookup failure on namespace requests
  (e.g. `"unknown table"`, `"unknown object"`).

## 2. Login

A *file session* authenticates with a **mount token** — a revocable
credential minted server-side for one (user, machine); the client never
handles a password:

```json
{ "type": "login", "id": 1,
  "data": { "session_type": "file", "client_token": "<token>" } }
```

Success:

```json
{ "type": "success", "id": 1,
  "data": { "session_resumed": false, "session_guid": "…", "file_only": true } }
```

Any other outcome is `{"type":"failure"}` (invalid, expired, revoked, or
wrong-scope token). The server binds the session to the token's user; all
permission filtering below is per that user.

## 3. The file-session allowlist

File sessions may issue **only** these requests (anything else → the 403
error frame):

| Request | Purpose | Phase |
| --- | --- | --- |
| `list_tables` | the mount root: tables holding attachments this user may read (§4.0) | read |
| `list_objects` | enumerate a table's records (id + display) | read |
| `get_object_display` | one record's display string | read |
| `list_files` | one record's attachment listing | read |
| `get_attachments` | legacy/rich attachment listing (apps' shape) | read |
| `get_file_token` | mint a byte-plane grant token | read/write |
| `refresh_client_token` | slide the mount token's expiry | session |
| `get_current_stamp` | server change stamp (cheap liveness/staleness probe) | session |
| `ephemeral_put` / `ephemeral_get` / `ephemeral_delete` | server-memory lock/temp files (§4.4) | read/write |
| `file_lease_acquire` / `file_lease_release` / `file_lease_renew` | cross-machine share modes — "file in use" (§4.5) | session |
| `add_attachment` | create an attachment row | write |
| `rename_attachment` | rename/move within a record | write |
| `link_attachment` | link an existing attachment to a record | write |
| `update_attachment_location` | re-point a row at new content | write |
| `delete_attachment` | soft-delete (server retention applies) | write |

The write family is listed for completeness; its full request/response
shapes will be specified in a v2 revision of this document before RecordFS's
write-back phase. A read-only client needs only the read + session rows.

## 4. Namespace requests

### 4.0 `list_tables` — the mount root

```json
{ "type": "request", "request": "list_tables", "id": 2, "data": {} }
```

Response — every table that holds at least one attachment and that the
session user may read:

```json
{ "id": 2, "tables": [ { "table": "workorders", "key_type": "id", "can_write": true }, … ] }
```

- Discovered, not configured: a table appears once its first attachment
  exists. Servers may cache the set for a few minutes.
- `key_type` (`"id"` or `"guid"`) is how that table's records are addressed
  in §4.2–§4.5.
- `can_write` is the user's table-level write permission. Present `false`
  as a read-only table and refuse writes up front (the server enforces it
  regardless); absent means an older server — assume writable and let the
  server refuse.
- Only on a **file** session. On a full session the same request name is an
  unrelated administrative verb.

### 4.1 `list_objects` — enumerate a table

```json
{ "type": "request", "request": "list_objects", "id": 2,
  "data": { "table": "workorders", "start": 0, "count": 500 } }
```

`start` (default 0) and `count` (default 0 = all) page the listing.
Response:

```json
{ "id": 2, "total": 1234,
  "objects": [ { "id": 50049, "key": "50049", "display": "250089 - Chris Wilson - Lot 22" }, … ] }
```

- `key` is always present (string form of the record key); `id` is present
  additionally when the key is numeric. Use `key` as the identity, `display`
  as the human name.
- The listing is server-filtered by the session user's permissions and is
  **one query** server-side — clients should still cache it (TTL) and must
  not re-enumerate per directory operation.
- `{"error":"unknown table"}` for unknown/unresolvable table names.

### 4.2 `get_object_display` — one record

```json
{ "type": "request", "request": "get_object_display", "id": 3,
  "data": { "table": "workorders", "key_type": "id", "key": 50049 } }
```

`key_type` is `"id"` for numeric-keyed tables (with numeric `key`) or
`"guid"` (with string `key`) for guid-keyed tables. Response:

```json
{ "id": 3, "object_id": 50049, "key": "50049", "display": "250089 - Chris Wilson - Lot 22" }
```

(`object_id` present when numeric.) Unknown record → `{"error":"unknown object"}`.

### 4.3 `list_files` — one record's attachments

```json
{ "type": "request", "request": "list_files", "id": 4,
  "data": { "table": "workorders", "key_type": "id", "key": 50049 } }
```

Response — one entry per attachment the session user may read:

```json
{ "id": 4, "files": [ {
    "guid": "179456c1-82af-11f1-a9e3-a22057d500be",
    "filename": "/photos/install-1.jpg",
    "location": "sha256:c6c23c5b…",
    "mimetype": "image/jpeg",
    "size": 1048576,
    "modified_on": "2026-07-21 14:03:22",
    "can_write": false,
    "ephemeral": false } ] }
```

- `filename` is the attachment's path **within the record's folder**, `/`
  separated. A leading `/` is canonical; clients MUST tolerate its absence
  (legacy rows). Intermediate path components imply folders.
- `mimetype` `"inode/directory"` marks an explicit (possibly empty) folder.
- `location` starting `sha256:` = daemon-backed content (fetch per §6).
  Any other form (a filesystem path, a cloud id) is legacy storage a
  mounting client should treat as unavailable-for-bytes.
- `can_write` reflects the user's write mask on that attachment — surface it
  as the read-only attribute.
- `ephemeral: true` entries are server-memory lock/temp files (§4.4): `guid`
  and `location` are empty, `can_write` is true, and the bytes travel over
  the ephemeral requests, not the byte plane.
- `modified_on` (`YYYY-MM-DD HH:MM:SS`, server-local time) may be absent.

### 4.4 The ephemeral plane — server-memory lock/temp files

Applications sharing files expect the filesystem to carry their lock and
temp files (`~$Book1.xlsx`, `plot.dwl`, `*.tmp`). These must be visible to
every client of the same record — that is what makes cross-machine app
locking work — but they are not documents: servers hold them **in memory**,
with no durable rows and no blob storage. Properties:

- Addressed like `list_files` plus `path` (leading `/`, `/`-separated; `..`
  rejected). Bytes travel inline as base64. Per-file cap: 1 MiB (`413`-style
  `"ephemeral file too large"` error beyond).
- `ephemeral_put {…target…, path, data}` → `{success}` — create or replace;
  the entry's owner becomes the writing session.
- `ephemeral_get {…target…, path}` → `{success, data, size, modified_on}`.
- `ephemeral_delete {…target…, path}` → `{success}` — **any** session may
  delete (lock cleanup between applications is cooperative).
- Lifetime: entries are tied to their owner session; when it disconnects
  they survive a grace window (~120 s) and then vanish — a crashed client's
  locks self-clean. Clients MUST treat a missing entry as normal
  (`success:false`, `"not found"`).
- Mounting clients gate which names ride this plane by pattern (`~$*`,
  `*.tmp`, `*.dwl`/`.dwl2`, `*.laccdb`/`.ldb`, LibreOffice's
  `.~lock.*#`); everything else is durable.

### 4.5 File leases — cross-machine share modes ("file in use")

On a real share, a second user who opens a document someone else is editing
gets a *sharing violation*; Office and CAD turn that into "locked for
editing by <user>" and offer a read-only copy. Each client's operating
system arbitrates only its own opens, so the server arbitrates **between
clients** with leases:

- `file_lease_acquire {…target…, path, open_id, access, share, machine}` →
  `{success, granted, holder?}`.
  - `access` and `share` are bitmasks in the Win32 `FILE_SHARE_*` layout:
    `1` read, `2` write, `4` delete. `access` is what this open may do;
    `share` is what it allows *other* opens to do.
  - `open_id` is the client's name for this open (unique within the
    session, ≤ 64 chars); `machine` is a display name shown to users who are
    refused (≤ 64 chars).
  - Refused when the open collides with one held by **another session**,
    under the Windows rule: each side's access must be permitted by the
    other side's share mode. Opens within one session never collide — the
    client's own OS arbitrates those.
  - `granted: false` carries `holder: {user, machine, since}` — who holds
    the colliding open.
  - Re-acquiring an `open_id` that is already held **moves** it to the new
    path (a rename carries its lease) — atomically, and only if nobody else
    holds the target; a refused move leaves the lease where it was.
  - `path` is addressed as in §4.4 and compares case-insensitively (ASCII).
  - `success: false` with `"unknown object"`, `"permission denied"` (table
    read is required — a lease changes no data; the writes that follow carry
    their own checks), `"bad request"`, or `"too many open files"`.
- `file_lease_release {open_id}` → `{success}` (`false`, `"not held"`, for
  an unknown id).
- `file_lease_renew {}` → `{success, held}` — extends every lease the
  session holds.
- Lifetime: leases die with their session, and expire **90 s** after their
  last renew — renew about every 30 s while holding any. A client that loses
  its network without closing its socket frees its files within that
  window. Leases live in server memory; a restart ends every file session,
  and with it every open handle.

Client conduct:

- Register every open that can change or remove a file (write or delete
  access). Release at the application's close — **after** any save it
  triggers has landed, so the next editor starts from that content. Map
  `granted: false` to a sharing violation.
- Readers need not register. Clients read immutable, content-addressed
  snapshots, so a save elsewhere cannot disturb an open reader — which is
  why a reader that denies write sharing does **not** block a remote editor
  here, deliberately unlike SMB (where it blocks every save).
- After acquiring a write lease, re-read the record's listing before copying
  content up: a cached listing may predate the previous editor's save.
- Owner and lock files (§4.4) are leased like any other file, so a peer
  cannot delete or overwrite a live one. Publish their bytes as they change
  rather than at close: applications read them to tell the refused user who
  holds the document.
- A server that predates leases answers with the §1 403 frame: carry on
  without arbitration (conflict detection at save time remains the backstop).

## 5. Mount-token management

- **Mint** (not available to file sessions — a *full* authenticated session,
  e.g. the vendor's desktop app at login, or an admin tool, mints for its
  own user): `create_client_token {"machine": "<hostname>", "ttl_days": 30}`
  → `{"success":true,"result":{"token":"<plaintext token>","ttl_days":30}}`.
  The plaintext token is returned **once**; the server stores only its hash.
- **Refresh** (file sessions, for their own token):
  `refresh_client_token {"token":"<token>","ttl_days":30}` → `{"success":true}`.
  Sliding expiry: call opportunistically (e.g. on mount + daily).
- **Revoke**: server-side administrative action (delete/revoke the token
  row). The client observes it as login `failure` / request errors — unmount
  to a disconnected state and wait for new credentials; never prompt.

## 6. The byte plane

### 6.1 Minting a grant

```json
{ "type": "request", "request": "get_file_token", "id": 5,
  "data": { "op": "get", "attachment_guid": "179456c1-…" } }
```

Success:

```json
{ "id": 5, "result": {
    "token": "<opaque grant token>",
    "exp": 1784563200,
    "mode": "direct",
    "urls": ["https://192.168.1.50:7245"],
    "fingerprint": "0e50a6fc…(64 hex)…",
    "bridge": "connected",
    "hash": "sha256:c6c23c5b…" } }
```

Failures: `{"success":false,"error":…}` with `"no file service"` (deployment
has no daemon), `"unknown attachment"`, `"permission denied"`,
`"attachment is not daemon-backed"`.

- The grant token is **opaque** to the client and short-lived (~120 s):
  mint per operation, never cache.
- `mode:"proxy_only"` means the daemon's direct plane is not advertised;
  file-session clients cannot use the server's proxy plane in protocol v1,
  so treat the content as temporarily unavailable.
- `op:"put"` (write phases) mints an upload grant; permission checks for the
  row write happen at the subsequent metadata request.

### 6.2 Fetching bytes — direct plane

For each `urls` entry in order, until one succeeds:

1. TLS-connect. **Verification is by fingerprint pin, not CA chain**: accept
   the handshake, compute lowercase-hex SHA-256 of the peer's DER
   certificate, and require exact equality with `fingerprint`. Mismatch =
   hard failure (do not fall through to CA validation; hostname is
   irrelevant).
2. `GET /files/{hash}` with `Authorization: Bearer <grant token>` (`{hash}`
   exactly as granted, e.g. `sha256:c6c2…`). `HEAD` returns headers only.
3. `200` → body is the content; response carries `X-Content-Hash`. The
   client MUST verify the SHA-256 of the received bytes equals the hash and
   discard on mismatch. `401/403` = bad/expired grant (re-mint once), `404`
   = content missing server-side.

Content is immutable and content-addressed: cache indefinitely keyed by
hash; never re-fetch a hash you hold. (`GET /health`, unauthenticated, is a
liveness probe.)

## 7. Client conduct requirements

Mounting clients integrate with interactive systems; the server side is
shared infrastructure:

- **Never enumerate unprompted.** Directory listings are driven by actual
  filesystem reads, TTL-cached; no background crawls of all records.
- **One namespace request in flight per directory** (coalesce concurrent
  readers client-side).
- Mark mounted volumes so content indexers skip them, where the platform
  allows.
- On auth failure: go quiet (disconnected state + bounded retry). Do not
  hot-loop login attempts with a dead token.

## 8. Credential handoff (Windows)

How a desktop app hands the mount token to the mounting client on the same
machine — both sides implement this format (reference header:
`shared/record_mount_credentials.h`, MIT):

- Registry key `HKCU\Software\RecordFS\mounts\<profile>` (profile default:
  `default`), value `blob` (`REG_BINARY`).
- `blob` = DPAPI `CryptProtectData` (user scope, entropy = UTF-8
  `RecordFS.credential.v1`) of UTF-8 JSON:

```json
{ "v": 1, "server": "ws://host:7243/", "token": "<mount token>",
  "drive": "S:", "user": "alice", "created": "2026-07-21T14:03:22Z" }
```

- Per-user by construction (HKCU + user-scoped DPAPI): another local user
  cannot decrypt it. Writers replace the value atomically; readers treat
  undecryptable/malformed blobs as absent-credentials (not errors).
- `user` is informational; the token itself determines identity.

## 9. Versioning

There is no protocol version handshake in v1. Compatibility rules: servers
add fields and new allowlisted requests without breaking existing ones;
clients ignore unknown fields and treat 403-on-new-request as "server too
old". Breaking changes will introduce an explicit version negotiation.
