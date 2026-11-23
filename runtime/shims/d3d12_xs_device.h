// Xbox-ABI proxy around a PC ID3D12Device.
//
// Measured Xbox device deviations used here (vtable indices, x64):
//   slot 26   GetCustomHeapProperties, the PC slot, but the retail executable calls it with the buffer for the
//             returned struct in RCX and the device in RDX (node mask in R8, heap type in R9). A PC C++ method has
//             the device in RCX and the buffer in RDX. The thunk takes the console form when RDX holds a device
//             proxy, and forwards the PC form otherwise. Slots 25 and 43 return structs too and are still
//             forwarded in the PC form: 25 is reported called the same way later in the boot (#6982), with console
//             resource descriptions the PC method cannot take as they are, and no call of 43 has been seen.
//   slot 62   CreateComputePipelineState(desc, w2, w3, iid, out): two words (0 from XboxBvhBuilder, meaning unknown)
//             before the IID. The PC slot 62 is CreateStateObject.
//   slot 67   CreateComputePipelineState from native code (desc, iid, out): converts the Xbox native-code desc
//             (+0x00 root signature, +0x28 code, +0x30 size) to D3D12_COMPUTE_PIPELINE_STATE_DESC and forwards
//             to PC CreateComputePipelineState (slot 11). The PC slot 67 is CreateProtectedResourceSession1.
//   slot 70   void (resource desc, extended view desc, CPU destination): a raw buffer shader-resource view of the
//             buffer at a console GPU VA, written by the PC CreateShaderResourceView (slot 18) for the live buffer the
//             device's resolver finds there. Never forwarded: the PC slot 70 is ID3D12Device8::CreatePlacedResource1.
//             Only the bounded raw-buffer shape below is supported; anything else is refused with the destination
//             untouched (see "Device slot 70" further down).
//   slot 76   resource creation -> PC CreateCommittedResource (PC slot 27), with the Xbox layout word ignored.
//   slot 133  CreatePlacedResourceX(va, desc, state, clear, riid, out) -> the registered host-page handler, only for
//             a plain row-major buffer, supported flags and no clear value. Unknown flags/layouts are refused without
//             rewriting them. No handler: E_NOTIMPL, null output and an unsupported trace.
//             PALMWAVE_EXPERIMENTAL_XBOX_HOST_IMPORT=1 opts into one diagnostic tuple (see the handler contract below).
//   slot 48   CreateCommandQueueX(desc, riid, out) -> the same queue proxy as slot 8. The Xbox desc's first fields are
//             D3D12_COMMAND_QUEUE_DESC's (public HLE).
//   slot 73   CreateCommandListX(desc, allocator, initialState, riid, out) -> PC CreateCommandList(0, type, allocator,
//             initialState, IID_ID3D12GraphicsCommandList, &list), handed back behind a console-shaped list proxy
//             (below). The type is the first word of the Xbox desc; there is no node-mask argument in the console
//             call (public HLE; the PC six-argument form differs). A type other than direct, bundle or compute
//             (copy, the derived DMA type 6, anything unknown) is refused with E_NOTIMPL rather than coerced to
//             DIRECT: the console DMA-list vtable is not modelled.
//   slots 78/79/80  SetFrameIntervalX, ScheduleFrameEventX, WaitFrameEventX. A per-device token state machine answers
//             them before the PC vtable is indexed; WaitFrameEventX returns at once with a fresh non-zero token.
//             Slot 80 is outside a PC ID3D12Device11 table, so it is checked before the beyond-table refusal.
// The slots 48/73/78/79/80 are the public HLE Freminet/ScarlettX's observation, not Microsoft documentation, and can
// be moved with PALMWAVE_XS_QUEUE_X_SLOT, PALMWAVE_XS_LIST_X_SLOT, PALMWAVE_XS_FRAME_INTERVAL_SLOT,
// PALMWAVE_XS_SCHEDULE_EVENT_SLOT and PALMWAVE_XS_WAIT_EVENT_SLOT (3..127; anything else is logged once and ignored).
// The device table has 192 entries; slot 133 clears its output on refusal and never creates a stand-in resource.
// Device slots 17..24 and console list slots 50..54 translate title-backed CPU descriptor handles; slot 51
// (ClearDepthStencilView) has a typed thunk so FLOAT Depth stays in XMM3 (see "Title-backed descriptor heaps").
// Only the existing routes are configurable; their 3..127 range is unchanged.
// The measured slots 9/10 (GetCPU/GPUDescriptorHandleForHeapStart) are ID3D12DescriptorHeap methods, not device
// methods: the PC device's 9/10 are CreateCommandAllocator/CreateGraphicsPipelineState and are forwarded unchanged.
// On the PC heap those getters use the hidden return pointer in RDX; the Xbox ABI returns the 8-byte handle in
// RAX. Device slot 14 therefore wraps the created heap in a heap proxy that performs that conversion. The heap
// proxy holds a reference to the device proxy, and its ID3D12DeviceChild::GetDevice returns that same proxy.
// The proxy's heap slots 9/10 expose only the Xbox shape: a PC C++ caller using the PC header's return convention
// would not receive the value, while GetDesc and the other heap slots forward the PC convention unchanged. A
// direct one-argument call against a real PC heap is invalid, so the device test exercises the conversion on the
// wrapper and compares it with a hidden-pointer call against the real heap.
//
// Every other PC device slot is forwarded by a generated thunk that swaps `this` to the PC device. The proxy's
// vtable therefore follows the PC device order, with slots 62, 70 and 76 and the public HLE's 48/73/78/79/80 deliberately
// shadowed. The full Xbox vtable order is not known from the spec; slots beyond the PC device's own vtable return
// E_NOTIMPL and log once.
//
// Command queues and fences (device slots 8, 75, 36, and 32 for a shared fence) come back wrapped in queue and fence
// proxies of the same kind: the public chain (19 queue slots, 11 or 12 fence slots in SDK 10.0.26100) is forwarded
// in PC order, and a slot past it, up to the end of the proxy's table (128 queue, 64 fence slots), is logged with its
// first four argument words and refused with E_NOTIMPL. A slot past the table faults on the vtable read inside a
// reserved, never-committed region, and the fault is logged with the slot. That is where the console's queue methods
// (PresentX, SuspendX, ResumeX) will show up; their slots are not known and are not guessed, and that the console
// keeps the public slots is an assumption (docs/d3d12_command_objects.md §5.2). Heap, queue and fence proxies are
// unwrapped wherever a device or queue method hands them to the real runtime: queue Signal/Wait (14/15), device
// CreateSharedHandle (31), MakeResident/Evict (34/35), SetEventOnMultipleFenceCompletion (45), SetResidencyPriority
// (46) and EnqueueMakeResident (50). QueryInterface answers only the modelled interfaces; one the real object has but
// the proxy does not model is logged once and refused.
//
// Command lists have two separate paths. Device slot 73 (CreateCommandListX, console callers) returns a console-shaped
// list proxy: a 160-slot table in console order (PC order through 26, console-only 27-30, the PC chain +4 through
// 58, 60 -> EndEvent, 61 -> ExecuteIndirect, 139 -> Reset, 146 -> GCL7 Barrier, 148 AllocateUploadMemory) and the
// console object body with the inline-record cursor pinned at its limits (+0x18 = +0x28 = +0x30), so inline writers
// always take the vtable path. Any other slot is logged, traced "unsupported" and returns E_NOTIMPL; the list then
// fails Close with E_NOTIMPL and queue ExecuteCommandLists refuses to submit it until a Reset. The one exception is an
// opt-in EXPERIMENT, off by default: with PALMWAVE_EXPERIMENTAL_XBOX_FLUSH=1 (exactly) at list creation, console slots
// 131/132 with four exact argument tuples record a real global SYNC_ALL/ACCESS_COMMON barrier through
// ID3D12GraphicsCommandList7, traced "experimental-hypothesis" (not a known contract; docs/BLOCKERS.md). Queue
// ExecuteCommandLists unwraps list proxies; the list unwraps allocator (Reset), bundle and heap proxies itself. The
// console list's QueryInterface answers the measured console IID (when the real list implements its PC capability)
// and the layout-identical base chain IUnknown/ID3D12Object/ID3D12DeviceChild/ID3D12CommandList. Only proxies created
// at slot 73 with the shared ID3D12GraphicsCommandList base GUID also answer that same GUID with their console table; PC
// ID3D12GraphicsCommandList1..8 IIDs are refused, since the console table is shifted from theirs. Device slot 12
// (the PC CreateCommandList) is forwarded unchanged and returns the PC-shaped real list, for callers
// compiled against the PC headers (the synthetic GPU tests and fixtures).
#pragma once

