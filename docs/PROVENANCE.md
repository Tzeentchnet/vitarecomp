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
| `soyjxck/vitarecomp` | `5a095d7f429b1e51ffca3af438b18ad1502f07fb` | Selected CPU analysis/emission/runtime topics, tests, and public homebrew fixtures | MIT; upstream notice retained in `LICENSES/MIT.txt` and `NOTICE` |
| Local renderer integration | `b1754cd5464604c70f39253d63b8c6163da20012` | Production Windows DX12 and Vulkan backends, shared conformance shader, tests, and documentation | GPL-2.0-or-later |
| Vita3K | None | Not incorporated | Not applicable |
| Retail game data or generated title code | None | Not incorporated | Not applicable |

Fetch-only comparison remotes do not indicate that their contents have been
copied, merged, or relicensed.

## Import ledger

Each row identifies an exact source, the selected boundary, and the evidence
used to keep unrelated host/title code out.

| Date | Source and commit | Paths or component | Method | License and notices | Validation |
|---|---|---|---|---|---|
| 2026-10-24 | `sp00nznet/vitarecomp@6ed4a5c59045706292c8ec03dfe5207ecc4033a4` | Complete inherited history | GitHub fork | MIT; preserved in `LICENSES/MIT.txt` and `NOTICE` | Clean Windows baseline: configure/build succeeded; CTest 4/4 passed |
| 2026-10-25 | `soyjxck/vitarecomp@5a095d7f429b1e51ffca3af438b18ad1502f07fb` (CPU snapshot `1e922f488d57fe5a6a11ac056120a2c6fb299e4d`) | Analyzer, ARM/Thumb decoder, generated-C emitter, module/NID/relocation readers; exact paths below | Selective file port followed by canonical-ABI adaptations; no merge | MIT; same upstream copyright and license retained | MSVC `/W4 /WX`; synthetic decoder/discovery/emitter/relocation/runtime tests; three public homebrew extract/emit/repeat/compile fixtures; private three-image generation/repeat/object compilation |
| 2026-10-25 | `soyjxck/vitarecomp@5a095d7f429b1e51ffca3af438b18ad1502f07fb` | Core CPU/VFP/NEON/half/memory/dispatch helpers listed below | Selective port; checked memory and mode-bearing dispatch hardened locally | MIT; same upstream copyright and license retained | Canonical runtime tests, prior-vector tests, cross-thread exclusive-monitor test, generated-source compilation |
| 2026-10-25 | `soyjxck/vitarecomp@5a095d7f429b1e51ffca3af438b18ad1502f07fb` | `samples/fbdemo`, `samples/hello`, `samples/threads` source and prebuilt public homebrew modules | Coherent fixture import | MIT; covered by source repository license and this ledger | CTest `homebrew-e2e`: extract, emit twice byte-identically, and compile all generated C |
| 2026-10-25 | Local `integration/renderers@b1754cd5464604c70f39253d63b8c6163da20012` | `src/gpu` DX12/Vulkan implementations, backend registry/runtime changes, pinned-DXC rules, shared HLSL, conformance tests, and renderer documentation | Complete branch merge onto CPU baseline `5c4aa9c7ae1b04c3c832ee399a89c5d853a34df3`; canonical CPU-generated C/runtime ABI retained | GPL-2.0-or-later | Combined MSVC Debug build; CTest 16/16; DX12 WARP and hardware lifecycle/resize conformance; validation-enabled Vulkan lifecycle/resize/recreate conformance; invalid-ICD no-fallback; wrong-DXC rejection; private CPU aggregate and deterministic-generation revalidation |

### Selected CPU file boundary

The analyzer/emitter import uses these files as they existed in the audited
CPU snapshot. The analyzer, decoder, emitter, module, NID, and relocation
implementation was compared against the audited final head before integration:

- `tools/armrecomp/analyze.c`, `analyze.h`
- `tools/armrecomp/decode.c`, `decode.h`
- `tools/armrecomp/emit.c`, `emit.h`
- `tools/armrecomp/module.c`, `module.h`
- `tools/armrecomp/nids.c`, `nids.h`
- `tools/armrecomp/reloc.c`, `reloc.h`
- the CPU-facing portions of `tools/armrecomp/main.c` and
  `tools/armrecomp/CMakeLists.txt`
- `tests/test_decode.c`, `tests/test_discover.c`, and `tests/test_reloc.c`

