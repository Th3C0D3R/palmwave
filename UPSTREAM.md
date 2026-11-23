# Source provenance

For the pinned source and license comparison used to choose graphics fixes, see [peer compatibility work](docs/peer_compatibility.md).

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

## Device slot 25 struct return

The console-form route for device slot 25 (`GetResourceAllocationInfo`, returning the 16-byte `D3D12_RESOURCE_ALLOCATION_INFO`) is adapted from sneylol's [Leonida PR #6973](https://github.com/ItsNotRexed/Leonida/pull/6973), revision `5dbc30bed79b399b527460501c0ee33f28712096`, under GPL-3.0. Only that PR's slot-25 logic is used; slot 26 keeps the adaptation above. The call form was measured locally: return buffer in RCX, device proxy in RDX, node mask in R8, count in R9 and the description array as the first stack argument, with the buffer returned in RAX. The proxy takes that form when RDX holds a device proxy, before it reads anything from the object in RCX, and forwards the PC C++ form unchanged otherwise. The 32-bit node mask and count are truncated as the PC prototype requires.

The descriptions reach the device unchanged: same pointer, count and bytes, padding included. No console flag, layout or padding is translated, masked or cleared, and the device's answer, `UINT64_MAX` included, is returned as it is. Whether console texture descriptions get a valid allocation size is a separate open question. PR #7110 (`2f2fb2fbb7306dcb822b2b6dee0191191169a3d4`) includes later description cleanup from PR #7078; these were consulted for information only and not imported.

The synthetic `xs_proxy_mock` case (`--slot25`, also in the default run) calls raw slot 25 in the console form with a stack-passed pointer to two distinct descriptions, poisoned upper halves on the 32-bit arguments and canaries around the buffer. It also calls the PC C++ method through the proxy, with answers of ordinary values and of `UINT64_MAX`. Written with Claude Code (Claude Opus 5.5), reviewed and validated with OpenAI Codex under Moon AI. MSVC 19.51 RelWithDebInfo build passed with four parallel jobs. All 13 `xs_proxy_mock` CTests passed in 0.33 seconds with the slot-70 correction also present; the dedicated `--slot25` case exited 0. No GPU was used. The optional trace reports the backend size and alignment and marks `UINT64_MAX` as failed without changing it.

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

## Title-backed descriptor heaps and CPU descriptor handles

