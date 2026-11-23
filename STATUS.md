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
| Driver pipeline creation | 18,500 completed with zero failures/skips; bulk build stopped for memory pressure; full-pass counts pending |
| Retail dry-run | Exit 0; five modules mapped |
| Retail initialization | Exit 0; four DLL initializations succeed |
| Retail execution (strict default) | Vulkan device, five queues, five fences, an allocator and the first Xbox command list created; recording stops at methods 132 and 131 |
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
## Shared base command-list interface

A later console factory call requests the public base graphics-list GUID. The proxy now admits that exact request only through Xbox slot 73 and preserves its identity when queried again. PC slot 12 still returns the PC layout; revision GUIDs and unrelated interfaces remain refused. The outer Xbox factory traces its own final result, including a wrapper refusal after successful backend creation. Four focused CTests pass in 0.91 seconds, including the RTX 5080 Vulkan copy/barrier regression with validation enabled. The new base-only mock covers interface identity, reference lifetime, a shifted method and rejection boundaries.

With the explicitly experimental synchronization diagnostic still enabled, the offline RTX 5080 probe now creates and closes 581 command lists. It records 21 startup events, four successful DLL initializations, no unimplemented import, and 1782 D3D12 trace events: 1761 successes, 11 call records and ten experimental calls. No Draw, ExecuteCommandLists or Present occurs. The next failure is an attempted call through null at device slot 133, beyond the current 128-entry proxy table. This advances the diagnostic path; the strict default still stops on the unknown synchronization methods.

The resource request is a 174 MiB row-major buffer with unordered-access and an additional console flag, an unestablished initial-state word and no clear value. Its first mapped 64 KiB region is committed and write-combined. Real host-memory import, safe lifetime and the console flag/state contract remain required; a separate committed buffer would not contain the title's writes. Raw traces and memory dumps stay outside Git. No rendered frame or stable launch is claimed.
## Explicit resource-placement stop

The device table now contains 192 callable entries. Slot 133 clears its output and reports unsupported host-page placement instead of jumping through a missing entry; it does not allocate a substitute buffer. Seven focused CPU mock modes pass. The integrated build passes five focused CTests in 1.11 seconds, including the RTX 5080 Vulkan copy regression with validation enabled. The next offline experimental run retains 581 created/closed lists and reports the placement refusal in its D3D12 trace, followed by the guest's failure handling. The null-target access violation is gone; resource placement remains the blocker. No Draw, submission, Present or first frame is claimed.
## Strict host-page buffer import

The slot-133 handler now imports tracked caller-owned buffers through `VK_EXT_external_memory_host`, with no
stand-in resource or fallback copy. It requires coherent host-visible memory, an importable external buffer that
does not require a dedicated allocation, a valid supported buffer description, and pages whose lifetime the
served memory shims can hold. Unknown console flags/state words, textures, unmanaged memory, write-combined
or uncached pages remain refused. Pins are logical shim reservations, not OS page locks; raw host/NT invalidation
paths remain outside their contract. Shader V# address translation is still open.

Integrated on Palmwave `b852eef`, built with MSVC 19.51, Windows SDK 10.0.26100 and Vulkan SDK 1.4.363.0:
- Ten CPU CTests pass in 0.61 seconds: xmem, device proxy/trace and all seven Xbox proxy cases, including the
  slot-133 unsupported trace. The queue, list, shared-IID and experimental synchronization regressions remain.
- Four GPU CTests pass in 15.32 seconds on NVIDIA GeForce RTX 5080, driver 621.14, with Vulkan validation enabled
  and no validation errors: host-page import, existing-heap refusals, queue proxy and allocator proxy.
- The host-page test checks CPU writes before and after import through real GPU readback, GPU copies observed at
  the same CPU address after a fence, shared lifetime, remapping after release, and refusals while pinned.
  The allocator test also retains the existing experimental barrier path with eight copies and 1024 readback bytes.

