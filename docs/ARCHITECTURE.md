# Architecture

How a Vita dump becomes a native executable, and what each stage is allowed to
assume about the one before it.

## The pipeline

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
                      │  generated/recomp_funcs.c
                      ▼
              ┌────────────────────┐   the runtime the generated C links against:
              │  vitarecomp (lib)  │   CPU state · memory · semantic helpers ·
              │  (src/, include/)  │   sceKernel/sceGxm HLE · dispatch
              └────────────────────┘
                      │
                      ▼   (+ a per-game host: load module, register, run, present)
              native executable — the recompiled game runs
```


## Stage 1 — the container stack

`container.c` parses SCE/SELF header, appinfo, ELF32 and program headers, and
the segment table, bounds-checking every offset it reads out of the file rather
than walking off the end of it. A retail NoNpDrm eboot is ciphertext from byte
zero; it is reported by name, not parsed into plausible garbage.

The encryption flag is cross-checked against the actual segment bytes (zlib
CMF/FLG, including the header checksum), so the report says "plain *and the
bytes agree*" — and prints a loud disagreement line if they ever do not. See
[`DECRYPT.md`](DECRYPT.md) and [`CORPUS.md`](CORPUS.md).

```
$ armrecomp info eboot.bin
size:     1680560 bytes
format:   SELF (SCE\0-wrapped ELF)

SCE header:
  version         3
  header len      0x1000
  elf filesize    3522160 bytes (decrypted)
  auth id         0x2F00000000000001
  self type       8
  sys version     0x1010000000000

ELF:
  type            ET_SCE_RELEXEC (0xFE04)
  machine         EM_ARM
  entry           0x002B36E8
  segments        5

segments:
  #   type         flags  vaddr      filesz     memsz
  0   LOAD         r-x    0x81000000 3053164    3053164
  1   LOAD         rw-    0x812EA000 7044       1204744
  2   SCE_RELA     ---    0x00000000 446924     0
  3   SCE_RELA     ---    0x00000000 4480       0
  4   SCE_VERSION  ---    0x00000000 10298      0

segment encryption:
  0   1501685    bytes  zlib      plain      (zlib header verified)
  ...
no encrypted segments, verified against zlib headers — this
module can go straight to the decoder once inflated.
```

## Stage 2 — inflate and reassemble

DEFLATE and zlib are written, not vendored (`inflate.c`, RFC 1951/1950):
canonical Huffman decoding, all three block types, overlapping back-references
handled byte-at-a-time, and the Adler-32 trailer verified. It is validated
against a fixed-Huffman *encoder* built from the spec in the test file, so the
decoder is checked against an independent implementation rather than a pasted
blob.

`armrecomp extract` takes a SELF in and writes a plain ELF32 out. Verification
is three independent numbers the file itself declares, all of which must agree;
see [`CORPUS.md`](CORPUS.md#reassembly-is-checkable-not-merely-plausible).

## Stage 3 — decode

`decode.c` handles both instruction sets with correct width determination,
control-flow extraction (targets, conditionality, interworking), IT blocks
distinguished from the NOP hints that share their encoding, and NEON/VFP
identified as a class. Coverage, class histogram, and instruction-set
determination come out of `armrecomp cover`. See
[`TRANSLATION.md`](TRANSLATION.md).

## Stage 4 — discovery

### `e_entry` does not point at code

The single most consequential thing discovery turned up. On Vita, `e_entry` names
`.sce_module_info`, not an instruction. Following it as a code address on
*Uncharted: Fight for Fortune* lands in the middle of a string table, on the
module name `cardgame` — and every function discovered from that seed would be
fiction.

The real entry is `module_start` inside that structure, and the structure also
names the export and import tables. This is psprecomp's "module_start is not
the program" in a sharper form: here the ELF entry point is not even in `.text`.

**Two address conventions, not one.** The `.sce_module_info` header fields
(`export_top`, `import_top`, `module_start`) are **segment-relative offsets**.
The pointers stored *inside* an import or export entry (`library_name`, the NID
tables, the entry tables) are **absolute virtual addresses**. Resolving one as
the other does not fault — it fails a bounds check and yields nothing, so the
symptom is a table of `(unnamed)` libraries rather than an error. The two
resolvers are kept as separate functions so the choice is explicit at every
call site.


### What discovery honestly does not know

```
$ armrecomp discover uncharted.elf
module:   cardgame   (ET_SCE_EXEC, no relocations)
seeds:
  call targets        52763
  pointer shape       22288  (heuristic; 4993 candidates rejected)
