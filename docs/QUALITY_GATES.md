# Quality gates

The Windows workflow is public-source-only and uses no repository secrets. Its
required matrix is:

| Job | Coverage |
|---|---|
| Formatting, static quality, artifact safety | pinned `actionlint` and PSScriptAnalyzer; CMake/JSON/Markdown/license checks; `git diff --check`; changed-line C/C++ formatting; deterministic tracked-file inventory |
| MSVC Debug | `/W4 /WX`, runtime/HLE and prior-semantic tests, public homebrew extract/emit/repeat/compile, DX12 WARP, Vulkan negative and smoke tests, pinned DXC shaders |
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
