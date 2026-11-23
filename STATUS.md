# Measured status

Updated: 23 November 2026. Target: a first verified frame from the retail executable, not gameplay.

## Local preparation

Source revision: original upstream `dde82f27e994337094afc476ded0b1426f6e87e2`, matching the inherited runtime code in the initial Palmwave import. Runtime built with MSVC 19.51, Windows SDK 10.0.26100 and Vulkan SDK 1.4.363.0. Shader disassembler: LLVM 21.1.8 with AMDGPU. Host: Windows 11, Ryzen 9 9950X3D, 64 GB RAM, NVIDIA RTX 5080 16 GB, driver 621.14.

| Measurement | Result |
|---|---|
| Requested package check | Version 1.0.2.1, 108 files present, executable found |
| Carve | 26,503 shaders; one scanner candidate rejected |
| FSSM unpack | 26,503 blobs, 412 MiB total, no reported problems |
| Disassembly | 26,503 clean results |
| Runtime compilation | 684 build steps completed |
| Vulkan in the intended offline sandbox | Initialization and enumeration succeed; no rendering test implied |
| Main-corpus translation/validation | 26,503 / 26,503 translated and SPIR-V valid; 24 min 25 s translation |
| Embedded BVH programs | 52 / 52 translated and SPIR-V valid |
| Embedded FSPX programs | 29 / 29 translated and SPIR-V valid with the native decoder; previously 18 / 29 with LLVM |
| Driver pipeline creation | Running on RTX 5080; final full-corpus counts pending |
| Retail dry-run | Exit 0; five modules mapped |
| Retail initialization | Exit 0; four DLL initializations succeed |
| Retail execution | Runtime export, TLS-slot-zero and DirectStorage factory blockers cleared; next stop is a refused file interface before D3D12 |
| First rendered frame | Not verified |

The scanner candidate is not counted as a shader translation failure. Generated data and full logs are private and stored outside the authoring repository. No game assets have been added to Git.

## Isolation

The local retail runs use a Windows AppContainer token with no Internet or private-network capability, no loopback exemption, and inherited confinement for ordinary child processes. Overlay, saves and cache writes use a private writable directory. Startup/D3D12 traces are created in TEMP and copied outside the sandbox before its temporary directory is cleaned up. This is the local runner's measured configuration, not a network guarantee supplied by palmwave itself.

## Reporting rules

Record the code revision, mode, exit code, event counts, first failure class and GPU/driver. Inspect the actual presented image before claiming a first rendered frame. Keep raw game traces, addresses paired with proprietary dumps, shader listings and screenshots containing game assets out of this repository.

Project: [Palmwave / Moon AI](https://realmoon.ai). Source provenance: [UPSTREAM.md](UPSTREAM.md).

## Synthetic regression baseline

The first full semantic run executed 5,956 tests across 447 files in 245 seconds, with two files failing because of stale fixtures: CUBE d16 rejection and a constant-EXEC assumption in atomic guard checks. Focused corrections add positive coverage and negative controls without changing translator code. The complete rerun with LLVM and SPIR-V tools available passed all 5,958 tests across 447 files in 439 seconds. `compileall` passed. These are CPU/tool validations, not game rendering tests.

## First local execution

The baseline run ended with Windows status `0xC0000374`. A first-chance debugger revealed an earlier breakpoint after the dynamic `XErrorReport` lookup failed; the final status alone did not establish a heap bug. Commit `eebabab` exposes the existing offline report handler, adapted from upstream PR #6582. Both targeted runtime tests passed, and the next offline run successfully initialized the GDK runtime.

The next first-chance exception is an access violation. Debugging confirms that the executable reads TLS slot zero directly while the loader assigned its TLS index six; the host and guest blocks differ. This matches the documented `/GA` loader issue. The loader correction adapted from upstream PR #4313 passed all seven focused CTests (35.73 seconds), including real `/GA` code, existing/new threads and clone cleanup. The next offline run confirms this access violation is cleared and stops at the first unimplemented `xcrdapi.dll!DStorageGetFactory` call with `PALMWAVE_UNIMPLEMENTED=stop`. No D3D12 call or rendered frame has been observed in these runs. Raw traces, addresses and disassembly remain private.

## FSPX decoder connection

The build now explicitly selects the existing Python RDNA2 decoder for its FSPX step. All 29 programs were rebuilt and passed SPIR-V validation, preserving dispatch metadata and the strict reachable-code gap guard. Main-corpus translation was unchanged. Of the 18 LLVM-baseline outputs, 16 are byte-identical; the other two reflect wave-size and literal-printing differences described in [fspx.md](docs/fspx.md). These are CPU/tool checks; GPU semantics of the 29 programs remain unverified. The focused FSPX/resume checks cover backend selection, dispatch wave size and cache invalidation. Implementation was assisted by Claude Code Opus 5.5 and reviewed in Codex.

## DirectStorage factory

The offline run now passes factory creation and reaches `OpenFile`, which refuses an unrecognized file interface. The shim serves real CPU file/memory queues adapted from upstream PR #4348, with strict interface checks and no host-open fallback when the file-system shim is absent. Both focused CTests (`dstorage`, `unimplemented_policy`) passed in 0.53 seconds. These are synthetic CPU checks; the retail probe on the RTX 5080 still stops before any D3D12 trace or rendered frame. This is a first-chance debugger stop, not a successful guest exit.