functions:            18978
  from seeds           8764
  from shape          10214  (heuristic)
bytes covered:        3424148 / 5656372  (60.5%)
  indirect sites      17970  (unresolved computed transfers)
```

Three things this does not claim:

- **54% of functions come from a heuristic.** Shape recovery requires a word
  with bit 0 set (the Thumb bit, the one genuinely reliable signal), pointing
  into `.text`, that decodes as a function prologue. An earlier version required
  only that it "decode as something", which rejected 45 of 27,281 candidates —
  0.16%, meaning it was barely filtering at all. Requiring a prologue rejects
  18%. It will still admit false positives and will miss leaf functions that
  push nothing, so the two tiers are reported separately rather than summed.
- **60.5% coverage is not 39.5% missed code.** The denominator is the whole
  executable segment, which holds literal pools and read-only data. 100% would
  indicate over-reach, not success.
- **17,970 indirect call sites are unresolved.** These are register-form `BLX`
  instructions whose destination is computed, and they are exactly what
  relocation seeding would enumerate on a 2015+ module. `ET_SCE_EXEC` has no
  relocations, so shape is all there is — which is the cost of the launch-window
  catalogue being where the exclusives are.

The selected CPU baseline additionally seeds SCE relocations, MOVW/MOVT-built
pointers, tail/interworking targets, and guarded TBB/TBH tables. Discovery
keys every code address with its instruction mode:

```
key = (address & ~1u) | (mode == Thumb ? 1u : 0u)
```

ARM and Thumb roots at the same aligned address therefore have separate
visited and function-index state. A separate mode-neutral byte bitmap keeps
coverage from counting their bytes twice. Current redacted whole-image
coverage and the correctness effects of this change are in
[`CPU_BASELINE.md`](CPU_BASELINE.md).


## Stage 5 — emit

### The pipeline runs end to end

```
$ armrecomp emit uncharted.elf recomp_funcs.c 100 path/to/vita-headers/db/360
  functions     100
  instructions  8827
  translated    7607  (86.18%)
  trapped       1220  (13.82%)
  literals      24  folded to constants
  import calls  49  bound to firmware

$ cmake --build build --config Release && ./gencheck
runtime up: sp=0x81800000  bad_access=0
lsl(1,32)=0 (ARM says 0, naive C gives 1)
asr(0x80000000,32)=0xFFFFFFFF (ARM says all ones)
5-3: c=1 (ARM sets carry when there is NO borrow)
3-5: c=0
linked ok
```

That is **SELF → ELF → decode → discover → emit → compile → link → run**, with
no key material anywhere in it.

### The output is meant to be read

`emit.c` translates one function at a time to readable C, with the disassembly
as comments:

```c
/* ---------------------------------------------------------------
 * vita_func_8100B910  --  4 instructions, 8 bytes
 * ------------------------------------------------------------- */