Compatible emitted-code helpers came from:

- `include/vitarecomp/cpu.h`, `dispatch.h`, `half.h`, `mem.h`, `neon.h`,
  `recomp_rt.h`, and `vfp.h`
- `src/cpu.c`, `src/dispatch.c`, `src/mem.c`, and `src/vfp.c`

The final tree was adapted rather than copied blindly. Local adaptations keep
the canonical generated-C ABI, preserve the renderer-neutral GPU contract,
retain strict aborting traps, add mode-bearing ARM/Thumb keys, checked
exclusive memory, FPSCR FZ semantics, deterministic reports, and
warnings-clean generated C. Final-head host callback dispatch and direct host
memory mapping were not imported because they belong to the deferred host/HLE
foundation rather than this CPU baseline.

### Audited topic origins

The selected final files contain work introduced by these exact downstream
topics:

| Topic | Exact commit | Selected files |
|---|---|---|
| SCE relocation decode/apply and relocation-backed discovery | `bac51008659b5aa55ef1f694aaee8267744cc2a1` | `reloc.c/.h`, `analyze.c/.h`, `emit.c`, `main.c`, `test_reloc.c`, CPU CMake entries |
| Jump tables, emitter fixes, initial NEON/runtime helpers | `51153867c1972b4272bd8cb1daaa69ab7247bc4e` | `analyze.c/.h`, `decode.c/.h`, `emit.c`, CPU/VFP/NEON/memory headers and sources, decoder/runtime tests, public `hello` fixture |
| Retail-census NEON and VFP forms | `3ed6ea9f97f74426f7b51071df449876f90888be` | `decode.c/.h`, `emit.c`, NEON/VFP headers, decoder/runtime tests |
| Reachable-only emission | `dea268cde235f3e56093fa3f72f5a01cf52097ea` | `analyze.c/.h`, `emit.c/.h`, `main.c` |
| Partitioned output and generated shared header | `52ce354ce2f6a35eda0a7ba0b7332175e712cfe0` | `emit.c/.h`, `main.c` |
| Segment-bounded body collection | `deb794d81ee4c88f8bd27211800995b97688face` | `emit.c/.h`, `main.c` |
| Variable imports, decoder guards, jump-table guards | `de4bd36fa71ec0361fbb0d2199f5b9f2ce06e485` | analyzer/decoder/emitter/module/NID files and decoder tests |
| A32 decoding and emission | `cb4308ed6ea22d90170bd67261825c5f76039f67` | analyzer/decoder/emitter files, NEON helper, decoder/runtime tests |
| Discovery and jump-table follow-up | `c6a9beda43584688e4bafc00972fb62316284e2f` | `analyze.c`, `decode.c/.h`, `emit.c`, decoder/runtime tests |

The selected downstream CPU import intentionally does **not** include that
fork's `host/`, renderer, GXM/HLE, audio, movie, filesystem, archive, or
title-specific behavior. The production renderers recorded above were authored
and integrated locally rather than imported from that downstream boundary.

## Combined mainline validation

The renderer checkpoint
`b1754cd5464604c70f39253d63b8c6163da20012` was merged onto canonical CPU
baseline `5c4aa9c7ae1b04c3c832ee399a89c5d853a34df3`. The combined tree built in
Visual Studio 2022 Debug with `/W4 /WX /permissive- /bigobj` and passed all
16 CTest entries. This includes the three public homebrew end-to-end fixtures,
both production-backend registrations, strict unavailable/no-fallback
behavior, DX12 WARP and available-hardware conformance, and
validation-enabled Vulkan conformance.

The private three-module check publishes only aggregate results. It reproduced
105,873 discovered functions, 4,050,600 instructions reached, 10,898,202 of
11,308,620 executable bytes covered, 48,706 emitted functions, 3,663,088
translated instruction instances, and 3,770 trapped instances. Repeated
generation produced the same 809 files and 356,792,679 bytes with identical
relative inventories, reports, and bytes. All three generated-source targets
compiled under the strict MSVC policy with no warning or error diagnostics.
No retail image, identifying path or hash, generated source, or raw private
report is part of the repository.

## Excluded material

Do not add retail game data, keys, decrypted executables, generated title
code, private dumps, or build products. External code must not be imported
until its provenance, compatibility, notices, and test coverage are recorded.
