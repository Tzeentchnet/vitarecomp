# Provenance

This document records the sources incorporated into this fork and the license
terms that travel with them. Update the import ledger before adding an
external commit or manually ported component.

## Foundation baseline

The repository was forked with its history intact from:

- Repository: <https://github.com/sp00nznet/vitarecomp>
- Starting branch: `main`
- Starting commit: `6ed4a5c59045706292c8ec03dfe5207ecc4033a4`
- Starting commit subject: `TRANSLATION: what the running program asks for`
- Starting commit date: 2026-08-20
- Local pin: `upstream-start-6ed4a5c5`

The starting commit is the unchanged upstream baseline used for the initial
Windows CMake and CTest validation. The pin is a tag; no upstream commit was
rebased, amended, or otherwise rewritten.

## License layers

All material inherited through the starting commit remains under the MIT
License. The verbatim upstream license is preserved at
[`LICENSES/MIT.txt`](../LICENSES/MIT.txt).

Unless a file says otherwise, modifications and additions first authored for
this fork after the starting commit are licensed under
GPL-2.0-or-later. The modified combined distribution is provided under
GPL-2.0-or-later as a whole, without withdrawing or replacing the MIT grant
for identifiable upstream material. See [`LICENSE`](../LICENSE),
[`NOTICE`](../NOTICE), and
[`LICENSES/GPL-2.0-or-later.txt`](../LICENSES/GPL-2.0-or-later.txt).

## Source inventory

| Source | Reference | Incorporated status | License |
|---|---|---|---|
| `sp00nznet/vitarecomp` | `6ed4a5c59045706292c8ec03dfe5207ecc4033a4` and its ancestors | Foundation history | MIT |
| `Shiz0id/vitarecomp` | Comparison remote `shiz0id` | Not incorporated | No determination made here |
| `soyjxck/vitarecomp` | Comparison remote `soyjxck` | Not incorporated | No determination made here |
| Vita3K | None | Not incorporated | Not applicable |
| Game data or generated title code | None | Not incorporated | Not applicable |

Fetch-only comparison remotes do not indicate that their contents have been
copied, merged, or relicensed.

## Import ledger

No downstream commits or manually ported external components have been
incorporated at this foundation point.

For each future import, add a row that identifies the exact source commit,
paths or component, integration method, original license, retained notices,
and validation evidence.

| Date | Source and commit | Paths or component | Method | License and notices | Validation |
|---|---|---|---|---|---|
| 2026-10-24 | `sp00nznet/vitarecomp@6ed4a5c59045706292c8ec03dfe5207ecc4033a4` | Complete inherited history | GitHub fork | MIT; preserved in `LICENSES/MIT.txt` and `NOTICE` | Clean Windows baseline: configure/build succeeded; CTest 4/4 passed |

## Excluded material

Do not add retail game data, keys, decrypted executables, generated title
code, private dumps, or build products. External code must not be imported
until its provenance, compatibility, notices, and test coverage are recorded.
