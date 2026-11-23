# Open runtime contracts

## Xbox queue field +8: engine and affinity semantics

**Evidence.** A local offline run on RTX 5080 reached the Xbox device queue route (default slot 48). A compute-queue description with type 2, priority 0, field +8 value 2 and node mask 0 was refused with `E_INVALIDARG`. In `runtime/shims/d3d12_vulkan_device.cpp`, `CreateCommandQueue` validates the PC flags, accepting only `DISABLE_GPU_TIMEOUT`. [Microsoft's public PC enum](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_command_queue_flags) defines NONE (0) and DISABLE_GPU_TIMEOUT (1); it does not define the Xbox field.

[upstream PR #7037](https://github.com/ItsNotRexed/Leonida/pull/7037), revision `16e3cb677c16227aeb6e23894909a5dc186da83b`, reports four compute queues with field/node pairs (1,0), (2,0), (3,0), (0,1). Independent reports there confirm that ordinary host queues get past that initialization stop. The PR describes a queue-selector interpretation, but supplies no public Microsoft contract proving it. PRs [#6954](https://github.com/ItsNotRexed/Leonida/pull/6954) and [#8258](https://github.com/ItsNotRexed/Leonida/pull/8258) instead retain bit 0 as a PC timeout flag. Passing a boot stop cannot distinguish those interpretations.

**Bounded compatibility.** `runtime/shims/d3d12_xs_device.cpp` copies only the 16-byte prefix on the configured Xbox queue route. Values 0..3 become ordinary host queue flags (`NONE`); every other value is refused before a backend call. Type, priority and node mask are preserved, the caller's description is unchanged, and PC slot 8 remains subject to PC validation. Each successful request creates a distinct queue object through the existing backend, which owns actual submissions, ordering and fences; its existing physical-queue sharing policy is unchanged. Backend errors propagate. A diagnostic records each observed source value and explicitly labels the engine/affinity semantics unverified.

**Unresolved.** The exact console field meaning, engine selection, concurrency and affinity guarantees remain unknown. The mapping neither proves those guarantees nor enables host timeout suppression. Establish the contract from public documentation or compatible independently verified implementation evidence, then add an appropriate queue/ordering regression before claiming exact emulation. CPU mocks prove the bounded adapter contract only. No console hardware validation or rendered-frame result is implied.

## Versioned console queue layout and the PresentX bridge

**Implemented (first lot).** `D3D12XboxCreateDevice` passes word 0 of its parameter block to `create_device_proxy(real, riid, out, xbox_version)` (default 0). Version 0 keeps the PC queue layout unchanged. Version `0x00020004` exactly selects the modern layout: Signal 16 and Wait 17 (fence proxy unwrapped), GetTimestampFrequency 18, GetClockCalibration 19 and GetDesc 20 route to PC slots 14..18; GetDesc accepts the PC form (queue in RCX) and the swapped console form (buffer in RCX, queue in RDX) and returns the buffer. Slot 35 is a typed `ExecuteCommandListsX(count, lists, frame word, flags)` sharing the slot-10 submission helper (unsupported-list refusal, unwrapped copy). It accepts a non-zero count, a non-null array without null entries or non-list proxies, frame word 0 and flags 0; other frame/flags values return `E_NOTIMPL` with an exact log and submit nothing. PresentX (32 modern, 30 for `0 < version < 0x20004`), SuspendX 23, ResumeX 24 and SignalX 25 are recognised and refused with `E_NOTIMPL`. Modern slots 8..15 are refused, because OpenScarlett (`87cdba6a`, MIT) keeps 8..13 in PC order with S_OK stubs at 14/15, whereas FiveStar (`18e225d2`, GPL-3.0-or-later) shifts the whole chain by two. Versions above `0x20004` refuse every queue slot past 7. The CPU test is `runtime/test/xs_queue_version_test.cpp` (CTest `xs_queue_version`).

**Open: frame word and flags at slot 35.** Both public sources ignore them and report success; FiveStar reports flags 0 or 3. Neither establishes their meaning, so a title that passes non-zero values is refused here. A measured tuple, plus an implementation of whatever the values require (frame pairing, flush), is needed before accepting them.

**Separate next lot: queue -> XboxPresentation -> presenter bridge.** This queue-routing change leaves PresentX returning `E_NOTIMPL`, and no frame is presented or claimed. Integration requirements:
- *Plane layout.* Ebiten (`d438df68`, Apache-2.0) gives field order Token, ResourceCount, ppResources, pSrcViewRects, pDestPlacementBase, ColorSpace, ScaleFilter, ExtendedDescCount, pExtendedDescs, Flags. FiveStar's inferred 0x48-byte plane is consistent with that order up to ColorSpace and is labelled inferred. OpenScarlett's header places the rects inline, which conflicts. A strict normalizer for the common 0x48-byte pointer layout can use the corroborated single-plane, single-resource SDR subset; unsupported fields must be refused. The complete parameters layout is available in the pinned Ebiten binding; compatibility with a particular retail call still needs a local trace.
- *Token source.* Device slots 78/79/80 keep a private counter that `XboxPresentation`'s `FramePacer` does not know, so the pacer would refuse their tokens as never handed out. One token source must be shared first.
- *Ownership and queue order.* The presenter must hold the real device without a cycle through the device proxy. Presentation must also be ordered after the presenting queue's submitted work on that queue's own `VkQueue`. The comment in docs/d3d12_vulkan.md 4.5 that all queues share one `VkQueue` is stale (see 4.4, `pick_queue_slot`).
- *No silent success.* A missing presenter, unknown plane fields (non-zero ScaleFilter/ExtendedDescCount/Flags, ViewCount, a token unknown to the pacer) and more than one resource must all be refused explicitly.

## Console command-list slots 131/132: unknown methods, an opt-in synchronization experiment

**Default (unchanged).** Console list slots 131 and 132 are not modelled. A call returns `E_NOTIMPL`, is traced `unsupported` and marks the list. The list's `Close` then fails, and queue `ExecuteCommandLists` refuses it until `Reset`.

**Observed calls.** Each call supplies four argument words. How many bits of each word the console method reads is not proved. Two tuples per slot have been seen:

| Slot | Local observation | Public loop measurement ([upstream PR #7076](https://github.com/ItsNotRexed/Leonida/pull/7076#issuecomment-7106977411)) |
|---|---|---|
| 132 | `(0x700000, 0, 0, 0x0000ffffffffff00)` | `(0x100000, 0, 0, 0x0000ffffffffff00)` |
| 131 | `(0xc3c0, 0x80020000, 0, 0x0000ffffffffff00)` | `(0xc380, 0x20000, 0, 0x0000ffffffffff00)` |

A static wrapper also contains `(0xc280, 0x20000, 0, 0x0000ffffffffff00)` for 131. It has not been observed at run time and is not accepted.

**Evidence, and how far it goes.** The source is AMD PAL (MIT), commit `c5e800072a32f68b6ccc4422936d96167c6e0728`. Only facts are cited here; no AMD code is imported.
- In `src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp`, the `Gfx10AcquireMemGcrCntl` bit layout gives `0xc3c0` = glkWb | glkInv | glvInv | gl1Inv | gl2Inv | gl2Wb (bits 6–9, 14, 15). `0xc380` is the same without glkWb. These are write-back and invalidate requests for the GPU caches.
- The same file asks for a full-range acquire as base 0 with every size bit set, in 256-byte units. The last word, `0x0000ffffffffff00`, resembles that size truncated to 48 bits.
- In `gfx9_plus_merged_f32_me_pm4_packets.h`, `ACQUIRE_MEM` puts `engine_sel` (micro engine = 1) in bit 31 of its second ordinal. That would fit the `0x80000000` bit of 131's second word, but bit 17 (`0x20000`) is unexplained. That word is not claimed to be the native ordinal.
- `0xc3c0` is also a register offset (`DB_OCCLUSION_COUNT0_LOW`). The equality alone does not choose between the two readings.
- No public source maps 132's `0x700000` / `0x100000` to a partial-flush event or anything else. The synchronization reading of 132 is weak.
- Microsoft's public samples call `FlushPipelineX(flags, 0, D3D12XBOX_FLUSH_RANGE_ALL)` with three arguments after `this`. They name no slot and no numeric flag values.

The evidence makes cache/pipeline synchronization the leading hypothesis for 131. It does not establish the slot-to-API mapping or 132's meaning.

**The experiment.** `PALMWAVE_EXPERIMENTAL_XBOX_FLUSH=1` (exactly `1`, read when a list is created; any other value is off) handles only the four tuples above, only on console-shaped lists from device slot 73. For each one, the backend list records one real enhanced barrier through `ID3D12GraphicsCommandList7`: a `GLOBAL` group with `SYNC_ALL` → `SYNC_ALL` and `ACCESS_COMMON` → `ACCESS_COMMON`. On the Vulkan layer this is a full `ALL_COMMANDS` / `MEMORY_READ|MEMORY_WRITE` memory dependency.

The call is traced `experimental-hypothesis`, and its first use per slot is logged prominently. The following stay strict (unsupported, list poisoned):
- any changed word, the other slot, any other slot;
- a backend list without GCL7.
An earlier unsupported mark is never cleared: even a matching experimental call cannot make that list submit.

A backend recording failure still makes `Close` fail. Implementation: `runtime/shims/d3d12_xs_device.cpp` (`list_experimental_flush`); tests: `xs_proxy_mock_test` section 7c.

**What the experiment does not show.**
- That these methods only synchronize. A register write, memory write, query, predicate or resource transition they might perform is not emulated.
- That the global barrier is equivalent. It may be stronger or weaker than the console operation; for example, it does not change resource layouts.
- That it is a compatibility fix. Its purpose is to reveal the next blocker in a diagnostic run. Any result obtained with it set (getting further in a boot, or any image) is an experimental observation and must be reported as one, not as a frame or a supported ABI.

**To close it.** Decode the fast path paired with the vtable call, if there is one, and record only the public PM4 names and fields. If 131 emits `ACQUIRE_MEM` and 132 a `VS/PS/CS_PARTIAL_FLUSH`, the effect is established without publishing executable bytes. The synthetic producer → barrier → consumer test exercises the implemented dependency, but cannot identify the console methods. Until their contract is established, the default stays strict.
