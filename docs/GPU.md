# Renderer and shader contract

`include/vitarecomp/gpu.h` is the stable C boundary between a recompiled title
host and the C++20 renderer implementation. The contract is backend-neutral;
Direct3D 12 and Vulkan are the only production backend identities. On Windows,
the production registry contains both real backends when
`VITARECOMP_ENABLE_DX12=ON` and `VITARECOMP_ENABLE_VULKAN=ON` (the defaults)
and the corresponding platform headers are available. Enumeration always
reports both identities and their actual availability. Requesting an
unavailable backend returns `VITA_GPU_ERROR_BACKEND_UNAVAILABLE`; it never
selects the other production backend or the contract-test null backend.

See [DX12.md](DX12.md) for Direct3D 12 behavior and the explicit WARP
workflow. Vulkan behavior is summarized below.

## Windows Vulkan backend

`VITA_GPU_BACKEND_VULKAN` is registered as `vulkan` when CMake finds Vulkan
headers on Windows. The built library does not import `vulkan-1.dll`: it loads
the system loader and resolves entry points at runtime. A missing loader, an
installed loader with no ICD, or no compatible physical device therefore
remains a structured unavailable result instead of preventing process startup.

The backend requires Vulkan 1.3, a unified graphics/compute/transfer queue,
`VK_KHR_swapchain`, timeline semaphores, synchronization2, and dynamic
rendering. These are the native operations used to implement version 1 of the
contract rather than optional performance features. Adapter enumeration
reports device type, PCI identifiers, UUID/LUID data when exposed, memory
heaps, limits, and feature-derived capability flags.

Implemented native paths include:

- Win32 surfaces; exact color-format selection; FIFO, immediate, and mailbox
  selection with FIFO fallback; swapchain images and views; per-frame acquire
  semaphores and in-flight fences plus per-image render-finished semaphores;
- explicit resize plus out-of-date/suboptimal recreation while preserving
  borrowed image identity; minimized windows return `INVALID_STATE` until
  restored instead of spinning or selecting another backend;
- buffers and images with Vulkan memory-requirement queries and explicit
  allocation/binding, host uploads, staging uploads, and host-visible
  readback. The backend intentionally uses no heavyweight allocator because
  the contract does not yet need suballocation policy;
- samplers, color/depth views, descriptor layouts/pools/sets, SPIR-V shader
  modules, graphics and compute pipelines, dynamic rendering, draw/dispatch,
  copies, push constants, and debug labels;
- synchronization2 barriers derived from contract stage/access/state values,
  image layouts, command encoding and validation, timeline-fence submission,
  CPU waits, presentation, and deterministic reverse-order destruction.

CPU-visible optimal images, non-2D attachment views, arbitrary custom sampler
border colors, packed depth-stencil upload/readback, 3D-image readback,
sampled depth-stencil aspect selection, multi-mip storage-image bindings,
contract v1 stencil operations, direct in-process HLSL compilation,
multi-submit presentation of one acquired image, and external debug-capture
integration return `VITA_GPU_ERROR_UNSUPPORTED`; none report success without
implementing the operation. Use a readback object for capture.

When requested and installed, `VK_LAYER_KHRONOS_validation` and
`VK_EXT_debug_utils` are enabled. Error-severity validation callbacks become a
sticky backend error, so conformance cannot pass after a high-severity
validation message. Debug utils are enabled independently when available for
object names and command labels.

Configure a Debug build with an installed Vulkan SDK and the pinned DXC:

```powershell
$env:VULKAN_SDK = 'C:\VulkanSDK\1.4.363.0'
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64 `
  -DVITARECOMP_DXC_ROOT=C:\tools\dxc `
  -DVITARECOMP_DXC_REQUIRED=ON `
  -DVITARECOMP_BUILD_DX12_CONFORMANCE=ON `
  -DVITARECOMP_BUILD_VULKAN_CONFORMANCE=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

`VITARECOMP_ENABLE_VULKAN=OFF` disables the backend. The
`vulkan-conformance` CTest opens a real Win32 window, selects `vulkan`, compiles
the canonical framework HLSL to SPIR-V with the exact pinned DXC, clears and
draws a triangle, presents multiple frames, validates readback, minimizes and
restores, resizes/recreates, and repeats device startup/shutdown. It uses
CTest's skip return code only when loader/ICD availability probing says Vulkan
is unavailable. `gpu-vulkan-invalid-icd-no-fallback` forces invalid ICD paths
and verifies that a Vulkan request fails instead of selecting DX12.

## ABI and ownership

Every input and output descriptor begins with `struct_size` and
`struct_version`. Callers initialize descriptors with `VITA_GPU_INIT(type)`.
Version 1 implementations accept a larger size so fields can be appended in a
future ABI, but reject a smaller size or unknown version before reading the
descriptor.

Handles are typed, opaque 64-bit values. Zero is invalid. Each handle contains
an internal owner, slot, and generation; using a handle with the wrong device
or after destruction produces a structured error instead of reaching a native
API. Applications must destroy objects in reverse dependency order:

1. reset or destroy completed command lists;
2. destroy pipelines and binding sets;
3. destroy views, shaders, layouts, samplers, and resources;
4. destroy fences and swapchains; and
5. destroy the device.

Destroying an object that is referenced by another object or recorded command
returns `VITA_GPU_ERROR_RESOURCE_IN_USE`. Swapchain image textures and render
targets are borrowed. They are valid until the next successful resize or
swapchain destruction and cannot be destroyed directly.

## D3D12 and Vulkan model

| Contract concept | Direct3D 12 | Vulkan |
| --- | --- | --- |
| backend factory | `IDXGIFactory`/adapter discovery | instance/physical-device discovery |
| device | `ID3D12Device` plus queues | `VkDevice` plus queues |
| swapchain | `IDXGISwapChain` | `VkSwapchainKHR` |
| buffer/texture | `ID3D12Resource` | `VkBuffer`/`VkImage` plus memory |
| render/depth target | RTV/DSV descriptor | `VkImageView` attachment |
| sampler | sampler descriptor | `VkSampler` |
| binding layout | root-signature table contract | descriptor-set layout |
| binding set | populated descriptor range/table | descriptor set |
| graphics/compute pipeline | pipeline-state object | `VkPipeline` |
| command list | graphics/compute/copy command list | primary command buffer |
| resource barrier | enhanced/legacy transition barrier | pipeline barrier and image layout |
| timeline fence | `ID3D12Fence` value | timeline semaphore value |
| acquire/present | back-buffer index and `Present` | acquire-next-image and queue-present |
| readback | readback-heap resource | host-visible transfer destination |
| debug capture | PIX or backend capture integration | RenderDoc/tool integration |

Contract resource states map to D3D12 resource states and to a Vulkan access
mask, stage mask, and image layout. A transition names both its tracked
before-state and after-state. Recording rejects a wrong before-state; submit
rechecks all command lists in queue order so separately recorded lists cannot
silently race the global state. Write states are exclusive. Compatible read
states may be combined.

New resources begin owned by the graphics queue. A queue-ownership transition
is recorded on the destination queue: `source_queue` must match the tracked
owner and `destination_queue` must match the command list. Resource use on any
other queue is rejected. Backends translate that single contract operation to
the required D3D12 queue synchronization or Vulkan release/acquire barriers.

## Commands, queues, and synchronization

A command list follows one state machine:

```text
initial -> recording -> executable -> submitted
             |  render pass  |
             +---------------+