#include <d3d12.h>

#include <cstddef>
#include <cstdint>

namespace rt::d3d12_xs {

// Wraps `real` and returns the requested device interface through `out`. Takes ownership of one reference to
// `real` on success; on failure the caller keeps its reference. Returns E_NOINTERFACE if `riid` is not a D3D12
// device interface the PC device implements.
HRESULT create_device_proxy(ID3D12Device* real, REFIID riid, void** out);

// True when `device` points at a proxy created by create_device_proxy. Used by the tests.
bool is_device_proxy(const void* device);

// IDXGIDevice1 on the device. A GDKX title asks its device for IDXGIDevice1, then GetAdapter and EnumOutputs(0),
// to find the output it gives SetFrameIntervalX (docs/d3d12_vulkan.md section 4.5). The proxy answers IDXGIObject,
// IDXGIDevice and IDXGIDevice1 itself, as one COM object with the device (same reference count, QueryInterface
// both ways). GetAdapter and GetParent return the adapter of the device behind the proxy: by default the host DXGI
// adapter with that device's LUID. A provider replaces that: `real` is the device behind the proxy, and it returns
// the adapter as `riid`. rt::d3d12::set_device_outer sets one for the Vulkan-backed device (the layer's own adapter).
//
// A title may ask D3D12XboxCreateDevice (so create_device_proxy) for IDXGIDevice1 directly. What comes back then is
// the proxy's DXGI pointer, not the proxy: is_device_proxy is false for it. Code that takes "the proxy" from a
// creation call goes through device_proxy_primary first.
using DxgiAdapterProvider = HRESULT (*)(ID3D12Device* real, REFIID riid, void** out);
// `device` is the proxy or its DXGI pointer. False, with a line on stderr, when it is neither: nothing is set then.
bool set_device_adapter_provider(void* device, DxgiAdapterProvider provider);
// The device proxy an IDXGIDevice pointer handed out by one belongs to, or null.
void* device_proxy_from_dxgi_device(void* dxgi_device);
// The device proxy itself, given the proxy or its DXGI pointer; null for anything else.
void* device_proxy_primary(void* device);

// True when `heap` is a descriptor-heap proxy created by device slot 14.
bool is_heap_proxy(const void* heap);
ID3D12DescriptorHeap* heap_proxy_real_heap(const void* heap);

// ---- Title-backed descriptor heaps (adapted from upstream PR #7021, Khuraie, 355201c5, GPL-3.0) ----
//
// PR #7021 reports that the title intercepts the console runtime's descriptor-heap allocation through its exported
// __XMemAllocationHooks__ (attribute word 0x00800004: allocator 0x80 in bits 16..23, object type 4 in the low 14 bits;
// size NumDescriptors * increment) and checks that heap slot 9 returns that address. A local offline probe also
// observed this assertion before the adaptation and passed it afterwards (see UPSTREAM.md).
//
// Slot 14 asks the installed allocator for that backing after the real heap exists. Ready hooks: the heap proxy's slot
// 9 returns the title's address, slot 10 still returns the real GPU start, and the range [custom, custom + size) maps to
// the real CPU start. No hooks: the plain heap, as before. A null allocation (E_OUTOFMEMORY), invalid hooks, an increment
// of 0, a range that wraps, or one that overlaps another tracked heap (E_FAIL) all release the real heap and any backing
// and fail the call. The backing is freed by the same pair that allocated it, after the real heap, on the last Release.
//
// CPU handles passed to device slots 17..24 and to console list slots 50..54 are translated by translate_cpu_descriptor_
// range. While no title-backed heap exists every handle passes through unchanged (the previous behaviour). While one
// exists: a handle inside a title range is translated after its device, heap type, increment alignment and the whole
// span are checked. All other handles pass through unchanged, including native heaps created directly on the
// backend. Bounds outside registered custom ranges cannot be classified as custom; the backend validates them.
// An invalid handle is never forwarded: device slots skip the void call, count it and log/trace "refused"; list slots
// mark the list unsupported (Close fails, submission refused). The title's own CPU writes into its backing memory are
// not seen by the real heap; only the API calls above are translated.
inline constexpr std::uint64_t kXmemDescriptorHeapAttributes = 0x00800004ull;

struct DescriptorHeapBacking {
    void* address = nullptr;
    std::uint64_t token = 0;     // opaque to the proxy: identifies the allocating hook registration
    void* release_hook = nullptr;  // opaque to the proxy: the free routine captured with `address`
};
enum class DescriptorBackingStatus { not_hooked, allocated, out_of_memory, invalid_hooks };
struct DescriptorHeapAllocator {
    DescriptorBackingStatus (*allocate)(std::size_t byte_size, std::uint64_t attributes, DescriptorHeapBacking* out);
    void (*release)(const DescriptorHeapBacking& backing, std::uint64_t attributes);
};
// The allocator slot 14 consults (copied). An explicit one (tests) wins; the default (D3D12XboxCreateDevice's bridge to
// the shim registry's hooks) is installed only while no explicit one is set. Null clears the explicit allocator.
void set_descriptor_heap_allocator(const DescriptorHeapAllocator* allocator);
void set_default_descriptor_heap_allocator(const DescriptorHeapAllocator* allocator);

enum class DescriptorHandleTranslation { unchanged, translated, invalid };
// Translates the span of `count` descriptors (0 counts as 1) of heap type `type` starting at `start`, as the rules
// above say. `device` is the device proxy (or its DXGI pointer); *out is `start` unless translated.
DescriptorHandleTranslation translate_cpu_descriptor_range(const void* device, D3D12_CPU_DESCRIPTOR_HANDLE start,
                                                           std::uint64_t count, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE* out);
// Test helpers: live title-backed ranges, live tracked heaps, and device-slot calls refused for an invalid handle.
std::size_t title_descriptor_range_count();
std::size_t tracked_descriptor_heap_count();
std::uint64_t refused_descriptor_handle_calls();

// Number of PC vtable slots the proxy forwards (detected from the interfaces the PC device supports).
unsigned device_proxy_slot_count(const void* device);

// The PC device behind a proxy. Null when `device` is not a proxy.
ID3D12Device* device_proxy_real_device(const void* device);

// Device slot 133 (the console's CreatePlacedResourceX) places a resource at the title's VA `va`. The proxy does not
// create it: the device behind it does, through this handler, which the Vulkan layer registers
// (rt::d3d12::create_buffer_at_host_address). With none registered the slot answers E_NOTIMPL.
//
// The proxy is strict and passes only semantics it knows, unchanged: a BUFFER description whose layout word is 0x1
// (ROW_MAJOR, which the console's row-major word and the PC enum share) and whose flags are none, ALLOW_UNORDERED_ACCESS
// or DENY_SHADER_RESOURCE, with a null clear value. Any other dimension, layout word or flag bit (the console's bits
// above the PC flags included, since their meaning is not established) is E_NOTIMPL, a clear value is E_INVALIDARG,
// and null desc/riid/out are E_POINTER; on every refusal *out is null and the handler is not called. `initial_state` is
// the title's third word, passed through unchanged: the handler decides which values it accepts (the Vulkan layer only
// COMMON, 0; its meaning on the console is not established, retail_boot_chain.md section 3.4). A failing handler must
// leave *out null; the proxy clears it again on failure. Shape from upstream #7218 (tibzejoker,
// a3eae955dcec4fb65864ca6c10b4ca3f2999c2d3, GPL-3.0).
//
// EXPERIMENT, opt-in only (experimental_host_import.h): with PALMWAVE_EXPERIMENTAL_XBOX_HOST_IMPORT exactly "1", the one
// tuple BUFFER, Alignment 0, Width > 0, Height/DepthOrArraySize/MipLevels 1, Format UNKNOWN, SampleDesc {1, 0}, layout
// word 0x1, Flags 0x00020004, state word 0xFFFFFFFF and a null clear value is passed to the handler as a NEW host
// description with Flags ALLOW_UNORDERED_ACCESS (0x4) and the state word COMMON (0); the title's description is not
// written and its padding DWORD at +4 is not read. Each such call is logged with the original and host values and
// recorded on the D3D12 trace with an "experimental ok/unsupported/failed" outcome. That bit 0x20000 may be dropped and
// that 0xFFFFFFFF means "no state" are hypotheses, not a measured console ABI. Every other value keeps the strict rules.
using PlacedAtVaHandler = HRESULT (*)(ID3D12Device* real, void* va, const D3D12_RESOURCE_DESC& desc,
                                      std::uint32_t initial_state, REFIID riid, void** out);
void set_placed_at_va_handler(PlacedAtVaHandler handler);

// ---- Device slot 70: a raw buffer SRV named by console GPU VA ----
//
// The console call is void(this, const D3D12_RESOURCE_DESC* resource, const view* view, D3D12_CPU_DESCRIPTOR_HANDLE
// destination). `view` is 64 bytes: a desktop D3D12_SHADER_RESOURCE_VIEW_DESC (40 bytes) and, at +0x28, the uint64
// console GPU VA of the buffer. No resource object is passed; the VA names it. The PC slot 70 is
// ID3D12Device8::CreatePlacedResource1, which would read the destination as a description, so it is never reached.
// Call shape measured on a local offline Xbox Series executable probe; PR #7066 (tedin7, 8e700080, GPL-3.0) and
// FiveStar (18e225d2, GPL-3.0-or-later) describe the same 0x28 + VA layout. Bytes 0x30..0x3F have no established
// meaning for a buffer view and are not read.
//
// The one supported shape (any other is refused; nothing is masked, clamped or guessed):
//   resource  Dimension BUFFER, Alignment 0, Width > 0 and a multiple of 4 and at most 0xFFFFFFFC, Height 1,
//             DepthOrArraySize 1, MipLevels 1, Format UNKNOWN, SampleDesc {1, 0}, Layout ROW_MAJOR, Flags NONE.
//             The DWORD at +4 is padding and is not read. Flags describe the view, not the resource's creation flags
//             (the measured buffer was created with ALLOW_UNORDERED_ACCESS), so they are not compared with them.
//   view      Format R32_TYPELESS, ViewDimension BUFFER, Shader4ComponentMapping D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
//             Buffer.FirstElement 0, Buffer.NumElements Width / 4, Buffer.StructureByteStride 0, Buffer.Flags RAW
//             exactly. The padding at +0x0C and +0x24 is not read.
//   VA        non-zero, a multiple of D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT (16), VA + Width without overflow, and
//             [VA, VA + Width) wholly inside ONE live buffer of this device that allows shader-resource access.
//   dest      a CBV_SRV_UAV descriptor slot of a heap this device proxy created (slot 14), on a descriptor boundary:
//             inside a title-backed range (translated to the real heap) or inside the real range of a plain heap.
//             Any other handle, including a native heap the proxy never saw, is refused: there is no registry entry
//             to validate it against.
// On success the PC CreateShaderResourceView (slot 18) is called with the live buffer and the same byte range as a
// raw view: FirstElement (VA - buffer start) / 4, NumElements Width / 4. Nothing is clamped to the buffer.
// On every refusal nothing is forwarded, the destination descriptor is untouched, the caller's descriptions are not
// written, the refusal is counted, logged and traced ("unsupported" for a shape this layer does not implement,
// "refused" for an invalid argument), and no reference is left behind. The method is void, so the title is not told.
//
// The buffer resolver is per device proxy. It finds the buffer that holds the VA and returns it with one reference
// added for the caller, taken safely against a concurrent last Release (rt::d3d12::set_device_outer installs the
// Vulkan layer's). Without one, slot 70 refuses every call.
struct BufferAtVa {
    ID3D12Resource* resource = nullptr;  // one reference, the caller's to release
    std::uint64_t start = 0;             // the buffer's first console VA (what its GetGPUVirtualAddress returns)
    std::uint64_t size = 0;              // its width in bytes
};
enum class BufferAtVaStatus {
    found,                // `out` is filled: [va, va + bytes) lies in [start, start + size)
    not_found,            // no live buffer of this device holds all of [va, va + bytes)
    not_shader_readable,  // the buffer holding it denies shader-resource access (or is not a plain buffer)
    failed,               // the resolver could not answer
};
// `real` is the device behind the proxy. On anything but `found`, out->resource must be null.
using BufferAtVaResolver = BufferAtVaStatus (*)(ID3D12Device* real, std::uint64_t va, std::uint64_t bytes,
                                                BufferAtVa* out);
// `device` is the proxy or its DXGI pointer. False, with a line on stderr, when it is neither: nothing is set then.
// Null removes the resolver.
bool set_device_buffer_at_va_resolver(void* device, BufferAtVaResolver resolver);

// What the last slot-70 call did (any device, any thread; for tests).
enum class SrvAtVaOutcome {
    none,                // no call yet (or reset)
    created,             // a raw view was written through the PC slot 18
    unreadable,          // a description pointer was null or its bytes are not all committed and readable
    unsupported_shape,   // the descriptions are not the supported raw-buffer shape
    invalid_range,       // a zero, misaligned or overflowing VA / width
    invalid_destination, // not a CBV_SRV_UAV descriptor slot of a heap this device proxy created
    no_resolver,         // no buffer resolver installed on this proxy
    unknown_va,          // no live buffer of this device holds the whole range
    not_shader_readable, // the buffer denies shader-resource access
    resolver_failed,     // the resolver failed or answered a range that does not hold the request
};
SrvAtVaOutcome last_srv_at_va_outcome();
std::uint64_t srv_at_va_created_calls();
std::uint64_t srv_at_va_refused_calls();
void reset_srv_at_va_stats();

// True for slots the proxy handles itself rather than forwarding verbatim: IUnknown (0-2), 8 and 75 (queue
// proxies), 9 (allocator proxies), 14, 17-24 (CPU descriptor handles translated), 26 (the console's struct-return
// form), 31/34/35/45/46/50 (proxies unwrapped),
// 32 and 36 (fence proxies), 48/73 and 78/79/80 (the Xbox slots above, at their configured indices), 62, 67, 70, 76
// and 133. A configured route moved onto slot 70 takes precedence over the built-in slot-70 view.
// Slot 10 is intentionally false: the measured 9/10 handle conversion is on the heap proxy, and device slot 9 is
// CreateCommandAllocator, which the proxy handles for the allocator proxy.
bool device_slot_overridden(unsigned slot);

// Queue and fence proxies created by device slots 8/75 and 36. The real_* accessors return the wrapped object
// (no reference added), or null when the pointer is not such a proxy.
bool is_queue_proxy(const void* queue);
bool is_fence_proxy(const void* fence);
ID3D12CommandQueue* queue_proxy_real_queue(const void* queue);
ID3D12Fence* fence_proxy_real_fence(const void* fence);
// Public slots forwarded (19 for a queue; 11 or 12 for a fence), and the size of the proxy's vtable.
unsigned queue_proxy_slot_count(const void* queue);
unsigned fence_proxy_slot_count(const void* fence);
unsigned queue_proxy_table_size();
unsigned fence_proxy_table_size();
// Live references on a queue, fence or allocator proxy (0 for anything else). Test helper.
unsigned long command_object_proxy_ref_count(const void* object);
// Per-slot call counters and first-call flags, shared by all queue proxies and all fence proxies.
std::uint64_t queue_slot_hits(unsigned slot);
bool queue_slot_first_call_seen(unsigned slot);
std::uint64_t fence_slot_hits(unsigned slot);
bool fence_slot_first_call_seen(unsigned slot);
// Vtable reads past a queue or fence proxy table that faulted in its guard region (counted by the fault logger).
std::uint64_t queue_table_overruns();
std::uint64_t fence_table_overruns();
// True when the queue, fence and allocator tables sit in front of their guard regions (false only if a reservation
// or the fault logger failed).
bool command_object_tables_guarded();

// Command allocator proxies (device slot 9): the public chain (9 slots: the base chain and Reset) is forwarded, a
// slot past it up to the end of the table (64 slots) is logged and refused with E_NOTIMPL, and a read past the
// table faults in a guard region, as for queues and fences. The console allocator's extra methods are not
// measured and not guessed. Slot 9 wraps only an allocator that answers kPalmwaveCommandAllocatorTag, Palmwave's
// own Vulkan-layer allocator: its command lists resolve the proxy (CreateCommandList, Reset). A PC runtime
// allocator goes back unchanged, because the PC runtime's lists would take a proxy for their own allocator; a
// command-list proxy that unwraps at Reset is what lets the host backend wrap allocators too.
// {7d0e6b51-34a2-4f0c-9a61-0c5b2e8f4a10}: the Vulkan layer's CommandAllocator answers it with its implementation.
inline constexpr GUID kPalmwaveCommandAllocatorTag{
    0x7d0e6b51, 0x34a2, 0x4f0c, {0x9a, 0x61, 0x0c, 0x5b, 0x2e, 0x8f, 0x4a, 0x10}};
bool is_allocator_proxy(const void* allocator);
// The wrapped allocator (no reference added), or null when `allocator` is not an allocator proxy.
ID3D12CommandAllocator* allocator_proxy_real_allocator(const void* allocator);
unsigned allocator_proxy_slot_count(const void* allocator);
unsigned allocator_proxy_table_size();
std::uint64_t allocator_slot_hits(unsigned slot);
bool allocator_slot_first_call_seen(unsigned slot);
std::uint64_t allocator_table_overruns();
// Heap slots 0-2, 7 (GetDevice -> device proxy) and 9/10 (handle conversion).
bool heap_slot_overridden(unsigned slot);

// Console command-list proxies (device slot 73). list_proxy_real_list returns the wrapped list (no reference added),
// or null when `list` is not a list proxy. list_proxy_slot_count is the PC slot count of the real list (82 for
// ID3D12GraphicsCommandList8); list_proxy_unsupported_slot the first console slot recorded since the last Reset that
// was not executed (0 when none). list_slot_hits counts calls per console slot, under the counter rule below.
bool is_list_proxy(const void* list);
ID3D12GraphicsCommandList* list_proxy_real_list(const void* list);
unsigned list_proxy_table_size();
unsigned list_proxy_slot_count(const void* list);
unsigned long list_proxy_ref_count(const void* list);
unsigned list_proxy_unsupported_slot(const void* list);
std::uint64_t list_slot_hits(unsigned slot);

// Number of live references on a device proxy. Test helper for the heap's GetDevice/lifetime checks.
unsigned long device_proxy_ref_count(const void* device);

// Per-slot call counters and first-call flags for the device and heap proxies. Slot 0 also counts the
// QueryInterface call made while a proxy is created, so tests reset the counters after creation.
// reset_device_proxy_stats also resets the queue and fence counters.
// The hit counters (device, heap, queue, fence and allocator) are for tests, and they are off at the start.
// A test must read a counter (any *_slot_hits function) or call reset_device_proxy_stats BEFORE the calls that it
// counts. Calls made before that are not counted (the counter stays 0); they set only the first-call flags.
std::uint64_t device_slot_hits(unsigned slot);
bool device_slot_first_call_seen(unsigned slot);
std::uint64_t heap_slot_hits(unsigned slot);
bool heap_slot_first_call_seen(unsigned slot);
void reset_device_proxy_stats();

}  // namespace rt::d3d12_xs
