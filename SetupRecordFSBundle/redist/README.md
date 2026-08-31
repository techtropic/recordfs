# redist — third-party installers the bundle chains

Binary installers are NOT committed to the repo. Before building
`SetupRecordFSBundle`, download the official installers and place them here
under these exact names:

| File | Where from |
| --- | --- |
| `winfsp.msi` | <https://winfsp.dev/rel/> (or the [winfsp releases](https://github.com/winfsp/winfsp/releases)) |
| `vc_redist.x64.exe` | <https://aka.ms/vs/17/release/vc_redist.x64.exe> (Microsoft VC++ 2015–2022 x64 redistributable) |

Use the unmodified, signed installers exactly as published:

- **WinFsp** — RecordFS is required to (and wants to) ship WinFsp only via its
  official installer; the kernel driver inside it is signed by the WinFsp
  project. Update by replacing the file; the bundle chains whatever sits here.
- **VC++ redistributable** — chained rather than deploying the CRT DLLs
  app-local, so the runtime keeps receiving OS-level security updates. The
  bundle skips it when an equal-or-newer runtime is already installed, and
  marks it `Permanent` so uninstalling RecordFS never removes a runtime the
  rest of the machine depends on.