submitted --(signal fence completed, reset)--> initial
```

Graphics work and render passes use the graphics queue. Compute pipelines use
compute-capable command lists outside render passes. Copies are recorded
outside render passes. A backend may map compute or copy work to the graphics
queue when it reports that topology, but it may not reorder work or silently
change the requested backend.

Every submission has one required timeline-fence signal. Signal values must
increase strictly. CPU waits are permitted only for values already submitted.
A command list and every object it references remain live until its signal has
completed and the command list is reset or destroyed. Resize requires no
acquired image and no recorded references to swapchain images.

Public entry points are thread-safe at the device boundary, but operations on
the same command list, binding set, swapchain, readback mapping, or destruction
sequence require external serialization. Different command lists may be
recorded on different threads. Queue submission order is the call order for a
given device. Callers must not race device destruction with any other call.

## Errors

Fallible functions return `vita_gpu_result` and optionally fill a caller-owned
`vita_gpu_error`. The error includes the backend, native error code, entry-point
name, and bounded diagnostic text. Invalid descriptors and lifecycle errors are
detected before backend calls. Native device loss, timeout, shader compilation,
and I/O errors remain distinct. No API stores a hidden process-global "last
error".

Backend names accepted by `vita_gpu_backend_from_name` are exactly `dx12` and
`vulkan` (ASCII case-insensitive). `auto`, aliases, and the contract-test null
backend are rejected. The null backend is linked only into
`test_gpu_contract`; the production registry cannot register a factory marked
test-only.

## Shared HLSL and pinned DXC

One shader request carries:

- the complete shared HLSL source and a stable logical source name;
- entry point and Shader Model 6 profile;
- stage and target (`DXIL` or `SPIR-V`);
- flags; and
- named defines.

`vita_gpu_shader_request_key` validates the request and computes a SHA-256 key
over a versioned canonical representation. Defines are sorted by name for the
key, so allocation addresses and define order cannot affect shader-cache
identity. Duplicate define names, embedded NUL source bytes, and a profile that
does not match the stage are rejected.

The accepted compiler is pinned in `cmake/VitarecompDxc.cmake`:

```text
upstream release tag: v1.8.2505.1
dxc/dxcompiler binary version: 1.8.2505.32
```

Microsoft's release note explicitly distinguishes the release tag from the
binary/NuGet version; discovery compares the value reported by `dxc --version`
to `1.8.2505.32`.

Acquire that release only from the
[Microsoft DirectXShaderCompiler v1.8.2505.1 release](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.8.2505.1)
or the matching `Microsoft.Direct3D.DXC` package. The repository does not
mirror, download, or execute an installer.

CMake searches `VITARECOMP_DXC_ROOT`, the environment variable of the same
name, its `bin/x64` and `bin` children, and then `PATH`. It accepts an
executable only when `dxc --version` reports the exact pin and exposes it as
`Vitarecomp::DXC`. Discovery never downloads a tool. The contract and
production backends do not need DXC unless a conformance shader target is
enabled. `VITARECOMP_BUILD_DX12_CONFORMANCE` and
`VITARECOMP_BUILD_VULKAN_CONFORMANCE` both require the exact compiler and give
an actionable configure error if it is absent or mismatched. Other production
shader builds can set `-DVITARECOMP_DXC_REQUIRED=ON` for the same policy.

Both conformance targets compile
`tests/shaders/gpu_conformance.hlsl`: DX12 receives stripped optimized DXIL and
Vulkan receives stripped optimized SPIR-V 1.3, from the same source entry
functions and warnings-as-errors policy. Generated binaries remain in the
build tree.
Runtime HLSL module creation returns
`VITA_GPU_ERROR_SHADER_COMPILER_UNAVAILABLE`; applications compile a shader
request with the pinned compiler and cache it by
`vita_gpu_shader_request_key`.

## Readback and capture

Readback storage is explicitly sized and populated by recorded buffer or
texture copy commands. Mapping returns a typed byte pointer, byte count, and
row/slice pitches; the readback must be unmapped before destruction. Vulkan
implements buffer and image readback, including swapchain capture when the
surface exposes transfer-source usage. Debug capture is an explicit begin/end
scope and reports `UNSUPPORTED` because external tools such as RenderDoc own
that integration.

## Validation checkpoint

Renderer integration commit
`b1754cd5464604c70f39253d63b8c6163da20012` was revalidated after merging it
onto CPU baseline `5c4aa9c7ae1b04c3c832ee399a89c5d853a34df3`. A fresh
Visual Studio 2022 Debug build used `/W4 /WX /permissive- /bigobj`, found the
exact pinned DXC, enabled both production registrations, and passed the full
16/16 CTest suite.

The graphics portion passed:

- DX12 WARP buffer upload/copy/readback/shutdown;
- WARP and available hardware presentation conformance, each for three
  complete device lifecycles and four frames per lifecycle, including
  minimize/restore and resize;
- Vulkan conformance for two complete device lifecycles with validation
  enabled, including presentation, resize, swapchain recreation, and
  deterministic readback;
- an invalid-ICD Vulkan request that remained unavailable instead of falling
  back to DX12; and
- configure-time rejection of an installed DXC whose reported version did not
  match `1.8.2505.32`.

No Vulkan error-severity validation callback or other high-severity graphics
message was reported. Windows Graphics Tools was not installed on the
validation host, so DX12 conformance reported its documented debug-layer
warning and ran without an info queue; applications that explicitly request
DX12 validation still fail rather than silently disabling it.