void vita_func_8100B910(void) {
    /* 8100B910  bl               */
    vita_func_8100B918();
    /* 8100B914  nop              */
    /* nop */
}
```

Anything untranslated emits a **named run-time trap**, never silence.
`VITARECOMP_TRACE=1` switches traps from abort to log-and-continue. It is a
discovery aid and is labelled as one: past the first missing piece the machine
state is wrong and every later trap is reached down a path that would not have
happened on hardware. What it is good for is one run naming many missing pieces
instead of one.

Emission is reachability-filtered and body-bounded. Large modules are split
into deterministic translation units that share one generated header. Every
run also writes path-free JSON and text reports containing the sorted
unsupported/trap census and each partition's function, instruction, and key
range inventory. Reports are suitable for byte-for-byte repeat checks; they do
not contain the input path, output path, or a module content hash.

### Indirect transfers need a run-time table

Once the trace stopped reporting untranslated instructions
([`LESSONS.md`](LESSONS.md#running-it-is-what-found-the-next-bug)) it started
reporting what the program was actually trying to do: **transfer control
through a register.** `module_start` does not call the real entry point,
it passes it as a *pointer* — the same shape psprecomp documented on PSP — and
C++ virtual dispatch does the rest. The module has ~18,000 such sites.

A recompiled program has no program counter, so `BX r3` cannot be translated
statically; the destination is a value known only at run time. The answer is a
sorted address→function table and a binary search
([`dispatch.c`](../src/dispatch.c)). A hit is a real transfer; a miss is a named
trap, never a silent return — jumping somewhere untranslated has to stop,
because continuing runs the caller with the callee's work undone.

Bit 0 is part of the dispatch key. Every pointer to Thumb code carries it as an
instruction-set marker, while an ARM entry at the same aligned address uses a
clear bit. Generated symbols, table entries, and `vita_dispatch_find` preserve
that bit rather than masking it, so interworking selects the correct body.

**Import stubs live in the same table.** A module reaches firmware through
pointers as well as through `BL` — `module_start` makes its very first firmware
call that way — so a dispatcher that only knows recompiled functions reports the
C++ runtime init as a missing address. The two lists have the same shape already
(guest address → `void(void)`) and both arrive sorted, so merging them keeps the
lookup a single binary search instead of adding a second one. Where both claim
an address the stub wins: being in the import table is what that address
*means*, and the bytes discovery found there are the placeholder body the loader
was supposed to overwrite.

The search assumes a sorted table, and an unsorted one does not crash — it
silently fails to find entries that are present, which looks exactly like
missing coverage. `vita_dispatch_init` checks the order once and refuses to
search a table that fails.

### Imports are bound, so firmware calls say what they are

A module never calls firmware directly. It calls a **stub** inside its own
`.text` that the loader patches at load time, so the recompiler sees an ordinary
`BL` to an ordinary address. Left unbound, every firmware call looks like an
internal call to a function whose body is placeholder filler — and recompiling
that filler would translate it and then "return" into whatever it happened to
be.

Pairing the import NID table with the entry table gives the stub address for
each function, so calls bind at emit time:

```
$ armrecomp emit uncharted.elf recomp_funcs.c 100 path/to/vita-headers/db/360
  translated    6161  (69.80%)
  import calls    49  bound to firmware

wrote recomp_funcs_imports.c
  524 import stubs, each trapping by name
