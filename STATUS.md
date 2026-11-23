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
| Retail execution | Vulkan device, five queues, five fences, an allocator and the first Xbox command list created; recording stops at methods 132 and 131 |
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
## Graphics device reached

The named Xbox file interface now preserves the documented `GetHandle` contract. Its raw-vtable and lifetime regression brings DirectStorage coverage to 458 checks with zero failures; both focused CTests passed in 0.44 seconds on CPU. The next offline run reports `D3D12XboxCreateDevice ok` on the RTX 5080, with 19 startup events, four successful DLL initializations and no unimplemented-import event. Execution then stops on an access violation in device slot 26, before a draw or presentation. Debugging confirms a struct-return buffer is mistaken for the device pointer; this matches the `GetCustomHeapProperties` ABI issue already documented in the inherited boot chain. No frame is claimed.
## Heap-properties calling convention

The slot-26 adapter, adapted from sennecools' upstream PR #6964, passes the console return buffer and device in the measured order while retaining the PC method form. The new synthetic regression reproduces the old access violation; all six mock test modes pass with the fix. The authoring build also passed `device_proxy`, `device_proxy_trace` and `xs_proxy_mock` in 0.26 seconds (CPU/WARP). The next offline RTX 5080 run clears the access violation, creates two command queues and one fence, then receives `E_INVALIDARG` for another Xbox queue description. Its D3D12 trace contains four successful calls and one failed call; no draw or presentation is observed.
## Xbox queue descriptions

The Xbox-only queue route now accepts the four observed option values as ordinary host queues, retaining backend errors and refusing unobserved bits. This is bounded empirical compatibility, not a verified console engine/affinity model; [BLOCKERS.md](docs/BLOCKERS.md) records that distinction. PC queue calls retain their existing validation. The new regression fails before adaptation; six CPU mock modes pass afterward. The authoring build passes three focused proxy CTests in 0.37 seconds (CPU/WARP). The next offline run on RTX 5080 creates five queues, five fences and one command allocator. It then refuses the console command-list interface at device slot 73. The D3D12 trace records 12 successful calls and one failed call, with no draw or presentation.
## NVIDIA memory capabilities

A Vulkan SDK 1.4.363.0 capability query identifies the RTX 5080 as adapter 0 (vendor `10de`, device `2c02`, driver 621.14), separately from the integrated AMD adapter. It exposes `VK_EXT_external_memory_host` revision 1 with 4096-byte minimum host-pointer alignment, sparse binding, sparse buffer residency and sparse aliasing. The reported sparse address space and maximum buffer size are 1 TiB. Mesh and task shaders are supported, with 128 maximum mesh workgroup invocations. These are device capability queries, not proof of successful memory import, shader correctness or a rendered game frame.
## Console command list reached

The console slot-73 route now returns a console-shaped list with the measured full interface ID; PC slot 12 retains its PC layout. Unknown recording methods mark the list incomplete, make Close fail and prevent submission. This replaces the upstream candidate's silent return from unimplemented methods. The upload arena uses mapped GPU resources rather than host-only pointers, but its contents and address translation have not yet been exercised by the retail probe. Arena storage remains live until device destruction; reclamation and general shader address translation remain open.

Three focused proxy CTests pass in 0.37 seconds (CPU/WARP), and queue/allocator CTests pass in 14.93 seconds. The new offline RTX 5080 run reports 19 startup events, four successful DLL initializations, no unimplemented import, and 17 graphics trace events: 14 successful and three unsupported. The first unsupported event is list method 132, followed by 131 and the refused Close. Their semantic contracts remain unverified; names or no-op handlers in other repositories do not establish their effects. No Draw or Present is observed. The debugger stops on the subsequent guest assertion; its own exit code 0 is not a successful guest exit or a first frame.
## Opt-in synchronization diagnostic

The default retail path still refuses unknown list methods 132 and 131. An explicitly opt-in diagnostic, `PALMWAVE_EXPERIMENTAL_XBOX_FLUSH=1`, maps only four observed argument tuples to real global enhanced barriers. This tests a synchronization hypothesis; it does not establish the Xbox method contract and does not count as a compatibility or frame milestone. The implementation was assisted by Claude Code Opus 5.5, with independent source review and GPU regression work in Codex.

Four focused CTests pass in 1.39 seconds. The RTX 5080 / driver 621.14 Vulkan case runs with validation enabled: four experimental barriers separate eight GPU copies, with all 1024 nonuniform readback bytes matching. No extra transition, UAV barrier or intermediate fence separates each producer and consumer. CPU mocks also preserve default refusal, argument mismatch rejection, backend capability checks, interface lifetime, prior list failure and backend Close errors.

The offline experimental retail probe records 48 D3D12 trace events: 28 backend successes, ten barrier records and ten explicitly experimental calls. Five console lists close successfully. A sixth list is created by the backend but refused by the console wrapper for its requested interface, followed by a guest assertion. No Draw, ExecuteCommandLists or Present is observed. This is a diagnostic result beyond the strict default stop, not a first rendered frame or stable launch.