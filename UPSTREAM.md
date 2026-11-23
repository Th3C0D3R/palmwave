# Source provenance

Palmwave is published with an independent Git history. Its initial source snapshot is derived from [ItsNotRexed/Leonida](https://github.com/ItsNotRexed/Leonida) at commit [`dde82f27e994337094afc476ded0b1426f6e87e2`](https://github.com/ItsNotRexed/Leonida/commit/dde82f27e994337094afc476ded0b1426f6e87e2), under GPL-3.0.

Original source copyright headers, LICENSE, technical documentation and bundled third-party notices are retained. The initial import does not claim original authorship of the inherited implementation. Historical measurements in the documentation are not new Palmwave test results.

The `third_party/rpf9-explorer` submodule remains pinned to `e2939175c36a02d6ed25fa2c217b9d6ddcb8b516` from [breadbbbb/rpf9-explorer](https://github.com/breadbbbb/rpf9-explorer).

The initial Palmwave changes are branding, project documentation and a separate local measurement record. No runtime code from XCL, Five Star, OpenScarlett or xbox-emulator has been imported in this initial commit. Future imports must record their source revision, license, authorship, rationale and tests here or in a component notice.

Public implementation notes from other projects can help identify a question to test. They do not substitute for a local trace. Do not copy GPL-2.0-only code into this GPL-3.0 project. Preserve MIT and other applicable notices when incorporating compatible components.

## XErrorReport export

The flat `XErrorReport` export and its synthetic runtime/loader tests are adapted from dracbat's implementation, rebased by Kam471 in [Leonida PR #6582](https://github.com/ItsNotRexed/Leonida/pull/6582), commit [`4224c53b145ea9e68eb579817d772fe7276b943a`](https://github.com/Kam471/Leonida/commit/4224c53b145ea9e68eb579817d772fe7276b943a), under GPL-3.0. A local offline boot stopped when `GetProcAddress` could not resolve this name. The export delegates to the existing offline `IXErrorImpl` report handler; `xgameruntime` and `run_tiny_gdk_load` cover lookup and callback delivery, with an additional message/context check in Palmwave.

## Executable TLS index zero

The early TLS-index-zero reservation, host-template clone and synthetic `/GA` loader tests are adapted from mariuslozo-boop's [Leonida PR #4313](https://github.com/ItsNotRexed/Leonida/pull/4313), final revision [`a9085b661c3637ce4d154dab3c0d5b4a436d90ec`](https://github.com/mariuslozo-boop/Leonida/commit/a9085b661c3637ce4d154dab3c0d5b4a436d90ec), under GPL-3.0, including Roberto0953's rebase and existing-thread correction. The upstream commits credit Claude Opus 5.5 as co-author. Local offline debugging established direct slot-zero access while the loader had assigned the executable another TLS index. The runtime now reserves zero before its C++ initializers and keeps its own TLS at a separate, OS-backed home index; the imported tests cover host/guest isolation, existing and new threads, reservation refusal, repeated loads and temporary-clone cleanup. Their local execution results are recorded separately in `STATUS.md`.

## DirectStorage factory

`runtime/shims/dstorage.cpp`, `runtime/shims/dstorage_xs_abi.h` and `runtime/test/dstorage_test.cpp` are adapted from mariuslozo-boop's [Leonida PR #4348](https://github.com/ItsNotRexed/Leonida/pull/4348), final revision `45c39cf6f4148aebbc8a88d476bea755887d35bd`, under GPL-3.0. The PR was closed unmerged, and its author permitted it to be taken over. The upstream commits credit Claude Opus 5.5 as co-author.

A local offline boot stopped at `xcrdapi.dll!DStorageGetFactory`, with factory id `62959d11-d45f-4884-a0a3-f7862ea243ae`. The PR's CPU file and memory queues, status arrays, fences, events, cancellation and error records are kept as written.

Palmwave changed the following:
- The PR learned unknown interface ids at run time and accepted the PC `IDStorageStatusArray` id. Both are replaced by a strict table that holds the measured factory and file ids for their respective objects.
- `OpenFile` no longer falls back to the host `CreateFileW` when the file-system shim is absent.
- `xcrdapi.dll` is registered as the primary name.
- Tests were added for raw factory slots 8 and 10, the named file IID and raw `GetHandle` slot 4, strict-id rejection and unsupported-request sentinels. The file test checks caller-owned handle lifetime across explicit close and final release.

FiveStar (GPL-3.0-or-later) and OpenScarlett (MIT) were consulted for ABI facts only, through source review; no code was copied from either. Microsoft's public Xbox documentation confirms `GetHandle`, whose signature differs from the siblings' `GetFileInformation` assumptions. The base adaptation was written with Claude Code (Claude Opus 5.5); OpenAI Codex added the measured file IID, handle-ownership tests and documentation correction. Test results are recorded separately. See [docs/directstorage.md](docs/directstorage.md).

## Device slot 26 struct return

The `GetCustomHeapProperties` ABI adapter and synthetic proxy regression are adapted from sennecools's [Leonida PR #6964](https://github.com/ItsNotRexed/Leonida/pull/6964), revision `ed70b4c92002ce76934646c384dc661e1d02c491`, under GPL-3.0. A local offline debugger confirmed the return buffer in RCX and the device proxy in RDX, matching the inherited boot-chain notes. The adapter passes the request to the real device, copies its complete answer into the supplied buffer and returns that buffer's address; the PC calling convention remains supported.

Palmwave strengthens the synthetic regression with a native C++ call through `ID3D12Device::GetCustomHeapProperties` for the PC convention. The console call checks truncation of the two 32-bit arguments, untouched surrounding bytes, one backend call and preserved proxy identity and reference counts. With the old forwarder this regression raised `0xC0000005`; after the fix all six `xs_proxy_mock` CTests passed in 0.20 seconds with MSVC 19.51 on CPU only. No GPU or game rendering validation is implied. The adaptation and local validation were assisted by OpenAI Codex.

## Xbox queue compatibility

The Xbox queue adapter is adapted from sneylol (Patrick R)'s [Leonida PR #7037](https://github.com/ItsNotRexed/Leonida/pull/7037), revision `16e3cb677c16227aeb6e23894909a5dc186da83b`, under GPL-3.0. That PR's own report and independent boot reports identify field +8 values 0, 1, 2 and 3; they do not establish a public console engine/affinity contract. The PR was closed in favor of #6954, whose different treatment of value 1 is not adopted here. PR #8258 (`bb8c1291e67455697cf33300fb041901df9c2140`, Koro4e, GPL-3.0) and its reviews were consulted for null-description and PC-route pitfalls; no implementation from it is imported.

Palmwave limits the mapping to values 0..3 and the configured Xbox queue route (default slot 48). It creates ordinary host queues with `Flags=NONE`, preserves the other prefix fields, refuses unobserved bits, and leaves PC slot 8 unchanged. It does not interpret console value 1 as permission to disable the host GPU timeout. Diagnostics identify this as empirical compatibility with unverified console engine/affinity semantics; [docs/BLOCKERS.md](docs/BLOCKERS.md) records the remaining contract gap.

The synthetic regression snapshots the backend description, exercises all four values, preserves the caller's bytes, checks distinct backend queue objects, rejects unknown bits before backend calls, and checks PC flags, capability queries and backend failures. The regression fails with the old forwarder; all six `xs_proxy_mock` CTests pass after the adaptation in 0.20 seconds with MSVC 19.51 on CPU only. No GPU execution or new rendered frame is claimed by this test. The bounded adaptation and tests were assisted by OpenAI Codex.

## Console command-list proxy

The console-shaped command-list proxy, the slot-73 command-list IID table (`runtime/shims/d3d12_xs_command_list_iid.h`), the `AllocateUploadMemory` upload arena and the related mock-test hunks are adapted from NolanCotter's [Leonida PR #2490](https://github.com/ItsNotRexed/Leonida/pull/2490), at commit `084c8bd097d30541f32ff3c7f0968b8baef5fc95` of NolanCotter/Leonida, under GPL-3.0. The IID header keeps its upstream provenance note. That note cites the 1.0.2.1 executable's slot-73 IID and TomBox's GPL-3.0 xbox-emulator list. The `Reset` at console slot 139 is credited upstream to a measurement on the retail title by Pipodi, and the arena's overflow refusal to a review on that PR by daemosofchaos. Only the hunks below were imported. No whole file was replaced.

A local offline run confirms that slot 73 creates the first console command list; the next unimplemented methods are slots 132 and 131. Palmwave changed the following:
- **Creation slot.** Only device slot 73 (`CreateCommandListX`) returns the console proxy. The PC slot 12 stays forwarded unchanged and returns the PC-shaped list to callers compiled against the PC headers. This replaces the upstream `console_lists` device flag and the `LEONIDA_D3D12_LISTS` variable, so the GPU tests, `D3D12XboxCreateDevice` and `check_tiny_frame.py` are unchanged.
- **Interface boundary.** The console proxy accepts its measured full console IID and the layout-identical base interfaces. A subsequent offline probe also requests the shared base `ID3D12GraphicsCommandList` GUID at console factory slot 73. This local adaptation admits that exact creation request with base-only backend capability and preserves its console identity on later QI; only objects created with the shared GUID expose it. PC revision IIDs remain rejected, and PC callers obtain their real list through slot 12. The outer Xbox factory traces its final result separately from backend creation.
- **Unsupported slots.** Upstream recorded unmodelled list slots and returned 0. Here they return `E_NOTIMPL` and mark the list. `Close` then fails and queue `ExecuteCommandLists` refuses the submission until `Reset`.
- **Console slot 59.** It is not forwarded as `SetPredication`, because a retail-title measurement in `docs/d3d12_vulkan.md` places a dispatch there.
- **Upload arena fallback.** The fallback that handed out host `VirtualAlloc` memory, with its own address used as the GPU address, is removed. A device that cannot map an UPLOAD buffer gets 0. Allocation aligns absolute addresses, and the IA/SO views are translated in fixed stack copies instead of per-list vectors.
- **List types.** Slot 73 refuses copy and unknown type words instead of coercing them to direct, since the console DMA-list vtable is not modelled.
- **Slot count.** `detect_list_slot_count` uses the SDK's cumulative counts (GCL1 66 … GCL6 80, GCL7 81, GCL8 82).
- **Cursor position.** The pinned inline-record cursor sits at `+0x580`, past every measured field, instead of `+0x518`.
- **Not imported.** The PresentX wiring, queue layout detection, SignalX, the list-tail name table, the `gameinput` ordinal, branding and the README/doc rewrites. The queue slot-48 flag translation was not touched, and neither were the DirectStorage and slot-26 code.

The adaptation and interface-boundary correction were written with Claude Code (Claude Opus 5.5) as Moon AI project contributions, then reviewed and built with OpenAI Codex. Five focused CTests pass: three CPU/WARP proxy tests in 0.37 seconds and two queue/allocator tests in 14.93 seconds. The offline retail probe on RTX 5080 records 14 successful graphics calls and three unsupported calls, with no draw or presentation. See STATUS.md for limits.

## Device slot 133 on host pages

The slot-133 route (`CreatePlacedResourceX`) in `runtime/shims/d3d12_xs_device.cpp`/`.h`, the 192-entry device table, `VulkanDeviceInfo::host_pointer_alignment`, the `VK_EXT_external_memory_host` enablement in `create_vulkan_device_standalone` and `runtime/test/vk_test_device.h`, the buffer-import core of `Resource::create_host_buffer` in `runtime/shims/d3d12_vulkan_device.cpp`, and the slot-133 cases of `runtime/test/xs_proxy_mock_test.cpp` are adapted from tibzejoker's [Leonida PR #7218](https://github.com/ItsNotRexed/Leonida/pull/7218), head revision `a3eae955dcec4fb65864ca6c10b4ca3f2999c2d3`, under GPL-3.0 (LICENSE blob `f288702d2fa16d3cdf0035b15a9fcbc552cd88e7`). The PR was open and unmerged when it was read. Its upstream GPU evidence (an RTX 4090 copy test, later RX 9070 XT and RTX 5060 Ti imports) is not a Palmwave measurement.

Only the hunks slot 133 needs were taken. Palmwave did not take the PR's `OpenExistingHeapFromAddress` or `EXISTING_HEAPS` changes, and departed from it as follows, following independent source review:
- No fallback success. The PR substituted a committed texture for a placed texture and a committed DEFAULT buffer for a failed import. Here both return an error with a null output.
- The PR fell back from `HOST_VISIBLE | HOST_COHERENT` to any memory type. Here only coherent host-visible types are used.
- The PR checked only commitment and the cache modifiers. Here the whole rounded range must be committed `PAGE_READWRITE` without a guard page, and the width is checked for overflow.
- The PR did not tie the import to the pages' lifetime. Here the range is pinned in a new header-only registry (`runtime/shims/host_pins.h`) before it is checked. A pin is granted only for a range wholly inside one allocation the title shims own (a served `VirtualAlloc`/`XMemVirtualAlloc` allocation, xmem-mapped physical pages, or a live `XMemAlloc` block); other memory, or no linked provider, is refused. The xmem shims refuse to unmap, remap, free or replace pinned pages, and so do the title's `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `UnmapViewOfFile` and `UnmapViewOfFileEx`; `XMemFree` is deferred. The registry refuses a second import of a physical page at another address. Release waits for the submitted GPU work, then frees the memory and unpins.
- Before an import the device must report host-allocation import for the buffer's exact flags and usage (`vkGetPhysicalDeviceExternalBufferProperties`), not dedicated-only, and the buffer must not require a dedicated allocation (`vkGetBufferMemoryRequirements2`).
- The PR masked unknown Xbox flag bits, rewrote every layout word and ignored the clear value. Here the proxy forwards only a buffer with layout word `0x1`, flags among `ALLOW_UNORDERED_ACCESS` and `DENY_SHADER_RESOURCE`, and no clear value, unchanged; the device validates the buffer description and accepts only `D3D12_RESOURCE_STATE_COMMON` (0) as the third word. The PR read that word as a `D3D12_BARRIER_LAYOUT`; that reading isn't established, so the retail `0xFFFFFFFF` is refused.
- Resources on identical or contained ranges share one import with a counted owner. Partial overlaps are refused.
- The test is new: `runtime/test/d3d12_host_placed_test.cpp`, plus the pin cases in `runtime/test/xmem_test.cpp`.

No OpenScarlett, XCL or #5378/#5381 code was used. The adaptation was written with Claude Code (Claude Opus 5.5), without shell, GPU or game access, so nothing here was compiled or run by the author. OpenAI Codex then reviewed that first candidate and found gaps: no external-buffer capability query, pins over memory the shims don't own, `VirtualProtect`/`UnmapViewOfFile` bypassing pins, and a slot-133 proxy that accepted unknown semantics. Claude Code (Claude Opus 5.5) made a second coding revision to fix them, under the same conditions. OpenAI Codex then built the strict isolated candidate with MSVC 19.51: nine CPU CTests passed in 0.72 seconds and two synthetic GPU CTests passed in 1.27 seconds on NVIDIA GeForce RTX 5080 with Vulkan validation enabled and no validation errors. The host-page test proves CPU writes before/after import reach a GPU copy and GPU copies reach the same CPU pages after a fence. These measurements do not establish console flag/state semantics, shader V# addressing, or a rendered frame. The integration on Palmwave `b852eef` preserves the queue, list, shared-IID, slot-26 and no-handler slot-133 corrections. Its runtime builds and ten CPU CTests pass in 0.61 seconds; four GPU CTests pass in 15.32 seconds on the same RTX 5080 (driver 621.14), again without validation errors. See `STATUS.md` for the separate offline retail refusal.

## Experimental host import (opt-in diagnostic)

`LEONIDA_EXPERIMENTAL_XBOX_HOST_IMPORT=1` (new `runtime/shims/experimental_host_import.h`, read once per process) enables one experiment layered on the strict slot-133 path above. Without it the code behaves as the strict revision does. This is a hypothesis under test, not an established console ABI; see docs/d3d12_vulkan.md section 4.2, "Experimental host import".
- Slot 133 (`runtime/shims/d3d12_xs_device.cpp`, `experimental_placement_shape`): only the exact tuple seen in public upstream observations (plain row-major buffer, `Alignment` 0, `Flags` `0x00020004`, third word `0xFFFFFFFF`, no clear value) goes to the existing importer, as a new host description with `ALLOW_UNORDERED_ACCESS` and `COMMON`. Every attempt is logged with the original and host values and traced with an `experimental` outcome. #7218 (tibzejoker, `a3eae955dcec4fb65864ca6c10b4ca3f2999c2d3`, GPL-3.0; a later head `1d4d2952e8d0d66152282e272d719e6d56ac6049` only renames a test) masked every unknown flag and had fallbacks. None of that is taken here, and #7218 offers no proof of what Xbox bit `0x20000` means. The barrier-layout reading of `0xFFFFFFFF` is supported, not proved, by Microsoft's public `ID3D12Device10::CreatePlacedResource2` page (checked by Codex) and DirectX-Headers `61c7718954ec73dbdd43c723240fe1a0452a3724` (`D3D12_BARRIER_LAYOUT_UNDEFINED = 0xFFFFFFFF`). The PC DDI's `0x20000` (`D3D12DDI_RESOURCE_FLAG_0124_ALLOW_EXECUTE_TILE_MAPPINGS`) belongs to a different enum and is not taken as evidence.
- xmem views (`runtime/shims/xmem.cpp`, `view_protect`): with the option on, `PAGE_WRITECOMBINE`/`PAGE_NOCACHE` are removed from the view protection of physical-page mappings at creation, and `PAGE_GUARD`, the access bits and the window's recorded protection are kept. This follows hcristosm's [Leonida PR #7492](https://github.com/ItsNotRexed/Leonida/pull/7492), revision `954fcb10617a456632b3f9b8697138988e7675cd`, GPL-3.0, which does the same unconditionally. Here it is gated on the opt-in, and a `PAGE_NOACCESS` window keeps the existing read-write rule only when it carries no modifier. The broader #8458 implementation was not used. Motivating public evidence: Zunraz's reproducer (`d7d2d4076bfa2411476305d0ca8ca3fc19c6a77a`, GPL-3.0) reports corrupt GPU readback from write-combined imports on RTX 5090/4090 and correct read-write results. Those are upstream results, not Palmwave measurements, and no code from that reproducer is copied.
- Tests: new modes in `runtime/test/xs_proxy_mock_test.cpp`, `runtime/test/xmem_test.cpp` and `runtime/test/d3d12_host_placed_test.cpp`, registered as separate CTest processes with the environment set by CTest.

Written with Claude Code (Claude Opus 5.5) without shell, build, GPU or game access, so the author compiled and ran nothing. OpenAI Codex independently reviews and runs the tests. No result for this experiment is claimed until Codex reports one, and its results are kept apart from the strict measurements.

## Project identity and tools

Palmwave is developed under [Moon AI](https://realmoon.ai), the AI platform created by Stellarr Studio. The maintainer account is [fablinoo](https://github.com/fablinoo). The initial setup was AI-assisted with OpenAI Codex and OpenAI image generation; Claude Code was assigned a read-only source review. This records the actual tool provenance.

## Device slot 133 refusal

The callable tail through slot 191 and the slot-133 placement argument positions refer to
[Leonida PR #7218](https://github.com/ItsNotRexed/Leonida/pull/7218), tibzejoker,
`a3eae955dcec4fb65864ca6c10b4ca3f2999c2d3` (GPL-3.0). This correction only replaces an absent device-vtable entry
with an explicit unsupported result and a null output. It imports no memory, creates no stand-in resource and
assigns no meaning to console flags or the state word. Existing environment remaps remain limited to slots 3..127.
Implemented and reviewed by OpenAI Codex, with a synthetic raw-ABI regression including a trace assertion.
Validation: seven `xs_proxy_mock*` CTests passed on Windows/MSVC19.51; CPU only, no GPU or game test in this correction.
The real host-memory import remains a separate candidate, prepared by Claude Opus5.5 and independently reviewed.

The experimental delta was independently merged with Palmwave's strict import and current proxy/IID corrections
by OpenAI Codex. Its trace marker was corrected to the single-token `experimental` outcome expected by
`tools/boot_trace.py`, while the backend retains its real HRESULT result. Thirteen CPU CTests passed in 0.58 seconds
and the experimental GPU-copy case passed in 0.51 seconds on RTX 5080 / 621.14 with Vulkan validation and no errors.
The separate local RW/WC discriminator and the subsequent offline retail progress are recorded in `STATUS.md`;
none of those observations proves the unknown console flag's semantics or a rendered frame.
