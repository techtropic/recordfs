# RecordFS

**Mount your business records as a native Windows drive.**

RecordFS presents the file attachments of a record-keeping server as a real
drive letter — one folder per record, named so people can find things:

```
S:\workorders\50049 - 250089 - Chris Wilson - Lot 22\plan.dwg
S:\workorders\50049 - 250089 - Chris Wilson - Lot 22\photos\install-1.jpg
```

Applications see an ordinary disk. CAD packages load and save drawings (with
xrefs) directly from record folders; nothing is copied by hand, nothing gets
lost on a desktop, and everything for a job lives with the job.

## Status

Early development. Current milestone: read-only mount (browse + open).
Write-back (save-in-place from applications) is designed and follows.

## How it works

RecordFS is a standalone protocol client. It contains no code from any server
product and talks to the server exclusively over two documented interfaces
(see [docs/protocol.md](docs/protocol.md)):

- **Namespace** — a WebSocket *file session*: record listings, display names,
  and per-record file listings, authenticated by a revocable mount token
  (never a password).
- **Bytes** — content-addressed blobs (`sha256:<hex>`) fetched over HTTPS
  from the server's file-storage daemon, authorized per operation by
  short-lived grant tokens and verified against a server-vouched TLS
  certificate fingerprint. Downloaded content is hash-verified and cached
  locally; identical content is never fetched twice.

The filesystem itself is provided by [WinFsp](https://winfsp.dev/).

The reference server implementation is **Scheduler++** (a proprietary
scheduling/manufacturing system). RecordFS interoperates with it purely
through the protocol documents above; any server implementing the same
protocol works.

## Building

Requirements:

- Visual Studio 2022+ (MSVC, C++20), CMake 3.24+, Ninja
- [vcpkg](https://github.com/microsoft/vcpkg) (`VCPKG_ROOT` set) — dependencies
  are declared in [vcpkg.json](vcpkg.json): Boost.Beast/Asio, OpenSSL,
  nlohmann-json
- [WinFsp](https://winfsp.dev/rel/) installed **with the Developer feature**
  (headers + import library). Without it the build still produces the
  protocol client (`recordfs probe`), just not the mount.

```
cmake --preset default
cmake --build build/default
```

## Usage

```
recordfs probe --server ws://host:7243/ --token <mount token>
    Exercise the protocol end to end without mounting: log in, walk the
    namespace, fetch one file, verify its hash. Use this to validate a
    deployment.

recordfs store-token --server ws://host:7243/ --token <mount token> [--drive S:]
    Save credentials for this Windows user (DPAPI-protected, per-user).
    Deployments integrated with the server's desktop client get this written
    automatically at login.

recordfs mount [--profile default]
    Mount the drive using stored credentials. (Requires WinFsp.)
```

Mount tokens are minted by the server for an authenticated user (in
Scheduler++: automatically at desktop login, or by an administrator). RecordFS
never sees or stores a password, and a token can be revoked server-side at any
time.

## License

RecordFS is licensed under the **GNU General Public License v3.0** — see
[LICENSE](LICENSE). It links against WinFsp under the GPLv3.

Contributions require a signed [Contributor License Agreement](CLA.md) — see
[CONTRIBUTING.md](CONTRIBUTING.md). The CLA exists so the project can offer
future versions under additional license terms (for example, alongside a
commercial WinFsp license); versions already released under the GPL remain
GPL forever.

The protocol specification in `docs/` is licensed CC-BY-4.0 so that anyone
may implement either side of it.
