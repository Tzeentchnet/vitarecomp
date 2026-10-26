<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# Prior AOT correctness migration

This document records reusable correctness knowledge adapted from the verified
2026-10-25 `killzone-aot` snapshot. The source branch was based on Vita3K
`ee50eb8644b4184996047f04406486281586935b`, ended at
`c505989f8c03dcae922fe93f09b0d30dac21c230`, and had ten additional
uncommitted public source/documentation paths. The snapshot validated their
exact current bytes before migration.

No title input, generated title source, trace, private path, or private-input
hash is present here. New files are GPL-2.0-or-later because the source tests
and Vita3K-derived work are GPL-2.0-or-later.

## Ported and executing

`test_prior_aot` adapts independent cases to the canonical C API:

- every CPU/VFP state item represented by the current runtime is captured and
  restored as raw values;
- NaN payload bits survive the VFP register surface;
- VFP unordered comparison flags and VMRS transfer are checked;
- checked, little-endian, unaligned guest memory rejects below-base,
  above-range, and straddling accesses; and
- arithmetic carry/borrow and shift-count boundary vectors execute through
  `recomp_rt.h`.

`prior-aot-dispatch-diagnostic` executes the canonical dispatcher in trace mode
and verifies that one missing indirect target reports both the source and the
raw destination (including its Thumb marker). Default non-trace behavior
remains the canonical aborting trap.

Machine-readable source and expected-state cases are in
`tests/fixtures/prior-aot/migration-vectors.json`.

`compose-generated-image-set` promotes the retained multi-image vectors through
the public `armrecomp compose` surface. It proves byte-identical output under
input reordering, address-sorted module/dispatch registries, distinct ARM and
Thumb keys at one aligned address, and explicit rejection of overlaps,
same-mode duplicates, malformed reports, and unresolved entry records. It also
proves that body cuts, unreported partitions, unclassified traps, and missing
PC instrumentation make aggregate entry ineligible, while classified named
traps remain eligible only as a strict first stop.

## Promoted integration points

The formerly deferred canonical seams now execute in public tests:

| Contract | Canonical integration | Public evidence |
|---|---|---|
| ARM/Thumb block-key collision at one aligned address | Mode-bearing discovery, names, sidecars, composition, and exact-key dispatch | Discovery, emitter, compose, and dispatch tests |
| Conditional A32 exit fallthrough and `BLX LR` aliasing | Canonical analyzer/emitter source capture and generated control flow | Emitter prior-vector regressions |
| Thumb aligned-PC `TBB`/`TBH` | Decoder, discovery, and emission use the architectural aligned PC | Discovery/emitter regressions |
| `VST1` lane rejection | Decoder/emitter validate the supported lane shape | CPU/emitter regressions |
| FPSCR flush-to-zero comparison | Raw FPSCR control/state and comparison helpers | Runtime/prior-AOT vectors |
| Exclusive monitor and ordinary-write invalidation | Shared synchronized monitor used by checked writes and LDREX/STREX | Cross-thread runtime tests |

A full external runtime-state adapter is not planned. Generated-module ABI v4
deliberately exposes only the title-neutral state required by generated C and
entry preparation, including explicit deterministic TLS layout. Its site-keyed
function-import binding behavior is unchanged from ABI v3.

## Required semantics retained by the fixtures

- Sort composed images and reports deterministically; never use input order as
  precedence.
- Reject overlapping executable ranges and duplicate `(address, mode)` keys.
- Preserve the function-pointer low bit as Thumb metadata.
- Report strict misses as `(source, destination address, destination mode)`.
- Compare all represented state and touched memory, not only return registers.
- Make every guest access checked and keep failed reads observable.
- Serialize an ordinary overlapping write and reservation invalidation under
  the same exclusive-monitor lock.
- Reject an unrecognized instruction form instead of emitting a broader form
  that happens to share an opcode family.

The historical C++ recompiler and complete Vita3K tree are intentionally not
part of this repository.
