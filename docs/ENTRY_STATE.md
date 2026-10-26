# Entry-state and TLS contract

This document describes the framework contract implemented by the native
runtime. It is an initialization contract for generated code, not a claim that
all Vita process-loader or kernel semantics are modeled.

## Checked main-stack and entry preparation

`vita_main_stack_prepare` requires an initialized runtime and a nonempty,
16-byte-aligned guest range. It rejects address overflow, unbacked memory, and
preparation while another guest thread is live. On success it records the main
stack and returns its aligned top as the initial SP.

`vita_cpu_reset` clears every represented GPR, integer NZCV/GE state, all 64
raw VFP/NEON words, FPSCR flags/control, instruction-trace state, the scheduling
slice, and the calling host thread's exclusive monitor.

`vita_cpu_prepare_module_start` additionally requires the sole live guest
thread to be the main thread. It validates the argument range and SP against
the checked main-stack configuration, resets main-thread TLS/errno execution
state, performs the CPU reset, and then applies:

- `r0 = argument_size`;
- `r1 = argument_pointer`;
- `sp = stack_pointer`; and
- `lr = return_address`.

The function does not dispatch or invoke an entry. If a generated entry is
invoked afterward, its register return value remains in `r0`. The framework
does not synthesize Vita process parameters, startup objects, or a firmware
return trampoline.

## Strict guest-call boundary

After strict module loading/binding and the checked state preparation above, a
host invokes generated code through `vita_guest_call`. A successful API call
reports exactly one of:

- clean completion;
- an explicit runtime exit request; or
- the first classified named strict failure.

Strict failures include untranslated instructions, unresolved transfers, and
unimplemented imports. The same-thread boundary stops at the first failure and
does not continue with invalid guest state. Null, nested, and same-thread
reentrant calls fail without invoking another entry. A game host can enforce
one-shot execution by placing this API behind a move-only prepared capability;
that host policy is separate from this title-neutral C API.

## TLS templates

`vita_tls_prepare` accepts an ordered array of templates. Array order is the
stable module order. Every template supplies a guest image, initialized size,
memory size, and power-of-two alignment. Preparation validates all ranges and
sizes transactionally, computes aligned non-overlapping offsets, and copies
the configuration and initialized bytes into runtime-owned storage. Later
guest writes to the source image therefore cannot change another thread's
initial TLS contents.

The runtime configuration may provide a dedicated `tls_base`/`tls_size`
arena. It must be a 16-byte-aligned range inside guest memory and disjoint
from the heap, sysmem, and kernel arenas. TLS preparation checks the real
usable capacity after alignment padding; TLS blocks are never allocated from
the kernel arena.

Each guest thread materializes its own block lazily from the dedicated arena.
The initialized prefix of every module is copied from the snapshot and the
remainder, alignment gaps, and runtime-owned kernel slots are zero-filled.
`vita_tls_materialize_current` is the checked eager form used by hosts before
entry. It fails rather than returning a zero base when the arena, alignment,
or copy is invalid. Blocks for different live threads are distinct.

`vita_tls_restart` discards materialized blocks but retains the templates, so
the next access reconstructs pristine per-thread data. Configuration, restart,
and entry preparation require a quiescent scheduler (only the main guest
thread may exist). Runtime shutdown releases blocks and templates; a fresh
runtime must configure them again.

Generated-module ABI version 4 records an explicit alignment on every TLS
descriptor. Composition emits `vita_generated_image_tls_layout`, which binds
each TLS-bearing descriptor to its descriptor index, TLS order, expected
offset, and alignment, plus aggregate data size/alignment.
`vita_tls_prepare_generated_modules` walks descriptors in composed order,
recomputes every aligned offset, and rejects missing, unsupported, or
mismatched metadata before installing templates. Two or more TLS modules are
handled by the same path as one; there is no one-module special case.

The compatibility functions `vita_main_stack_init` and `vita_tls_init` remain
available for old hosts, but they cannot report errors. New hosts must use the
checked APIs.

Public CTest coverage is `entry-state` plus `runtime`; strict loader and binder
coverage is in `loader-strict` and `module-set-binding`. The focused Gate 1
selection and its exact 6/6 result are recorded in
[CPU_BASELINE.md](CPU_BASELINE.md#checkpoint).
