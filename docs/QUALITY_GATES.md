# Quality gates

The Windows workflow is public-source-only and uses no repository secrets. Its
required matrix is:

| Job | Coverage |
|---|---|
| Formatting, static quality, artifact safety | pinned `actionlint` and PSScriptAnalyzer; CMake/JSON/Markdown/license checks; `git diff --check`; changed-line C/C++ formatting; deterministic tracked-file inventory |
| MSVC Debug | `/W4 /WX`, generated-module ABI/report composition, strict loader/binding and entry-state tests, runtime/HLE and prior-semantic tests, public homebrew extract/emit/repeat/compile, DX12 WARP, Vulkan negative and smoke tests, pinned DXC shaders |
| MSVC Release | the same build and test coverage with release optimization |

Both production backends must compile and link. Configure fails rather than
dropping Vulkan when the pinned headers are absent. The invalid-ICD Vulkan test
always runs and verifies that selection does not fall back. The Vulkan GPU
smoke reports `SKIP` only when the loader exposes no usable ICD; all other
errors fail. DX12 WARP conformance is required on the Windows runner.

Run the exact local equivalents from a Windows x64 checkout:

```powershell
pwsh -NoProfile -File tools/ci/Invoke-Quality.ps1 -Bootstrap
pwsh -NoProfile -File tools/ci/Invoke-WindowsBuild.ps1 `
  -Preset windows-x64-debug -Bootstrap
pwsh -NoProfile -File tools/ci/Invoke-WindowsBuild.ps1 `
  -Preset windows-x64-release -Bootstrap
```

Bootstrap downloads only versioned public tools and verifies their SHA-256
before extraction. Outputs remain under ignored `out/`.

The focused public Gate 1 framework selection is:

```powershell
ctest --test-dir out\build\vitarecomp-public-debug -C Debug `
  --output-on-failure `
  -R "^(emit|compose-generated-image-set|runtime|entry-state|loader-strict|module-set-binding)$"
```

On 2026-10-25 it passed 6/6 tests. It uses only public/synthetic fixtures and
validates report policy, deterministic composition, strict loading/binding,
stack/TLS/CPU entry state, and guest-call outcomes. Private generated modules
are neither required nor accepted by the hosted workflow.
