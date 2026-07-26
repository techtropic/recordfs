# Contributing to RecordFS

Thanks for your interest. A few ground rules keep the project healthy and its
licensing options open.

## Contributor License Agreement (required)

**Every contribution requires a signed [CLA](CLA.md)** (copyright assignment
to the project maintainer) before it can be merged — no exceptions, including
one-line fixes. This is what allows the project to offer future versions
under additional license terms; everything released under the GPL stays GPL.

Sign once per contributor: open a PR adding your name, email, and date to
`CLA-SIGNATORIES.md` with the statement in [CLA.md](CLA.md), or include the
same statement in your first PR description.

## Code rules

- C++20, MSVC-clean at `/W4`. Match the style of the file you're editing.
- **No copied code.** Do not paste code from other projects, including
  permissively licensed ones, without raising it in the PR first — provenance
  must stay clean for the licensing model to work.
- The protocol boundary is sacred: RecordFS speaks to servers only through
  the interfaces in [docs/protocol.md](docs/protocol.md). Changes that need
  new server behavior start as a protocol-spec discussion, not code.
- New dependencies need a GPLv3-compatible license and a good reason.

## Development

`recordfs probe` exercises the full protocol stack against a live server
without mounting anything — use it as the fast feedback loop. The WinFsp
mount path builds only when the WinFsp Developer feature is installed
(`RECORDFS_HAVE_WINFSP`).
