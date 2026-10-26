# vitarecomp

[![Windows quality gates](https://github.com/Tzeentchnet/vitarecomp/actions/workflows/windows-quality.yml/badge.svg)](https://github.com/Tzeentchnet/vitarecomp/actions/workflows/windows-quality.yml)

### *The Vita has one good emulator. It has no static recompiler.*

> Static recompilation toolkit for PlayStation Vita titles.
> Turn ARM into native C. No emulator required, no keys required.

---

## What Is This?

**vitarecomp** is an open-source toolkit that provides the analysis tools,
recompiler, and runtime needed to **statically recompile PlayStation Vita games
into native executables**.

Instead of interpreting or dynamically recompiling ARM instructions at runtime
(what Vita3K does), we take the opposite approach: **translate everything ahead
of time** into C that compiles with any modern compiler on any modern platform.

This is the same philosophy behind:
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) (N64 → native)
- [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) (Xbox 360 → native)
- [ps3recomp](https://github.com/sp00nznet/ps3recomp) (PS3 → native)
- [psprecomp](https://github.com/sp00nznet/psprecomp) (PSP → native)

`vitarecomp` is the reusable toolkit. Games brought up on it live in separate
repos that consume this one as a submodule — that split is deliberate: the
toolkit is the thing other people fork to recompile *their* Vita game.

> **No game data here.** Dumps, `eboot.bin`, `.suprx` modules, VPKs and any key
> material are all `.gitignore`d. This repo is the recompiler, the runtime, and
> docs — bring your own dump.

## Why Vita?

The Vita is a better target than its reputation suggests:

- **One documented CPU.** A quad-core ARM Cortex-A9 running ARMv7-A. ARM is the
  most widely documented architecture there is, and unlike the PSP's Allegrex it
  has no vendor-specific opcode divergences to rediscover.
- **A documented OS boundary.** Games call `sceKernel*` / `sceGxm*` / `sceCtrl*`
  by NID through a module import table. That is a *library* surface, not a
  hardware surface, which means it can be implemented as HLE C rather than
  emulated — and the NID→name mapping is published and MIT-licensed.
- **A corpus that needs no decryption.** Every segment of every QA/prototype
  build measured so far is plaintext, zlib-compressed only. The whole pipeline
  can be built and validated with no key material entering the picture. See
  [docs/CORPUS.md](docs/CORPUS.md).

## The Challenge

| Component | What It Is | Why It's Hard |
|-----------|-----------|---------------|
| **Cortex-A9** | ARMv7-A, Thumb-2 dominant | Two instruction sets interleaved; one bad width desynchronises every instruction after it |
| **NEON** | 128-bit SIMD | The only genuinely hard piece left — but 0.63% of instructions once scalar VFP is handled, not the wall its reputation suggests |
| **SELF container** | SCE-wrapped ELF | Not the PS3 layout; every field after `0x20` is shifted by `0x10` |
| **`.sce_module_info`** | Module metadata | `e_entry` points *at this structure*, not at code — and it mixes segment-relative with absolute addressing |
| **`sceGxm`** | The GPU API | 107 imported functions, one of which is a shader compiler |
| **`ET_SCE_EXEC`** | Statically linked modules | No relocations to mine, so function-pointer recovery is heuristic — and this is the entire launch window |

## Architecture

```
include/vitarecomp/   runtime API — cpu, mem, dispatch, generated ABI,
                      strict loader/binding, entry state
src/                  runtime — CPU state, memory, semantic helpers, traps
tools/armrecomp/      the toolkit:
                        container.c  SELF / ELF / velf parsing
                        inflate.c    DEFLATE + zlib, written not vendored
                        decode.c     ARMv7-A + Thumb-2 decoder
                        module.c     .sce_module_info, import/export tables
                        analyze.c    function discovery
                        nids.c       NID → name against vita-headers
                        emit.c       ARM → C
                        compose.c    deterministic generated image sets
tests/                ctest — synthetic, no game data
docs/                 the detail
```

## How It Works

```
  dump / .vpk ──► armrecomp ──► eboot.bin / *.suprx      peel the container
                  (zip · dir)    (a SCE\0-wrapped ELF)    stack down to a module
                      │
                      ▼
              ┌────────────────────┐   segment table is PLAINTEXT; segments are
              │  inflate           │   zlib. QA builds need no keys at all.
              └────────────────────┘   SELF ──► a plain ELF32 ARM module
                      │
                      ▼
              ┌────────────────────┐   decode ARMv7 + Thumb-2, discover functions
              │  armrecomp         │   (following the module's export table and
              │  (tools/)          │   SCE_RELA relocations), translate to C:
              └────────────────────┘   vita_func_<addr>
                      │  generated modules + composed registry
                      ▼
              ┌────────────────────┐   the runtime the generated C links against:
              │  vitarecomp (lib)  │   CPU state · memory · semantic helpers ·
              │  (src/, include/)  │   sceKernel/sceGxm HLE · dispatch
              └────────────────────┘
                      │
                      ▼   (+ a per-game host: load module, register, run, present)
              native executable — the recompiled game runs
```

Stage-by-stage detail in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Status

**Phases 1–5 and the standalone phase-6 runtime foundation are complete; HLE
expansion remains in progress.** The full pipeline runs end to end — SELF →
ELF → decode → discover → emit → compile → link → run — with no key material
anywhere in it.

| Phase | What | Status |
|---|---|---|
| **1 — Container stack** | SCE/SELF, appinfo, ELF32, program headers, segment table; bounds-checked, named refusal for PFS retail dumps, encryption flag cross-checked against zlib headers | ✅ Complete |
| **2 — Inflate & reassemble** | DEFLATE + zlib written not vendored; **15/15 corpus modules extract and round-trip**, 236 KB → 53 MB | ✅ Complete |
| **3 — Decoder** | ARMv7-A + Thumb-2, width determination, control flow, IT blocks, NEON/VFP classed; `armrecomp cover` over 16 modules | ✅ Complete |
| **4 — Discovery** | `.sce_module_info`, import/export tables, SCE relocations, recursive descent with distinct ARM/Thumb keys, MOVW/MOVT pointers, jump tables, tail targets, and pointer-shape recovery over code and data | ✅ Complete |
| **5 — Emitter** | ARM → readable C, literal folding, run-time dispatch, measured NEON/VFP forms, reachability filtering, partitioned output, and deterministic reports. **Generated C compiles with MSVC `/W4 /WX`** | ✅ Complete |
| **6 — Runtime & HLE** | Checked guest memory, TLS CPU state, scheduler/threading, allocators, libc/libm, import registry, I/O/FIOS2, ULT synchronization, VFS/PSARC, diagnostics, shutdown/restart, and explicit unsupported results are implemented. Guest `sceGxm`/GXP, media, input, UI/savedata, networking/NP, full ULT fibers, true asynchronous I/O, and title-specific compatibility remain deferred | 🔨 Foundation complete; expansion in progress |

The selected CPU baseline preserves the upstream generated-C ABI while
integrating the audited downstream analyzer/emitter topics. On a redacted
three-image retail set it covers **96.371%** of 11,308,620 executable bytes and
translates **99.8972%** of 3,666,858 emitted instruction instances. All three
generated modules compile as MSVC objects; repeat generation is byte-identical.
Unsupported operations remain named aborting traps, never silent output.
Generated-module ABI v1, strict module-set loading/binding, checked
stack/TLS/CPU preparation, and the bounded guest-call outcome API are public
title-neutral contracts; they do not claim that any particular title boots.
Method, trade-offs, and provenance are documented in
[docs/CPU_BASELINE.md](docs/CPU_BASELINE.md).

> **Compiling at this scale:** each function becomes its own COMDAT section and
> a COFF object indexes at most 65,535, so MSVC needs `/bigobj` on the generated
> file. It is a count limit, not a size limit. `armrecomp emit` says so itself.

## Documentation

| Document | What It Covers |
|----------|---------------|
| **[Architecture](docs/ARCHITECTURE.md)** | The pipeline stage by stage: container, inflate, decode, discovery, emit, runtime. Address conventions, indirect dispatch, import binding, and why each runtime helper exists |
| **[CPU baseline](docs/CPU_BASELINE.md)** | Selected analyzer/emitter boundary, redacted retail metrics, deterministic reports, strict traps, and executable regression evidence |
| **[Translation](docs/TRANSLATION.md)** | Coverage figures, `ThumbExpandImm` and its traps, what is still trapping ranked by value, the NEON/VFP breakdown |
| **[Corpus](docs/CORPUS.md)** | The 15 measured modules, the plaintext finding, the `ET_SCE_EXEC` → `ET_SCE_RELEXEC` era split and why it makes the launch window the hard case |
| **[Decryption](docs/DECRYPT.md)** | The SELF container, what is encrypted and what is not, and why the QA corpus needs no keys |
| **[HLE](docs/HLE.md)** | The firmware surface, measured, and the order of work |
| **[Entry state](docs/ENTRY_STATE.md)** | Checked main stack, deterministic module-start CPU state, TLS templates, and the strict guest-call boundary |
| **[GPU contract](docs/GPU.md)** | Renderer-neutral ownership, production DX12/Vulkan behavior, shader contract, and combined validation |
| **[DX12](docs/DX12.md)** | Native Direct3D 12 behavior, validation requirements, WARP, and conformance |
| **[Lessons](docs/LESSONS.md)** | Corrections and bugs found by running rather than reasoning: `MOVT`, the wrong dominant-instruction-set metric, the SIMD undercount, the PS3-layout SELF header |
| **[Roadmap](docs/ROADMAP.md)** | The six phases, checked off against what actually runs |
| **[Prior AOT migration](docs/PRIOR_AOT_MIGRATION.md)** | Canonical test adaptations, retained correctness contracts, machine-readable vectors, and exact deferred integration points |

## Getting Started

> **Prerequisites**: CMake and a C compiler (MSVC on Windows; gcc/clang
> elsewhere). The core has **no external dependencies** — a fresh clone builds
> with nothing installed.

```powershell
git clone https://github.com/sp00nznet/vitarecomp.git
cd vitarecomp

cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
# -> build/tools/armrecomp/Release/armrecomp.exe
# -> the vitarecomp runtime static library
```

```
armrecomp info     <file>                       identify a module, report its structure
armrecomp extract  <self> <out.elf>             SELF in, plain ELF32 out
armrecomp cover    <elf>                        coverage, class histogram, instruction set
armrecomp funcs    <elf> [nid-db]               the HLE work list, from the import table
armrecomp discover <elf>                        function discovery
armrecomp emit     [--module-id <id>] <elf> <out.c> [n] [nid-db]
                                                translate to C
armrecomp compose --entry-module <id> --output <registry.c>
                   <module/report JSON>...       compose generated modules
```

`emit` also writes deterministic `<out>.report.json` and
`<out>.report.txt` files with the strict unsupported/trap census, structural
readiness, entry resolution, instrumentation state, and partition inventory.
The deterministic `<out>.module.json` sidecar carries load/executable ranges,
complete ARM/Thumb dispatch keys, entry/TLS records, and complete
import/export/variable metadata. None contains an input path or content hash.

`--module-id` accepts a 1-63 character ASCII identifier beginning with a letter
and continuing with letters, digits, or underscores. Supplying one namespaces
every module-owned generated symbol while leaving shared `vita_hle_*` symbols
shared. Generated C exports the versioned, title-neutral descriptor declared by
`vitarecomp/generated_module.h`. Omitting the option retains the legacy
single-module symbol names and emission behavior.

The current public versions are generated-module ABI v4, module-sidecar schema
v4, module-report schema v4, and compose-report schema v4. Descriptor
`abi_version` and `struct_size` are first so a consumer rejects an incompatible
record before reading later fields. Each function-import record carries its
NIDs, local dispatch key, import-table target site, public name, strict policy,
and writable direct-call target slot. TLS-bearing descriptors carry explicit
power-of-two alignment; composition records deterministic descriptor
index/order, expected offset, and aggregate size/alignment.

`compose` consumes an unordered collection containing exactly one
`.module.json` and one `.report.json` for every module. It accepts only the
current complete schemas, sorts modules by load address and ID, and rejects
range overlap, duplicate full ARM/Thumb dispatch keys, ambiguous IDs/exports,
metadata disagreement, and an entry key that does not resolve exactly once.
It writes a generated registry C/header pair plus aggregate `.report.json` and
`.report.txt` files. The registry references the module-ID-qualified
descriptors, exports the validated aggregate TLS layout, and installs one
key-sorted dispatch table.

After strict loading and module-table validation, hosts bind the set with
`vita_binding_install_module_set` from `vitarecomp/binding.h`. The binder
combines strict-loader post-link metadata with the generated descriptors,
rejects duplicate NID exports and exact ARM/Thumb-key disagreements, resolves
function imports only to generated exports, exactly registered HLE functions,
or named strict traps, and resolves variables only to module exports or
explicit runtime providers. It verifies loaded target/ABS32 words before one
transactional commit, updates every generated direct-call slot, and installs
the same resolved functions in one private merged dispatch table through the
checked dispatch API. A failed commit restores guest patches and target slots.
Errors contain module/record indices, not input paths or hashes. The old
`vita_dispatch_init` and generated
`vita_generated_image_init` void functions remain compatibility wrappers.

Aggregate `structuralReady`/`entryEligible` require complete metadata, resolved
entry/TLS records, PC instrumentation, and zero body cuts, unreported
partitions, or unclassified traps. Fully classified named traps remain
entry-eligible. `productionReady` is a separate zero-gap result and is false
while any classified trap remains. In strict execution, those traps are
allowed only as the first named stop: `vita_guest_call` returns the first
failure and never continues past it. Composed artifacts contain neither input
paths nor content hashes.

The strict loader consumes caller-owned immutable bytes and never opens a path.
A host preflights and transactionally loads the complete module set, validates
post-link tables/relocations, installs binding and exact-mode dispatch,
prepares stack/TLS/CPU state, and only then calls through `vita_guest_call`.
Clean return, explicit exit request, and first strict failure are distinct
results; nested or invalid calls fail before entry.

`info` accepts a SELF (`eboot.bin`, `*.suprx`) or a plain ELF/velf. The optional
NID database is a path to
[vita-headers](https://github.com/vitasdk/vita-headers) `db/360` — **loaded at
run time, not vendored**: bundling it would be permitted, but it is data, and
keeping it external means the toolkit carries no third-party source and a newer
database needs no rebuild.

## Picking a Title

The toolkit is the product; a game brought up on it lives in its own repo
consuming this one as a submodule. Selection criteria, all measurable before
committing:

1. **A QA/prototype build exists** — plaintext segments, no keys, no PFS.
2. **Small `.text`** — less to reach full decode coverage on.
3. **2D** — `sceGxm` is the deepest part of phase 6; a title that leans on it
   lightly gets to "runs" sooner.
4. **Low NEON density** — `armrecomp cover` reports it.

| Title | Why | Status |
|---|---|---|
| ***Uncharted: Fight for Fortune*** (2012-11-01) | 5.6 MB `.text`, `ET_SCE_EXEC`, a turn-based card game — light on `sceGxm` | 21,495 functions emitted, **98.95% translated**, 18,563 firmware calls bound; runs into game code |

## Relationship to Other Projects

| Project | License | How it is used |
|---|---|---|
| [vita-headers](https://github.com/vitasdk/vita-headers) | MIT | The NID database — the published mapping from import NIDs to function names. The reference for the HLE work list. |
| [VitaSDK](https://github.com/vitasdk) | MIT | Headers and the published ABI for `sceGxm` / `sceKernel` / `sceCtrl`. |
| **[Vita3K](https://github.com/Vita3K/Vita3K)** | **GPLv2** | **Oracle only** — run as a separate process and compared against. No code copied, linked, or vendored. |
| [psprecomp](https://github.com/sp00nznet/psprecomp) | MIT | Sibling project for PSP. Same "`module_start` is not the program" shape, same oracle arrangement with PPSSPP. |
| [ps3recomp](https://github.com/sp00nznet/ps3recomp) | MIT | Sibling project for PS3. Same project structure and conventions. |

**Vita3K's license is the constraint that most shapes the runtime.** It is
GPLv2, so the usual "chop the emulator into a link library" move is not
available if the toolkit is to stay MIT — and the part you would most want from
it (the `sceGxm` and kernel HLE) is exactly the part you would be tempted to
vendor. There is no LGPL escape either: the arrangement that works for
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp), extracting LGPL-2.1
components from xemu, depends on QEMU deliberately dual-tracking its hardware
model under LGPL so it can be embedded. Vita3K has no equivalent.

What does carry over is everything else — independently implemented algorithms
with credit, and functional facts (NIDs, struct layouts, enum values, calling
conventions) which are not copyrightable and are most of what the shallow work
needs.

*Licenses above are the working understanding and are re-checked before any code
or data from a project is actually used.*

## Contributing

The gaps are named and ranked, which makes them pick-up-able:

- **NEON** — ~1,300 instructions, the only genuinely hard piece left in the emitter
- **Branch-target promotion** — a discovery fix, 0.65%
- **`SceLibc` / `SceLibm`** — 72 of 96 left, largely host-forwardable
- **Data-segment pointer recovery** — 551 static constructors are invisible to
  discovery because shape recovery only scans the executable segment
- **`sceGxm`** — ~85 shallow state setters, then the scene pipeline
- **GXP shader translation** — a compiler, and the largest single piece
- **A true negative for the corpus** — a module known to be encrypted, that the
  parser correctly refuses

## Credits & References

All code here is original, but it stands on a great deal of prior
reverse-engineering. With thanks to:

- **[VitaSDK / vita-headers](https://github.com/vitasdk)** — the MIT-licensed
  homebrew SDK and the NID database, without which the import table is just
  hashes.
- **[Vita3K](https://github.com/Vita3K/Vita3K)** — the best public description
  of Vita behaviour that exists. Used as a behavioural oracle and as
  documentation. **Reference only; no code copied.**
- The **Vita homebrew and reverse-engineering community**, whose published work
  on the SELF format, the module info structures and the NID scheme is what
  makes any of this tractable.

## Renderer contract

The stable C renderer ABI and production Windows Direct3D 12 and Vulkan
backends, including adapter/swapchain/resource/command support, ownership and
queue rules, structured errors, validation conformance, the canonical HLSL
pipeline, and pinned DXC discovery policy are documented in
[docs/GPU.md](docs/GPU.md).

## Legal

This project contains no proprietary Sony code, encryption keys, or copyrighted
material. It provides clean-room implementations of system library interfaces
based on publicly documented behaviour. Running a game requires a dump that
**you** own. Key derivation, key extraction, and the PFS layer are out of scope,
now and later.

## License

This fork is a mixed-provenance combined work. Material inherited from
`sp00nznet/vitarecomp` through
`6ed4a5c59045706292c8ec03dfe5207ecc4033a4` remains available under the MIT
License; its verbatim terms are in [LICENSES/MIT.txt](LICENSES/MIT.txt).
Modifications and additions first authored for this fork are licensed under
GPL-2.0-or-later, and the modified combined distribution is provided under
GPL-2.0-or-later as a whole. This does not revoke or replace the upstream MIT
grant.

See [LICENSE](LICENSE), [NOTICE](NOTICE),
[LICENSES/GPL-2.0-or-later.txt](LICENSES/GPL-2.0-or-later.txt), and
[docs/PROVENANCE.md](docs/PROVENANCE.md) for the complete terms and source
history.

Independent, non-commercial preservation work; not affiliated with or
endorsed by Sony Interactive Entertainment.