A separate offline retail probe used the new strict runtime with the synchronization diagnostic enabled and no
host-import experiment. It recorded 21 startup events with four successful DLL initializations, and 1783 D3D12
events: 1761 `ok`, 11 `called`, 10 `experimental`, one `unsupported`. Slot 133 refuses flags `0x20004` and logs
the third word `0xffffffff`; neither is silently translated. The guest then reaches its assertion. No Draw,
Submit or Present is observed, and no first frame is verified. This confirms that the real strict handler is reached;
it does not establish the console placement contract or successful retail import.

The import implementation and its revision were written with Claude Code (Claude Opus 5.5), independently
reviewed, integrated and tested with OpenAI Codex. Compatible source attribution is in `UPSTREAM.md`.

## Opt-in host import diagnostic

`PALMWAVE_EXPERIMENTAL_XBOX_HOST_IMPORT=1` selects one bounded placement hypothesis: a plain row-major buffer
with exactly flags `0x20004`, third word `0xffffffff` and no clear value is passed to the real importer as UAV /
COMMON. The same opt-in makes newly created xmem physical-page views use normal cached host memory when the
logical window requested WC/NC; the logical protection is retained and existing views are never reprotected.
Other shapes and options remain strict, as does the default when the variable is absent or not exactly `1`.
This does not establish the meaning of console bit `0x20000` or the Xbox method's initial-layout contract.

The local synthetic cache discriminator used two fresh RTX 5080 / 621.14 processes, each with 32 mapped 64-KiB
views. RW readback matched all 2,097,152 bytes. WC import and bind succeeded but GPU readback differed in 4,205
bytes; the CPU oracle matched in both processes. Both used HOST_VISIBLE | HOST_COHERENT memory type 2 and
reported zero Vulkan validation errors. After Vulkan cleanup all 32 WC views reported RW without a VirtualProtect
call in the probe; the moment and cause of that change were not determined. This fixture supports creating RW
views for the host experiment; it proves neither the console flag meaning nor general cache equivalence.

On the integrated experimental candidate, thirteen CPU CTests pass in 0.58 seconds and the GPU experimental
case passes in 0.51 seconds on RTX 5080 / 621.14 with Vulkan validation and no errors. It imports through raw slot
133, checks CPU writes before/after import through GPU readback and GPU writes at the same CPU VA after a fence,
and preserves alias, lifetime and argument refusals. This tests buffer copies, not shader V# addressing or shader
UAV writes. Claude Code Opus 5.5 prepared the candidate; OpenAI Codex independently reviewed, integrated and tested it.

The separate offline retail probe enabled both host-import and synchronization diagnostics. It recorded 22
startup events, four successful DLL initializations and no unimplemented import. Its 1785 D3D12 events contain
1763 `ok`, 11 `called`, 10 `experimental-hypothesis` and one `experimental`. A real 182,452,224-byte buffer import
succeeds, followed by CreateDescriptorHeap and GetCPUDescriptorHandleForHeapStart, then a guest assertion.
No Draw, Submit or Present is observed. A first rendered frame is still not verified.

## Descriptor handles and active runtime name

The title allocation hooks now provide the CPU base of each console descriptor heap, and descriptor calls translate
that base to the real backend heap. A mixed native/custom heap case preserves native PC handles. The typed depth-clear
adapter preserves the float argument in its register. Four CPU/WARP CTests pass; the independent Vulkan regression on
RTX 5080 / driver 621.14 passes with validation and synchronization checks: all 192 color bytes and all 16 depth values
match, with no validation error through device destruction.

The next offline retail run passes the heap-address assertion and reaches SRV/UAV/CBV calls. It records 23 startup
events, four successful DLL initializations, no unimplemented import, and 1,800 D3D12 events: 1,767 successful, 22 call
records and 11 experimental records. The next access violation is in the PC method reached through Xbox device slot
70. That console call's argument layout requires its own adapter. No Draw, ExecuteCommandLists or Present is observed.