```

```c
void vita_hle_sceGxmMapFragmentUsseMemory(void);
```

and in the companion file, one default per import:

```c
/* SceRtcUser::sceRtcGetCurrentTick  stub 0x814B83D0 */
void vita_hle_sceRtcGetCurrentTick(void) { vita_trap_import(0x814B83D0, 0x23F79274); }
```

That is what makes the HLE **incrementally implementable**. The generated C
links from the first build, and an unimplemented firmware call names exactly
which function the game wanted — rather than failing to link, or silently
returning zero. Implementing one means removing its stub and providing a real
body in the link.

### Building the output at full scale

Two things about compiling a whole recompiled module are worth knowing before
meeting them as errors.

**It is a section-count limit, not a size limit.** Each function becomes its own
COMDAT section, and a COFF object file can index 65,535 of them. At 20,989
functions this module goes straight past that and MSVC reports `C1128: number of
sections exceeded object file format limit`, which names a symptom nobody would
connect to the cause. `/bigobj` fixes it; other toolchains do not have the
limit. `armrecomp emit` says so in its own output rather than leaving it to be
discovered.

**The import defaults must be a static library, listed after the runtime.** One
file per import, one archive member per function. See the note in `emit.h`: a
linker pulls an archive member only for a symbol still undefined, which is what
makes implementing an import a matter of defining it rather than editing
generated code.

## Stage 6 — the runtime

### Where the runtime earns its keep

Every helper exists because the obvious C is **wrong**, not merely verbose:

- **Shifts by ≥ 32.** C leaves them undefined; ARM defines them as zero; x86
  masks the count to 5 bits, so the naive `v << 32` returns `v` *unchanged* —
  the exact opposite of the right answer. Invisible until a shift amount is
  computed rather than constant.
- **Carry on subtraction is NOT a borrow.** ARM sets it when there is *no*
  borrow. Getting it backwards inverts every unsigned comparison in the program.
- **`ADC`'s carry cannot be tested with `res < a`.** With a carry in, adding
  `0xFFFFFFFF` leaves the value unchanged while genuinely carrying, so the naive
  test silently breaks every multi-word addition.
- **Flag operands are captured before the destination is written**, because `rd`
  and `rn` are frequently the same register and the flag computation needs the
  original values.

All of it is pinned by `tests/test_runtime.c`, with the expected values derived
from the architecture's definition rather than from what the code returns.

### Runtime/HLE ownership

`vita_runtime_init` receives one backed guest-memory range and disjoint heap,
sysmem, and kernel arenas within it. Guest-address translation uses
subtraction-based bounds checks so `address + length` cannot wrap into a valid
range. Generated code retains `vita_mem_ptr` for ABI compatibility; HLE code
uses the checked copy/string/atomic interfaces and reports explicit failures.

ARM registers, flags, VFP state, tracing state, and the scheduling slice are
host thread-local. Shared runtime state is protected independently. The
scheduler gives exactly one guest thread the execution baton at a time and
saves/restores the guest register image on each handoff. A virtual clock can
advance by a configured read step and by explicit waits, making timeout and
slice tests deterministic.

The major ownership boundaries are:

| Owner | State |
|---|---|
| runtime | lifecycle, diagnostics, counters, clock and exit requests |
| memory | guest backing range and checked/exclusive writes |
| scheduler/thread manager | host workers, guest TLS, stacks and sync objects |
| HLE registry | immutable copied import metadata and dispatch functions |
| VFS | canonical directory/PSARC mounts and open handles |
| native host (future) | window/devices/presentation and mount configuration |

Callbacks cross these boundaries only after the owning lock is released.
Guest callbacks run through the generated dispatch table while that guest
thread holds the scheduler baton. Runtime diagnostics and exit hooks likewise
run outside the runtime-state mutex. TLS teardown receives the scheduler-owned
slot directly; it does not call back through scheduler initialization while
the scheduler boot mutex is held.

### Imports and unsupported behavior

The generated static import stubs remain supported. A host may additionally
register imports dynamically by `(library NID, function NID)` or by
`(library name, function name)`. Dispatch copies diagnostic names while holding
the registry lock, releases the lock, and only then invokes HLE code. Missing
imports emit a structured diagnostic and return `VITA_ERROR_UNIMPLEMENTED`;
there is no generic success fallback.

### Host filesystem and archives

The public VFS is the only path from guest names to host files. It forces
`app0:` read-only, canonicalizes existing targets (or the existing parent for
creation), and verifies containment after resolving symlinks/reparse points.
Directory and PSARC mounts share file, directory, stat, seek, and enumeration
operations. PSARC parsing bounds the header, TOC, block table, manifest,
packed spans, and archive offsets before allocation or I/O.

SceIofilemgr is layered on the VFS. FIOS2 adds completed-operation handles,
overlays, and archive mount handles but uses the same path resolver. Its
asynchronous API is currently a deterministic synchronous backend.

### Shutdown contract

The native host must stop requesting new guest work and join or otherwise
quiesce executing guest workers before `vita_runtime_shutdown`. Arbitrarily
cancelling generated C at a host instruction boundary is not safe. Shutdown
then runs registered C++ destructors, closes FIOS/I/O/VFS handles, releases
thread TLS and stacks, clears synchronization/callback tables, resets
allocators/imports/memory/clock, and supports a fresh initialization.

Shutdown is idempotent. Tests cover live-handle cleanup, main and worker TLS
teardown, destructor dispatch, counter convergence, and runtime restart.
