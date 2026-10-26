# The HLE layer — measured, not estimated

The firmware surface a module needs is not a matter of opinion: the module's own
import table names every function it calls, and the MIT
[vita-headers](https://github.com/vitasdk/vita-headers) NID database turns those
hashes back into names.

```
$ armrecomp funcs uncharted.elf path/to/vita-headers/db/360
nid db:   154 files, 9274 functions
module:   cardgame
firmware libraries needed (524 functions across 39 libraries):
...
resolved 519 of 524 imported functions (99.0%)
```

**519 of 524 resolved.** That is the HLE work list, derived rather than guessed.

## Standalone runtime boundary

The runtime is a title-neutral library. It owns guest memory, CPU/TLS state,
the guest scheduler, synchronization objects, import dispatch, allocators,
clock state, and the VFS. A consuming native host owns process setup, the
window, devices, presentation, and policy. The only host-facing hooks in this
layer are the diagnostic and exit callbacks in `runtime.h`, the minimal
`host.h` contract, and mounts explicitly supplied through `vfs.h`.

The checked stack, deterministic CPU-entry reset, and ordered TLS-template
contract is specified in [ENTRY_STATE.md](ENTRY_STATE.md). That contract
deliberately stops short of claiming unimplemented Vita loader semantics.

After a host completes strict loading/binding and those entry-state checks,
`vita_guest_call` provides the same-thread boundary. It distinguishes clean
completion, an explicit runtime exit request, and the first classified named
strict failure. The failure stops immediately; diagnostic trace continuation
is not a production entry policy. Invalid, nested, or same-thread reentrant
calls fail without starting another guest call.

An explicit runtime variable provider can be paired with a checked strict
memory range. Any read, write, copy, atomic, or exclusive access overlapping
that range returns `VITA_STRICT_FAILURE_UNRESOLVED_VARIABLE` through the same
guest-call boundary, including the accessed address, variable NID, and retained
name. This permits run-driven bring-up without treating unresolved storage or a
sentinel value as successful behavior.

`app0:` is always read-only, even when a host accidentally requests a writable
mount. Other mounts opt into write access. Guest paths are normalized beneath
their mount and reject parent traversal, extra drive separators, control
characters, host absolute paths, and symlink/reparse escapes. PSARC archives
are read-only mounts and use the same normalized namespace.

## Implemented foundation

The implemented, synthetic-test-covered categories are:

- checked guest ranges, copies, moves, fills, bounded strings, atomics, and
  exclusive-monitor invalidation;
- thread-local ARM/VFP state, deterministic baton scheduling, thread/TLS
  lifecycle, mutexes, semaphores, conditions, event flags, message pipes, and
  callbacks;
- reusable/coalescing libc heap, mspace, sysmem blocks, runtime stacks, and
  a dedicated checked/coalescing guest TLS arena for per-thread runtime slots
  and composed module TLS images;
- NID/library and name/library HLE registration with structured diagnostics;
  a missing import returns `VITA_ERROR_UNIMPLEMENTED`, sets the guest error
  result, and never reports success;
- SceLibc/SceLibm forwarding with checked guest boundaries, C++ guards and
  exit destructors, SceIofilemgr, synchronous FIOS2 operations/overlays/archive
  mounts, and the ULT synchronization subset;
- measured SceGxm initialization, partitioned memory and USSE mappings,
  contexts, notification memory, finish/program state, linear textures,
  shader-patcher allocation/registration, and bounds-checked GXP header and
  parameter reflection; vertex-program creation now validates its packed ABI,
  parses bounded primary/secondary GXP streams, and emits a title-neutral USSE
  family census before failing strictly without creating an object;
- directory and read-only PSARC mounts, file/directory/stat/seek operations,
  virtual or host clocks, lifecycle counters, restart, and clean shutdown.

FIOS2 asynchronous entry points currently execute synchronously and return a
completed operation handle whose result remains queryable until deletion. ULT
implements the synchronization/storage subset, not its fiber scheduler.

## Explicitly deferred

The production DX12/Vulkan renderer contract is available to a native host.
Shader-patcher registration retains a validated GXP span. Vertex shader
creation parses that span and classifies all 35 active top-level USSE families.
This complete top-level classification is not semantic translation, and the
SPEC fallback is not proof that its subopcode is valid. Explicit illegal
families and unclassified words stop at separate named strict boundaries; an
otherwise classified census still stops at the static
translation-not-implemented boundary. It never calls the patcher allocator or
writes the output because no GXP/USSE-to-HLSL translation exists, and it does
not return a success-shaped placeholder. The remaining deferred HLE categories
are:

- the remaining SceGxm guest ABI and GXP reflection, GXP/USSE shader
  translation, scene/draw semantics, display queues, and guest-to-host
  resource mapping;
- audio, codecs, video playback, camera, microphone, and other media devices;
- controller, touch, motion/sensor, keyboard, and IME input;
- common dialogs, savedata UI/policy, trophies, app utilities, and commerce;
- networking, HTTP/SSL, NP/sign-in, matchmaking, and online services;
- the ULT fiber scheduler and genuinely asynchronous FIOS2 execution (the
  implemented synchronization and completed-operation subsets remain);
- power, location, near/adhoc, and other device/platform services; and
- title-specific patches, timing exceptions, and compatibility behavior.

Unsupported APIs must use `vita_hle_unimplemented` (or a documented firmware
error) rather than a zero/success placeholder. Vita3K source may be consulted
as code reference, but its runtime output is not a review gate; it is not
linked, vendored, or required at runtime.

## Combined validation checkpoint

The runtime foundation at
`91c91ea1c6d291ba3fcd53dca3ba9c843be36d10` was merged with the CPU and
DX12/Vulkan mainline rooted at
`eb49fca7e2074771a0a84a0c5a6f2b5b7c5d1d46`. Visual Studio 2022 Debug and
RelWithDebInfo builds retained `/W4 /WX /permissive-`; each configuration
passed all 17 CTests, including all three public homebrew
extract/emit/repeat/compile fixtures. The focused runtime, foundation,
prior-AOT, and strict-dispatch set passed 4/4, as did the supported MSVC
AddressSanitizer set.

The shutdown regression initializes main-thread TLS before teardown. It would
deadlock if teardown reacquired scheduler initialization while holding the
scheduler boot lock. Teardown instead releases the scheduler-owned TLS slot
directly. Scheduler handover callbacks, guest callbacks, diagnostics, and exit
hooks are likewise invoked only after their owning lock is released. All
framework test executables route MSVC assertion/abort reporting to stderr and
disable interactive crash UI.

## Extension rules

1. Derive an API from a public prototype/NID and record provenance.
2. Validate every guest pointer, range, string, size calculation, and output
   before touching host memory. Preserve the raw generated-code ABI only where
   compatibility requires it.
3. Keep shared host state synchronized, but release internal locks before
   diagnostics, exit hooks, or guest callbacks. Do not solve an ownership bug
   by making a mutex recursive.
4. Route filesystem access through the VFS; never embed host paths or
   title-specific mount assumptions.
5. Add a synthetic success case, boundary/failure cases, and shutdown/restart
   coverage. An unsupported call must fail explicitly.
6. Keep optional emulators and proprietary fixtures out of the dependency and
   test graph.

## Why the database is loaded, not vendored

`vita-headers` is MIT, so bundling it would be permitted. It is loaded from a
directory at run time anyway, because it is *data*: the toolkit stays free of
third-party source, a newer database needs no rebuild, and a module targeting a
different firmware needs a different directory rather than a different binary.

## SceGxm is 107 functions, and that number is misleading

`SceGxm` is the largest single import — over a fifth of everything. But the
count flattens together work that differs by orders of magnitude:

| Group | Count | Depth |
|---|---:|---|
| State setters (`sceGxmSet*`) | 28 | Struct writes |
| Texture accessors (`sceGxmTexture*`) | 20 | Struct field get/set |
| Shader & program | 23 | **Mixed — see below** |
| Render target / colour surface / depth-stencil | 14 | Struct init and bind |
| Memory mapping | 7 | Map guest pages GPU-visible |
| Sync / display | 6 | Fences and flip |
| Scene, draw, context, lifecycle | 9 | **The pipeline itself** |

Roughly **85 of the 107 are shallow** — they write a field in a structure the
driver later reads. They need care about layout and nothing else.

The real work is in two places.

### 1. GXP shader translation

```
sceGxmShaderPatcherCreateVertexProgram
sceGxmShaderPatcherCreateFragmentProgram
sceGxmShaderPatcherRegisterProgram
sceGxmShaderPatcherGetProgramFromId
```

These consume **GXP**, Sony's compiled shader container, and must produce
something a host GPU will execute. That is a compiler, and it is the single
largest piece of work in the project — Vita3K's equivalent took years.

### 2. GXP reflection

```
sceGxmProgramFindParameterByName        sceGxmProgramParameterGetCategory
sceGxmProgramParameterGetType           sceGxmProgramParameterGetComponentCount
sceGxmProgramParameterGetArraySize      sceGxmProgramParameterGetResourceIndex
sceGxmProgramGetDefaultUniformBufferSize
```

The game asks the driver about its own shaders — where a uniform lives, what
type it is, how big the default buffer is. This needs the GXP container parsed
but not translated, so it is bounded and mechanical once the format is mapped.

### 3. The pipeline

`sceGxmInitialize`, `sceGxmCreateContext`, `sceGxmBeginScene`, `sceGxmDraw`,
`sceGxmDrawInstanced`, `sceGxmEndScene`, `sceGxmMidSceneFlush`. The Vita is a
tile-based deferred renderer, and scene semantics are where that shows: what is
resolved when, and what a mid-scene flush guarantees.

**So the honest shape is ~15 functions of real work, one of which is a compiler
project, and ~92 that are shallow.**

## Why this is written rather than borrowed

Vita3K is **GPLv2** — confirmed from its own README, which says the choice is
*"largely dictated by external dependencies, most notably Unicorn"*. Unicorn is
a CPU emulator: precisely the component a static recompiler exists to replace.
The licence is therefore downstream of a part this project would never touch,
but that gives no relicensing room, because contributors licensed their work
into the GPLv2 whole.

There is no LGPL escape here. The approach that works for
[`xboxrecomp`](https://github.com/sp00nznet/xboxrecomp) — extracting
LGPL-2.1 components from xemu, which the LGPL expressly permits linking into MIT
code — depends on **QEMU deliberately dual-tracking its hardware model under
LGPL** so it can be embedded. xemu inherits that. Vita3K has no equivalent, and
nothing in it is offered under LGPL.

What does carry over is the rest of that arrangement, unchanged:

| Tier | Applies here? |
|---|---|
| LGPL components, linked | **No** — none exist for Vita |
| Independently implemented algorithms, credited | **Yes** |
| Functional facts: NIDs, struct layouts, enum values, register semantics | **Yes** |

That last row is not a consolation prize. Structure layouts, GXP container
format, enum values and calling conventions are functional facts rather than
expression, and they are most of what the shallow 92 functions need.

Vita3K is a **code reference**, not a runtime review oracle. No code is copied,
linked, or vendored.

## Order of work

1. **Bind imports.** Route recompiled calls into a NID-keyed dispatch table, so
   an unimplemented import is a named trap rather than an unresolved symbol.
2. **The shallow 92.** State setters, texture accessors, memory mapping — bulk
   but not depth.
3. **`SceLibc` / `SceLibm` and the runtime foundation.** Implemented with
   checked guest boundaries; additional imports remain incremental.
4. **GXP reflection.** Bounded once the container format is mapped.
5. **The pipeline.** `BeginScene`/`Draw`/`EndScene` against a host graphics API.
6. **GXP shader translation.** The compiler. Last; until implemented, shader
   creation must fail explicitly rather than returning a success-shaped object.

## What a single-player bring-up does not need

`SceNet`, `SceNpMatching2`, `SceHttp`, `SceNpBasic`, `SceNpScore`, `SceNetCtl`,
`SceNpUtility`, `SceNpMessage`, `SceNpManager`, `SceNpTrophy` — **112 functions
that exist only for online play and trophies.** Deferring them takes the target
from 524 to roughly 412, and is the same call `psprecomp` made about WTF's
ad-hoc networking path.