The `__XMemAllocationHooks__` registration, the slot-14 title backing and the device slot 17..24 handle translation are adapted from Khuraie's [Leonida PR #7021](https://github.com/ItsNotRexed/Leonida/pull/7021), revision `355201c51e6aba95c582e2df344782b22e97bb31`, under GPL-3.0. The console command-list hunks for slots 50..54, including the typed `ClearDepthStencilView` thunk, are adapted from alexander-src's [Leonida PR #7880](https://github.com/ItsNotRexed/Leonida/pull/7880), revision `0aba6c3611541f169c25f0e3a1de90db25bdd2d4`, under GPL-3.0. #7880's diff is stacked on older changes. Only its descriptor-handle hunks in `ListProxy` and the matching test ideas were used. Its other hunks, test boilerplate, shader and CMake changes, documentation and `check_tiny_frame.py` were not imported, and no whole file was replaced.

The hook layout and allocation attribute word `0x00800004` come from #7021's public source report. A local offline Xbox Series executable probe also observed the heap CPU-handle assertion before this change. With these hooks, that assertion is passed and SRV, UAV and CBV creation are reached. The next observed stop is an access violation through device slot 70. This does not establish its console contract, shader-visible descriptor-byte synchronization or a rendered frame.

### What Palmwave changed

- **Registration.** The loader registers the export after relocation. `~Loader` removes the registration after process detach and before unmapping. The registry requires the whole struct to lie inside the exporting image, and it checks both routine pointers on every snapshot, because the title may fill the struct at run time. Only one registration can be active, and a second exporting image is refused. A struct that does not fit its image registers as invalid. Two null pointers mean no hook. One null pointer, or a pointer outside the image, makes the hooks invalid. Each registration gets a generation number.
- **Allocation.** Slot 14 calls the hooks after the real heap exists. If the hook returns null, slot 14 fails with `E_OUTOFMEMORY`. #7021 fell back to the plain heap in that case. Slot 14 fails with `E_FAIL` in these cases:
  - the hooks are invalid;
  - the increment is 0;
  - the real start is unknown;
  - the address range wraps;
  - the new range overlaps a live title range, or overlaps a live real range in either direction.

  In every failure path the real heap is released first and then the backing, exactly once, through the pair that allocated it. Heap slot 9 returns the title address, and slot 10 keeps the genuine GPU start.
- **Translation.** While no title-backed heap exists, every handle passes through unchanged, as before. While one exists, a handle inside a title range is translated only if its device and heap type match, it lies on a descriptor boundary, and its whole span fits. The span covers `CopyDescriptors` range sizes, `CopyDescriptorsSimple` counts and the single-range form of `OMSetRenderTargets`. All handles outside registered custom ranges pass unchanged, including native heaps created directly on the backend. Range ends, freed ranges and other untracked addresses cannot be classified as custom; the backend remains responsible for those handles. A mixed native/custom regression covers this boundary.
- **Invalid handles.** Device slots 17..24 are void. They do not forward an invalid handle. They count it (`refused_descriptor_handle_calls`), log it and trace it as `refused`. List slots 50..54 mark the list with the existing unsupported state, so `Close` fails and the queue refuses the submission until `Reset`. #7880 forwarded more than 8 render targets untranslated. Palmwave refuses that call.
- **Slot 51.** `ClearDepthStencilView` has its own typed `WINAPI` thunk, so `FLOAT Depth` stays in XMM3. The UAV clears translate only the CPU handle, and the GPU handle passes through unchanged.

### New fields

- **`HeapProxy`:** `custom_cpu_start` (0 for a plain heap), `backing` and `release_backing`. The backing and its free routine are captured at creation and owned by the proxy after a successful creation.
- **Descriptor-range table:** one `DescriptorRange` per heap made by slot 14. Each holds the device, the heap, `real_start`, `custom_start`, `byte_size`, `increment` and `type`.
- **`DescriptorHeapBacking`:** `address`, plus an opaque `token` and `release_hook`. Through the bridge in `d3d12_xs.cpp`, these hold the registration generation and the title's free routine.

### Lock and lifetime strategy

The range table, the allocator configuration and the hook registry each have their own immortal mutex. None of them is held while the title's routines run.

On a heap's last `Release`, teardown runs in this order:
1. Remove its range.
2. Delete the proxy.
3. Release the real heap.
4. Release the device proxy.
5. Free the backing.

Translation copies its result while holding the table lock. Using a heap's handles while it is being released is the caller's error, as on the PC. A backing released after its image was unregistered is reported and not freed, because the routine's code may be gone.

### Remaining limitations

- The title's own CPU reads and writes in its backing memory are not mirrored into the real heap. Only the API calls above are translated.
- Other CPU-handle consumers are not translated. These include `CreateSampler2`, `CreateSamplerFeedbackUnorderedAccessView` and any console-only slot.
- Untracked native CPU handles pass unchanged, including heaps created directly on the backend. Only a handle inside a registered custom range can be validated as custom; addresses outside those ranges remain the backend's responsibility.
- Increments come from the host device through slot 15. Code that assumes console increment constants is not covered.
- Hook routines in a module other than the exporting image are refused.
- Which image the console runtime consults when several export the struct is not known. Palmwave uses the first registration.
- The synthetic mock checks the typed slot-51 path. It cannot show that the old integer thunk lost Depth, because XMM3 can survive it by chance. The WARP readback checks the value end to end.
- An out-of-memory failure of the range table itself is handled in code but not injected by a test.

### Tests and validation status

New synthetic tests were written in these files:
- `runtime/test/xs_proxy_mock_test.cpp`: `check_descriptor_heaps` and `check_list_descriptor_handles`.
- `runtime/test/device_proxy_test.cpp`: registry bounds and cleanup, hooked WARP heaps, and an RTV (255, 0, 128, 255) and DSV Depth 0.25 clear with readback through console slots 50, 51 and 52.
- `runtime/test/loader_test.cpp`: export registration, refusal and cleanup.

Validation on Windows/MSVC 19.51: four focused CTests pass (`loader`, `device_proxy`, `device_proxy_trace`, `xs_proxy_mock`) in 11.74 seconds. The two device tests use WARP and read back RGBA (255, 0, 128, 255) and depth 0.25; they are not NVIDIA Vulkan tests. The offline RTX 5080 / 621.14 probe records 1,800 D3D12 events and no draw, submission or presentation. Both existing flush and host-import experiments were enabled for that diagnostic probe. No retail frame is claimed.

The adaptation, tests and this note were written with Claude Code (Claude Opus 5.5) as a Moon AI project contribution. OpenAI Codex reviewed the adaptation, corrected the native-handle boundary and unpaired-allocator guard, and built and tested the change.

### Vulkan descriptor readback regression

`descriptor_proxy_gpu_test.cpp` exercises the same descriptor proxy on the Vulkan backend with an explicit
source-crafted allocation pair; loader registration remains covered by the CPU/WARP tests above. Three 4x4 colour
targets use two custom RTV handles and one handle from a heap created directly on the backend. The console list
records both OMSetRenderTargets forms, typed ClearDepthStencilView, ClearRenderTargetView, transitions and copies.
A real queue and fence precede comparison of every texel. Both allocated descriptor backings are released once.
Early error paths wait for GPU work before resources are destroyed.

Validation: the focused CTest passed in 0.41 seconds on NVIDIA GeForce RTX 5080, driver 621.14, with Vulkan and
synchronization validation enabled. All 192 colour bytes and all 16 depth values matched; depth was exactly 0.25.
There were zero validation errors through resource and device destruction. This is a synthetic readback test,
not a retail frame or proof of the console descriptor contract. The test was written with Claude Opus 5.5 and
reviewed, hardened for error-path cleanup, built and executed with OpenAI Codex.

## Device slot 70: raw buffer SRV by console VA

The next stop after the heap hooks was device slot 70. Palmwave now implements one bounded shape of it: a raw buffer SRV whose 64-byte view description carries the buffer's GPU VA at +0x28. The contract, accepted fields and refusals are in [docs/d3d12_vulkan.md](docs/d3d12_vulkan.md#42-resources-heaps-placed-and-reserved) ("Device slot 70").

- **Sources.** The device-side lookup of a live buffer by VA is adapted from tedin7's [Leonida PR #7066](https://github.com/ItsNotRexed/Leonida/pull/7066), revision `8e700080066d7bbc0528aeaae5c8fb6c9cfdc347`, under GPL-3.0 (`add_ref_buffer_at`). [FiveStar](https://github.com/ThatRecompDev/five-star-recomp/blob/18e225d2cbd526ba710beb69b7884d6c758d9291/src/FiveStar.Loader/Shims/D3D12X/Resources/AddressViewDescription.cs), revision `18e225d2cbd526ba710beb69b7884d6c758d9291`, under GPL-3.0-or-later, independently confirms the 64-byte view layout with the VA at +0x28. No FiveStar code was copied.
- **What Palmwave changed.** Only the documented raw-buffer shape is accepted; unknown shapes are refused, padding is ignored, and a refusal leaves the destination unchanged and holds no reference. The VA must resolve to a real, live buffer of the same device that holds the whole range. Nothing is cut to fit, and no resource-data copy or shadow is created. Ownership is checked against the device state, not only the shared VA index. The reference is taken with `try_add_ref` under the index lock and released once after PC slot 18 writes the view. The destination goes through the same title-handle translation as slots 17..24.
- **Tests.** `xs_proxy_mock_test` (`check_srv_at_va`, `--slot70`) covers the raw ABI and every refusal on the CPU. `d3d12_compute_test` creates its raw SRV slot 1 through slot 70 on an arena buffer and reads it in a compute shader. `d3d12_host_placed_test` (`check_slot70`) compares the slot-70 descriptor over a real host import with PC slot 18's. A shader read through a host-import view is not run: its V# base is the title's host VA and the shader adds `heap_base`, so that path remains open.
- **Validation (2026-11-23).** MSVC x64 RelWithDebInfo, four build jobs: all 13 `xs_proxy_mock*` CPU tests passed (0.35 s). On NVIDIA GeForce RTX 5080, driver 621.14, with Vulkan validation and synchronization validation enabled, `d3d12_host_placed` passed (0.41 s), `d3d12_host_placed_experimental` passed (0.28 s), and `d3d12_compute` passed (1.00 s); no validation errors. The arena test first exposed an incorrect requirement for an individually owned VkBuffer; the resolver now accepts the real shared-arena buffer binding. The shader oracle matched 32 values and 32 out-of-bounds zeros, with all 32 shifted-control values different. This proves the arena path; the host-import tests prove descriptor construction and the existing import/copy checks, not shader addressing of the imported VA. OpenAI Codex ran these checks and added the explicit device-ownership guard. The offline application probe subsequently crossed slot 70 and reached 2,235 D3D12 calls, stopping at a different struct-return ABI; no draw, submission or present was observed.
- **Credits.** Implemented with Claude Code (Claude Opus 5.5) for the coding and reviewed by OpenAI Codex, as a Moon AI project contribution. This is a component result, not a frame.

## Console PresentX bridge

`runtime/shims/xbox_present_bridge.{h,cpp}`, `runtime/shims/xbox_present_wire.h`, the presentation hooks in `runtime/shims/d3d12_xs_device.{h,cpp}`, `rt::d3d12::device_has_outer` and the tests `runtime/test/xbox_present_wire_test.cpp` and `runtime/test/xbox_present_bridge_test.cpp` are written for Palmwave. No code is imported. The plane and parameter layouts follow these public sources, read for facts only:
- hajimehoshi/ebiten `internal/graphicsdriver/directx/d3d12_windows.go` at `d438df689b19544c82543ad522b36be86a5a60b7` (Apache-2.0): struct field order and types (lines 846-878), token type and null value (lines 461-464), frame-event prototypes (lines 1550-1590). Its Windows PC vtable is not used as evidence of Xbox slot numbers.
- Microsoft directxtk12modelviewer `DeviceResourcesGXDK.cpp` at `37e6552a5e479b6f128f3f412fccfcae2447a318` (MIT, lines 420-440): the zeroed struct and the fields a title sets. Its [lines 55-58 and 224-225](https://github.com/walbourn/directxtk12modelviewer/blob/37e6552a5e479b6f128f3f412fccfcae2447a318/DeviceResourcesGXDK.cpp#L224-L225) explicitly call `PresentX(0, nullptr, nullptr)` for a blank screen before resource cleanup. The existing backend represents blank as black; the sample does not specify RGB values. Blank calls have a separate counter and are not evidence of a first title-resource frame.
- FiveStar at `18e225d2cbd526ba710beb69b7884d6c758d9291` (GPL-3.0-or-later), `src/FiveStar.Loader/Shims/D3D12X/Present/PresentPlane.cs` and `PresentSlots.cs`: the same 72-byte plane through +0x28 and `PresentX(count, planes, parameters)`. None of its game-derived offsets are used.
- OpenScarlett at `87cdba6a16052a5809f904ed5725d70e50521744` (MIT) corroborates only queue slots 32/35 and version `0x20004`. Its inline-rectangle plane struct conflicts with the sources above and is not used.

This is a public-source contract, not a local retail measurement: no plane from the local title has been read. Only the strict SDR single-plane subset in `docs/d3d12_vulkan.md` section 4.5 ("Console PresentX bridge") is connected. Everything else is refused.

Written with Claude Code (Claude Opus 5.5) using only file tools, as a Moon AI project contribution. OpenAI Codex reviewed and completed the implementation, added bounded completion of the presentation fence, source reuse without intervening capture, actual queue-slot routing in the GPU fixture, minimized/closed-window handling and lifetime checks.

Validation on 2026-11-23: MSVC x64 RelWithDebInfo, four build jobs; the runtime device-creation entrypoint also compiles. All 17 focused CPU CTests passed on the initial bridge base (0.34 s); after rebasing onto the current runtime/resource snapshot, all 22 passed (0.56 s). `xbox_present_bridge` passed on NVIDIA GeForce RTX 5080 with Vulkan and synchronization validation enabled (0.59 s on the rebased snapshot, zero validation errors). Modern slot 35 submits source-crafted clears and slot 32 presents them in the actual `Palmwave Game Launcher` window. Six actual presentations complete; readback verifies five distinct frames (teal, green and red source rectangles, blue, black), with 17 explicit refusals and no failed presentation. The requested source is 96x64; native minimum-window sizing gives a 120x64 client/capture, whose output pixels are checked. No proprietary data or retail frame is involved. The first fixture run correctly exposed its overly strict assumption of a 96x64 client; the check now uses and verifies the full actual captured extent.

### Indexed frame event 2

OpenAI Codex independently implemented the bounded origin-0/index-2 scheduling model; no third-party code was copied. [FiveStar FramePacer.cs](https://github.com/ThatRecompDev/five-star-recomp/blob/18e225d2cbd526ba710beb69b7884d6c758d9291/src/FiveStar.Loader/Shims/D3D12X/Present/FramePacer.cs), revision `18e225d2cbd526ba710beb69b7884d6c758d9291`, GPL-3.0-or-later, lines 26-27, 62-69 and 95-120, establishes the compatible implementation's index 2, per-event microsecond offset and existing-token wait. Its code-derived observations are evidence of a compatible implementation, not public SDK or hardware guarantees; no application addresses or listings are reproduced. Ebiten's Apache-2.0 binding at `d438df689b19544c82543ad522b36be86a5a60b7`, `internal/graphicsdriver/directx/d3d12_windows.go` lines 1550-1551 and 1585-1586, corroborates the argument types and order. Indices 1/3 and nonzero flags remain refused. Indexed waits preserve tokens, use the retained token's fixed frame-grid origin and do not consume an origin or latency slot; tokens outside the 64-record history are refused. A finite timeout remains a timeout if the event deadline is later, even when the waiter wakes after both.

CPU validation covers independent offsets with a nonzero origin offset, blocking, finite timeout, repeated waits, unknown/null tokens, unchanged origin/latency counts, and suspend/re-registration. After a clean 24-step MSVC build with four jobs, all 23 focused CPU CTests passed (1.85 s). The bridge GPU regression, including Schedule(0), Schedule(2), and Wait(2) on the same issued token, passed on RTX 5080 with Vulkan and synchronization validation enabled, zero errors (0.73 s): six real synthetic presentations, five distinct readback-verified frames, 17 refusals, zero failed presentations. The first incremental attempt used an obsolete xbox_present.cpp object and failed the Schedule(2) assertion; the measured generated MSVC include prefix was repaired outside the source tree, then all targets rebuilt cleanly, with 54 recorded dependencies including frame_pacer.h. This extension is a Moon AI project contribution. No retail frame is implied.

## Device slot 127: CreateCommittedResource3

A local offline probe measured device slot 127 with the argument list of the public [`ID3D12Device10::CreateCommittedResource3`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device10-createcommittedresource3) (PC slot 76): heap properties, heap flags, desc1, initial layout, clear value, protected session, castable count and list, IID and output, ten words after `this`. The measured request asks for a committed texture whose desc1 flags carry bits 0x00300000 outside the PC set.

- **Sources.** The ABI layout, the typed `QueryInterface(ID3D12Device10)` call and the synthetic ten-word test shape are adapted from tedin7's [Leonida PR #7052](https://github.com/ItsNotRexed/Leonida/pull/7052), revision `356e77ec34e5959645113d68aed4119fead24d45`, under GPL-3.0. Its layout replacement, PC flag mask and slot-count guard were not taken.
- **What Palmwave changed.** The adapter forwards every word exactly as passed: no description copy, no masking or layout change, no opt-in, no fallback. The call goes to the interface pointer `QueryInterface` returns (which may differ from the base device), released once afterwards. Without `ID3D12Device10` the call returns `E_NOTIMPL`. Null heap, description or IID return `E_POINTER` without reaching the backend. The output is optional, so the S_FALSE capability check is kept; otherwise it is cleared first and stays null on failure. Every outcome is traced. A configured Xbox route moved onto 127 keeps priority, as on slot 70. The measured request's unknown flag bits reach the unchanged Vulkan backend. The subsequent offline probe confirmed `E_INVALIDARG` for resource flags, followed by the application's assertion: 2,238 D3D12 trace events, including 2 failed outcomes for the backend and adapter. No draw, submission or present was observed.
- **Tests.** `xs_proxy_mock_test` (`check_committed_resource3`, also in the default run; `--slot127` with the trace on; `--env-slots-127` for the route priority, both registered in CMake). OpenAI Codex reviewed the candidate, merged the published slot-25 regression, built with MSVC 19.51 using four parallel jobs, and ran all 15 `xs_proxy_mock` CTests: 15/15 passed in 0.38 seconds. No GPU was used by these mock tests. The separate offline probe used an RTX 5080 (driver 621.14).
- **Credits.** Coded with Claude Code (Claude Opus 5.5), reviewed and validated with OpenAI Codex, as a Moon AI project contribution. This is a component change, not a frame.

## Imported RAW buffer views and the shader host-read dependency

Original Palmwave code; no third-party code was copied. Written with Claude Code (Claude Opus 5.5) as a Moon AI
project contribution, without shell, build, GPU or game access. OpenAI Codex reviewed the code, added the foreign-device guards and tests, hardened cleanup and count maintenance, and built and ran the tests.

- **RAW V# bases** (`runtime/src/imported_view_address.h`, the RAW SRV/UAV hunks of `d3d12_vulkan_device.cpp`, the
  UAV-clear decode in `d3d12_com.cpp`): a slot-133 buffer's RAW views name its device address minus `heap_base` where
  representable and are refused otherwise (docs/d3d12_vulkan.md 3.3). Codex reported for this first candidate: core
  build 36/36; `imported_view_address` passed in 0.04 seconds; `d3d12_import_raw_view` passed in 0.62 seconds on NVIDIA
  GeForce RTX 5080, driver 621.14, with Vulkan validation and synchronization validation and zero errors, 33,344 words
  exact. That run's `heap_base` was `0x70e0000` and both imports (`0x171f0000`, `0x174a0000`) were representable. That is
  one synthetic placement, not general placement support.
- **Shader host-read dependency** (`runtime/src/vulkan_backend.h`: `VulkanQueue::host_read_barrier`, the count of
  CPU-read-back buffers in `VulkanContext`, a diagnostic counter; `runtime/test/host_read_barrier_test.cpp`; the
  two-submission GPU test). It follows the Khronos Vulkan Guide's synchronization example "CPU read back of data
  written by a compute shader" (shader write -> HOST / HOST_READ, then the fence; coherent memory needs no invalidate).
  Final validation: all three focused CTests (`imported_view_address`, `host_read_barrier`, and
  `d3d12_import_raw_view`) passed in 0.85 seconds. The GPU case passed in 0.69 seconds on NVIDIA GeForce RTX 5080,
  driver 621.14, with Vulkan and synchronization validation enabled and zero errors through device destruction.
  It made 66,112 exact word comparisons: the original 128 KiB pages immediately after the shader-only submission,
  those pages again after a separate clear/copy submission, and the READBACK results. The first submission increased
  the actual shader host-read barrier counter by one; the second left it unchanged. Foreign-device SRV/UAV calls
  left all 32 destination bytes intact. The shader was translated and passed spirv-val without translator changes
  or a corpus run. A fresh 19-step build with verified header dependencies replaced an invalid incremental build
  whose localized MSVC include prefix had left an obsolete object; no runtime workaround was introduced.

## Versioned console queue layout

The modern queue slots (Signal 16, Wait 17, GetTimestampFrequency 18, GetClockCalibration 19, GetDesc 20, SuspendX 23, ResumeX 24, SignalX 25, PresentX 32, list submission 35) come from the two public implementations below; the version threshold `0x00020004` is specifically OpenScarlett's. They were read as sources only. No code was copied from either:
- [OpenScarlett](https://github.com/californiaAlmonds/OpenScarlett) at `87cdba6a16052a5809f904ed5725d70e50521744`, MIT (2026 OpenScarlett Contributors): `src/shims/d3d12_xs_shim.cpp`, `GetPatchedVtbl` and the queue adapter.
- [FiveStar](https://github.com/ThatRecompDev/five-star-recomp) at `18e225d2cbd526ba710beb69b7884d6c758d9291`, GPL-3.0-or-later: `XboxInterfaces.cs`, `XboxQueueMethods.cs`, `Present/PresentSlots.cs`, `Present/PresentPlane.cs`, `docs/d3d12x-shim.md`.

Both are implementation evidence, not hardware or SDK documentation. Their layouts are partly inferred from a retail title's call sites, and none of those addresses or listings are reproduced here. They disagree on modern slots 8..15, so Palmwave routes none of them. Palmwave did not adopt OpenScarlett's S_OK stubs at 14/15, its `>= 0x20004` rule for later versions, or either source's unconditional success at slot 35, which ignores the frame word and flags. Plane field names were checked against ebiten (`d438df689b19544c82543ad522b36be86a5a60b7`, Apache-2.0) and DirectXTK12 model viewer `DeviceResourcesGXDK.cpp` (`37e6552a5e479b6f128f3f412fccfcae2447a318`, MIT). The strict pointer-plane subset is now connected by the Console PresentX bridge above; the wider contracts remain refused. A local probe recorded the device parameter word 0 as `0x00020004`; this change only propagates that word. Written with Claude Code (Claude Opus 5.5) without shell, build, GPU or game access; nothing was compiled or run by the author. Contract and gaps: [docs/BLOCKERS.md](docs/BLOCKERS.md), "Versioned console queue layout and the PresentX bridge".

Validation by OpenAI Codex on 2026-11-23: MSVC x64 RelWithDebInfo with four build jobs compiled the new queue test, existing synthetic proxy test and WARP device-proxy target. All 14 CPU-only CTests matching ^xs_(proxy_mock|queue_version) passed in 0.45 s; no GPU was used. The WARP executable was compiled, not run. Review fixed a missing standard header and the legacy PresentX trace name. This checks the bounded proxy adapter, not a presented frame.
## Device slots 92-95, 108-110 and 112: Device4/Device8 resource routes

Eight Xbox Series device slots now reach PC `ID3D12Device4` and `ID3D12Device8` methods: 92 `CreateCommittedResource1`, 93 `CreateHeap1`, 94 `CreateReservedResource1`, 95 `GetResourceAllocationInfo1`, 108 `GetResourceAllocationInfo2`, 109 `CreateCommittedResource2`, 110 `CreatePlacedResource1` and 112 `GetCopyableFootprints1`. The contract is in `runtime/shims/d3d12_xs_device.h`.

- **Sources.** The slot-to-method mapping comes from [OpenScarlett](third_party/notices/OpenScarlett-MIT.txt) (`src/shims/d3d12_xs_shim.cpp`), revision `87cdba6a16052a5809f904ed5725d70e50521744`, by OpenScarlett Contributors, under MIT. Its notice is kept in `third_party/notices/OpenScarlett-MIT.txt`. Two Leonida pull requests under GPL-3.0 corroborate it. tedin7's [#7490](https://github.com/ItsNotRexed/Leonida/pull/7490) at `e05b550e2bcb3f1326d511ec93214ffac5449ca5` forwards console 83-103 to PC slots 39 lower and 105-112 to PC slots 40 lower, which gives the same eight methods. justr4mzi's [#7201](https://github.com/ItsNotRexed/Leonida/pull/7201) at `1d080f4a69605a330d75210f4390d65212383096` covers slot 108 only. They were used as facts only, and no code was taken from them. Both use broad forwarding or no-ops that are not adopted here. This is public reference code. No retail call at these slots has been observed locally.
- **What Palmwave does differently.** The OpenScarlett bodies convert `DESC1` to `DESC`, ignore the sampler-feedback mip region and the protected session, retry with `COMMON`, mask flags, and in some cases substitute an independently committed resource. None of that is taken. Each call queries the PC device for the exact interface, calls that method through the returned pointer with the caller's own pointers and values, and releases the interface. The proxy reads no pointed-to bytes and passes padding, unknown flags, the state word, the clear value and the session through unchanged. The device's answer is returned unchanged, including `UINT64_MAX` allocations and failure footprints. A device without the interface gets an explicit refusal and a diagnostic: `E_NOTIMPL` with a null output, `{UINT64_MAX, UINT64_MAX}` with the per-resource array unwritten, or the PC failure footprint. It never gets a stand-in. Slots 95/108 use the slot-25 rule: the console C-ABI form (return buffer in RCX, device in RDX) is recognised before the object in RCX is read, and the PC C++ form is also accepted. Configured Xbox routes keep their priority on these slots. Slots 25/26/70/127/133 and the 104-107 mapping are unchanged.
- **Tests.** `xs_proxy_mock_test --resource-routes` (CTest `xs_proxy_mock_resource_routes`) uses mock `ID3D12Device4`/`ID3D12Device8` objects that are distinct from the base device. It checks one call on the correct PC slot, exact pointers and bytes (including a nonzero DESC1 mip region, unknown flags and padding), 32-bit words poisoned in their upper halves, a 64-bit offset, canaries, HRESULT and output propagation (`S_OK`, `E_INVALIDARG`, `E_OUTOFMEMORY`, a null output giving `S_FALSE`), null IIDs, balanced references, both struct-return forms with null buffers, and refusal on Device7 and Device3 mocks. It runs on the CPU only.
- **Validation (2026-11-23).** OpenAI Codex reviewed the implementation and ran a clean MSVC x64 RelWithDebInfo build with four jobs. All 14 `xs_proxy_mock*` CPU CTests passed (0.34 s), including the eight-route fixture; no GPU was used. The build caught and corrected a test-only size assumption: the SDK 26100 `D3D12_RESOURCE_DESC1` is 64 bytes, not 72. Configurable-route priority on these eight particular ordinals has been reviewed in source but has no new dedicated fixture. No local application call at these ordinals or GPU resource creation through them has been observed.
- **Credits.** Written with Claude Code (Claude Opus 5.5), reviewed and tested by OpenAI Codex, as a Moon AI project contribution. No rendered frame is claimed.

### Opt-in committed-texture experiment (PALMWAVE_EXPERIMENTAL_XBOX_COMMITTED_TEXTURE)

- **What it is.** A diagnostic hypothesis, off by default (docs/BLOCKERS.md). With the variable exactly `1`, only the one measured tuple reaches the backend as a description copy with Flags `0x00300005` changed to `0x5`. Everything else stays as passed and stays strict.
- **Sources.** Palmwave code only; no implementation was copied. These revisions were read for facts only: OpenScarlett (MIT) `87cdba6a16052a5809f904ed5725d70e50521744`, whose broad texture flag mask was not taken; FiveStar (GPL-3.0-or-later) `4ec61b9362f7abbf0a73183e950f589280b4819b`, for the layout-index reading that matches the public enum; AMD PAL (MIT) `c5e800072a32f68b6ccc4422936d96167c6e0728`, showing that DCC/metadata are driver-private. None defines bits `0x00300000`. The enum values come from the public Windows SDK as measured by the maintainer; no SDK header was copied. Claude Opus 5.5 reviewed the facts beforehand (read-only session `bc26d01b`).
- **Tests.** CPU: `xs_proxy_mock_committed_texture_experiment` and `xs_proxy_mock_committed_texture_with_host_import` (`--committed-texture-experiment`, also part of the default `xs_proxy_mock` run). GPU: `d3d12_committed_texture` (`runtime/test/d3d12_committed_texture_test.cpp`, with hand-written GLSL fixtures `runtime/test/glsl/cs_committed_texture_{write,read}.comp`, not translated RDNA2). OpenAI Codex ran 17/17 CPU CTests successfully (0.37 s), then the GPU test on RTX 5080 / 621.14: PASS in 0.53 s, validation and synchronization validation enabled, zero errors. The same image produced 11,520 exact clear texels and 24,576 exact SRV texels after UAV writes, with an untouched-clear control patch. The CPU fixture was corrected after review: the last byte of the 64-byte DESC1 is part of the feedback depth, not padding; only actual padding at +4 is mutated. This proves the committed-image host path, not the separate host-import/arena shader path.
- **Re-evaluation.** The first different tuple or console flag requires a common, evidenced rule and a meaningful oracle, not another tuple exception. The current default remains strict. A subsequent offline application probe created two matching 4K images and called SRV/RTV/UAV creation, then stopped at a separate allocation-accounting assertion whose contract remains under investigation: 2,249 D3D12 events, no draw, submission or present. That observation does not prove texture metadata semantics or a rendered frame.
- **Credits.** Coded with Claude Code (Claude Opus 5.5), reviewed and tested by OpenAI Codex, as a Moon AI project contribution. No game data was provided to the coding tool. This is a component experiment, not a frame.


## Experimental allocator-hook pages behind a committed texture

Original Palmwave integration built on its existing XMem hook bridge, pin registry
and Vulkan retirement path. Claude Code (Claude Opus 5.5) implemented the candidate
and synthetic tests with source-only tools as a Moon AI project contribution.
OpenAI Codex independently reviewed the lifetime, fixed partial-retirement and
enqueue allocation failures, corrected test claims, compiled, and ran the tests.
The coding session reached its configured turn limit after producing the core
and tests; it did not run the GPU or the executable.

Public comparisons used OpenScarlett (MIT, revision
`87cdba6a16052a5809f904ed5725d70e50521744`), upstream PR #7240
(jacksonslab, GPL-3.0, `d61755d4d1d02f9503f63448e1ebcc4ab361a3d0`) and
PR #7411 (garuspl, GPL-3.0, `a6e85125d86ef64bfdb4a20fd76d3964532e59c1`).
Their separate texture allocations were not imported as a storage-identity
solution. No XCL implementation was copied. The slot-127 ABI attribution above
remains unchanged.

Validation: 2/2 new CPU CTests passed in 0.17 s; the runtime hook-backed texture
GPU CTest passed in 0.87 s on RTX 5080 / 621.14, Vulkan and synchronization
validation enabled, zero errors. This proves the synthetic imported-image
contract and lifetime, not console DCC/tiling, a translated texture shader or
a rendered executable frame.

## Signed 48-bit buffer-view addresses

Original Palmwave code, prepared with OpenAI Codex and Claude Code (Claude Opus 5.5); no third-party code was copied.
The signed48-v1 marker and sign extension define this project's normalized translator/runtime contract, not a measured
console rule. The translator sign-extends decoded V# bases before offsets in both vector and scalar readers. The
runtime encodes representable imported RAW views on either side of the shader arena and refuses stale translated
modules at pipeline creation. Full-width pointers and the original CPU address registry retain their meaning.

Claude Code prepared the initial ABI-guard, runtime-fixture and documentation integration without shell, build or
GPU access. OpenAI Codex reviewed the change, added the independent native negative-placement oracle and performed
the recorded validation. The existing ClearUAV encoded-base subtraction is retained, with signed arithmetic tests.

The complete semantic suite passed 5,969 tests across 448 files in 336 seconds. The main corpus retained 26,503/26,503 translated and SPIR-V-valid modules; BVH retained 52/52 and FSPX 29/29, with no missing, extra, source-hash or container-hash mismatches. These are CPU/tool checks, not rendering results.

On RTX 5080 / driver 621.14 with Vulkan and synchronization validation, the D3D12 imported RAW regression passed in 0.68 seconds with positive relative addresses: 66,112 exact word comparisons and 256 scalar checks, including ClearUAV. Its `--below` mode returned 77 because both imports remained above the arena; it is not a negative-placement pass. The independent native `vk_import_signed48` test passed in 0.33 seconds with two real imports below the real arena: 98,304 original-memory word comparisons and 256 scalar checks, zero validation errors. It uses the standard translated shader and actual arena `heap_base`, and proves negative scalar/vector loads and stores without claiming the D3D12 descriptor or ClearUAV paths at negative placement.

The combined integration preserves the separately committed JSON parser changes. The tests use synthetic source
fixtures; no proprietary shader data, full traces or generated corpus is included. The full corpus and private
validation report remain outside the repositories. No new executable startup or rendered frame is claimed.

The combined runtime build passed. Its focused CTest run completed in 1.98 seconds with six passes and one explicit
skip for D3D12 negative placement. It included the ABI marker, JSON loader, address arithmetic, host-read barrier,
positive D3D12 import and native negative-address shader checks.

## Xbox indirect signature type 105

Palmwave adapts the slot 41 dispatch interpretation and mock recorder from mrp1no's [ProjectLeonida PR #7163](https://github.com/ProjectLeonida/Leonida/pull/7163), [revision 084c8c6](https://github.com/ProjectLeonida/Leonida/pull/7163/commits/084c8c60e7e839d0caefaaeccf41a6348b062cf1), under GPL-3.0. This PR was closed without merging. Its call-site evidence identifies compute state and a 12-byte indirect argument. Palmwave translates the observed single argument on node zero with no root signature, using a stack copy. Other shapes reach the ordinary backend validation. The console scheduling semantics are open. Five-star-recomp corroborates the type value independently. No code from five-star-recomp was used here.

The CPU proxy regression checks the value passed to the backend, the unchanged input and refusal to translate an unmeasured stride. The offline RTX 5080 diagnostic reached four successful command signatures after the fix. The fifth requested a mesh dispatch and returned unsupported. There was no indirect execution or game frame.