The active runtime is now palmwave.exe with palmwave_tls_host.dll and PALMWAVE_ options. The requested console and
presentation-window title is Palmwave Game Launcher. The renamed runtime reproduces the same measured stop above.
Targeted Python checks pass: 186 tests and 47 subtests, three skips; compileall passes. Source notices and licenses
retain their original attribution. Public descriptions and commit naming use generic Xbox Series terminology.

## Bulk compiler memory pressure

The bulk pipeline worker was running 16 compiler threads and occupied about 23 GiB of resident memory. The controller
and that worker were stopped; available RAM rose from about 6 GiB to 37.5 GiB, with no checker left running. Completed
cache checkpoints remain outside the repository. The build example now uses two jobs. Future bulk runs must use bounded
parallelism; this interruption is a resource-management decision, not evidence of a shader translation failure.

## Address-based RAW buffer view

The Xbox device slot-70 adapter resolves a live buffer by its console address, retains it for the call, checks the
whole requested range and translates the registered destination handle. It creates a real RAW SRV through the
backend. Unknown shapes, missing buffers, invalid destinations and out-of-range views are refused without changing
the destination. The implementation does not create a null stand-in or shrink a requested view.

The integrated runtime builds with MSVC 19.51 and all thirteen targeted CPU mock CTests pass in 0.35 seconds.
A separate offline run on RTX 5080 / driver 621.14, with both existing diagnostic experiments enabled, passes the
former slot-70 crash and creates the view of the real 174 MiB import. It records 23 startup events, four successful
DLL initializations, no unimplemented import and 2,235 D3D12 events: 2,200 successful, 24 call records and eleven
experimental records. The next first-chance access violation is the allocation-info call at device slot 25.
Its entry probe confirms that the result buffer precedes the device argument, unlike the PC C++ calling form.
Raw logs and disassembly remain private. No Draw, ExecuteCommandLists, Present or rendered game frame is observed.

Creating an imported-buffer view does not prove that translated shaders can read it. The descriptor uses the console
address, while the standard shader path adds the arena base. Imported-memory shader addressing and real shader
read/write validation remain separate work; no successful imported-buffer shader dispatch is claimed here.

The source-crafted GPU regression on RTX 5080 / 621.14 also passes with Vulkan and synchronization validation:
strict host import in 0.41 s, experimental host import in 0.28 s and arena compute in 1.00 s, with zero validation
errors. The compute shader reads the slot-70 arena view through the standard shader address path: all 32 values
and 32 out-of-bounds zeros match, and all 32 shifted-control values differ. This exposed and corrected an overly
strict resolver check: arena resources share their VkBuffer rather than owning an individual buffer handle.
The host-import cases validate descriptor construction and existing copies; imported-buffer shader addressing
remains unverified as described above.

## Allocation-info calling convention

Device slot 25 now accepts the measured console struct-return form as well as the PC C++ form. The description
array, flags, layout and padding reach the backend unchanged; the adapter returns its size and alignment without
substitution. Thirteen CPU mock CTests pass in 0.33 seconds, plus the dedicated slot-25 check, including stack
arguments, output canaries, reference counts and an unchanged invalid-size result. No GPU is used by these mocks.

The next offline RTX 5080 / driver 621.14 probe passes the former access violation. The first 3840x2160 RGBA8
render-target allocation query succeeds with size 33,423,360 bytes and alignment 65,536, without added console
padding. This validates that request on this backend, not the general console tiling or metadata contract. The
probe records 23 startup events, four successful DLL initializations, no unimplemented import, and 2,236 D3D12
events: 2,201 successful, 24 call records and eleven experimental records. Resource creation then reaches Xbox
device slot 127, which is beyond the PC interface and returns E_NOTIMPL; a guest assertion follows. Both existing
synchronization and host-import diagnostic options remain enabled. No Draw, ExecuteCommandLists, Present or
rendered game frame is observed.
