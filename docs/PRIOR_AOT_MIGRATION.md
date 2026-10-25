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

## Deferred integration points

These cases are precise but cannot honestly execute against a canonical
production seam that does not yet exist:

| Contract | Exact future integration point | Why deferred |
|---|---|---|
| Deterministic multi-image ordering and overlap rejection | Add image-set/report composition beside `tools/armrecomp/main.c`; reuse `vm_image` from `tools/armrecomp/module.h`; add canonical tests under `tests/` | `armrecomp` currently accepts one image and has no report-merger API. |
| ARM/Thumb block-key collision at one aligned address | Replace address-only function marking in `tools/armrecomp/analyze.c`; carry mode through `funcset`, symbol naming, and table generation in `tools/armrecomp/emit.c`; make `include/vitarecomp/dispatch.h` and `src/dispatch.c` search the same key | The pending canonical CPU/mode-selection design must choose the generated-C ABI. Changing only dispatch would leave discovery and symbols aliased. |
| Conditional A32 exit fallthrough and `BLX LR` aliasing | `emit_insn` in `tools/armrecomp/emit.c`, with synthetic decode/emission tests under `tests/` | The C emitter has no generated-code execution harness and its call/PC-state ABI is part of the CPU-selection work. |
| Thumb aligned-PC `TBB`/`TBH` | Decoder/discovery support in `tools/armrecomp/decode.c` and `analyze.c`, followed by emission in `emit.c` | Canonical table-branch recovery is not implemented. |
| `VST1` lane rejection | Future NEON operand validation in `decode.c`/`emit.c` | Canonical NEON emission is not implemented. |
| FPSCR flush-to-zero comparison | Extend the FPSCR representation in `include/vitarecomp/vfp.h` and comparison helpers in `src/vfp.c` | The current runtime stores comparison flags but no FPSCR mode/control word. |
| Exclusive monitor and ordinary-write invalidation | Introduce the shared monitor at the eventual thread/CPU owner, then route `src/mem.c` writes and LDREX/STREX helpers through the same lock | There is no canonical per-thread CPU or exclusive-monitor API yet. |
| Full oracle state including PC/CPSR/FPSCR mode bits and 64 extended words | Optional oracle adapter after canonical CPU selection | The current generated-C ABI deliberately has no PC variable and represents a smaller state surface. |

These deferrals are not claims of test execution. When each seam lands, promote
the corresponding fixture to a canonical unit/differential test before changing
production semantics.

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
