# RecordFS

**Mount your business records as a native Windows drive.**

RecordFS presents the file attachments of a record-keeping server as a real
drive letter — one folder per record, named so people can find things:

```
S:\workorders\250089 - Chris Wilson - Lot 22\plan.dwg
S:\workorders\250089 - Chris Wilson - Lot 22\photos\install-1.jpg
```

Applications see an ordinary disk. CAD packages load and save drawings (with
xrefs) directly from record folders; nothing is copied by hand, nothing gets
lost on a desktop, and everything for a job lives with the job.

## Status

Pre-1.0, in field testing. The drive browses, opens, and saves in place
(write-back through content-addressed uploads), honours the server's table
and per-file permissions, and behaves like a shared drive when several
people work on the same records (below). The wire contract is
[docs/protocol.md](docs/protocol.md).

## Several people, one file

RecordFS aims to behave like a file share:

- **File in use.** While someone has a document open for editing, a second
  person who opens it on another machine gets what a share would give them:
  Office and AutoCAD report it locked for editing by that person and offer a
  read-only copy (Office's *Notify* works — write access arrives when the
  file is closed). Nobody can delete or rename a file someone else is
  editing. The server arbitrates this with share-mode leases (protocol §4.5)
  that end the moment the editor closes the file, or within 90 s if their
  machine drops off the network.
- **Lock files are shared.** Office `~$` owner files, AutoCAD `.dwl` and
  LibreOffice `.~lock` files are visible on every machine, so applications'
  own "who has this open" machinery works.
- **Changes show up.** A save made elsewhere raises a change notification in
  folders you have open within about 5 s, so applications that watch for it
  offer to reload; a fresh open always reads the latest version.
- **Open files stay stable.** A file you already have open keeps the version
  you opened, as it would on a share when someone else replaces it.
- **Conflict copies as the last resort.** If two saves still collide (say,
  against a server too old to arbitrate), the later one is kept beside the
  other as `name (conflict <date time>).ext` — nothing is overwritten.

Not supported: databases that several people write at once (Access,
QuickBooks company files). They rely on byte-range locking inside one shared
file, which a whole-file model cannot provide.

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

### Runtime prerequisites (target machines)

- **WinFsp** — installed by the setup bundle.
- **Microsoft Visual C++ 2015–2022 x64 Redistributable** — `recordfs.exe` and
  the bundled OpenSSL DLLs import `MSVCP140`/`VCRUNTIME140`. The setup bundle
  chains Microsoft's own `vc_redist.x64.exe` (never app-local copies, so
  Windows Update keeps servicing the runtime). Installing the bare MSI skips
  it: on a machine without the runtime the agent cannot start, and a
  background process that fails to resolve imports does so silently — run
  `recordfs probe` from a console, which reports the loader error.

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

RecordFS uses **WinFsp - Windows File System Proxy, Copyright (C) Bill
Zissimopoulos** — <https://github.com/winfsp/winfsp>.

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
