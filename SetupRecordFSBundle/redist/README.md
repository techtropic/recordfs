# redist — third-party installers the bundle chains

Binary installers are NOT committed to the repo. Before building
`SetupRecordFSBundle`, download the official WinFsp installer from
<https://winfsp.dev/rel/> (or the GitHub releases of
<https://github.com/winfsp/winfsp>) and place it here as:

```
winfsp.msi
```

Use the unmodified, signed MSI exactly as published — RecordFS is required
to (and wants to) ship WinFsp only via its official installer; the kernel
driver inside it is signed by the WinFsp project. Update to a newer WinFsp
by replacing the file; the bundle chains whatever version sits here.
