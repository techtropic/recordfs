# Contributing to RecordFS

Thanks for your interest. A few ground rules keep the project healthy.

## Licensing of contributions

RecordFS is MIT-licensed. By submitting a contribution you agree it is
provided under the same MIT license (inbound = outbound). No CLA, no
paperwork — the MIT grant already covers everything the project needs,
including offering future versions under different terms.

## Code rules

- C++20, MSVC-clean at `/W4`. Match the style of the file you're editing.
- **No copied code.** Do not paste code from other projects, including
  permissively licensed ones, without raising it in the PR first — clean
  provenance keeps the licensing simple.
- The protocol boundary is sacred: RecordFS speaks to servers only through
  the interfaces in [docs/protocol.md](docs/protocol.md). Changes that need
  new server behavior start as a protocol-spec discussion, not code.
- New dependencies need an MIT-compatible license and a good reason.

## Development

`recordfs probe` exercises the full protocol stack against a live server
without mounting anything — use it as the fast feedback loop. The WinFsp
mount path builds only when the WinFsp Developer feature is installed
(`RECORDFS_HAVE_WINFSP`).
