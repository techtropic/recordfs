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

### Visual Studio / installers

`recordfs.sln` carries the MSBuild path: `recordfs.vcxproj` plus the WiX v3.11
installer projects — `SetupRecordFS` (the RecordFS MSI) and
`SetupRecordFSBundle` (the end-user `RecordFSSetup.exe`, which chains the
official signed WinFsp installer first, then RecordFS). Requirements beyond
the compiler: the [WiX Toolset v3.11](https://wixtoolset.org/releases/v3.11/stable)
build tools (+ VS extension for IDE use), a vcpkg dependency tree at
`vcpkg_installed\x64-windows\x64-windows` next to the solution
(`vcpkg install --triplet x64-windows --x-install-root=vcpkg_installed\x64-windows`),
and the official WinFsp MSI dropped at `SetupRecordFSBundle\redist\winfsp.msi`
(see the README there). The `Release Signed` configuration signs the exe, MSI,
and bundle engine with Azure Trusted Signing (expects `tsmetadata.json` at the
solution root; not committed).

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

RecordFS is licensed under the **MIT License** — see [LICENSE](LICENSE).

It links against [WinFsp](https://winfsp.dev), which is GPLv3 with a FLOSS
exception permitting use by open-source projects under OSI-approved licenses
(MIT qualifies). The installer chains WinFsp's own unmodified, signed
installer; RecordFS ships no WinFsp code. If you fork RecordFS into a
non-FLOSS product, that exception no longer covers you — you would need your
own WinFsp arrangement.

Contributions are accepted under the same MIT license (inbound = outbound) —
see [CONTRIBUTING.md](CONTRIBUTING.md).

The protocol specification in `docs/` is licensed CC-BY-4.0 so that anyone
may implement either side of it.
