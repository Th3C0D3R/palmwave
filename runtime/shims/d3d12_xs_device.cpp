// Xbox-ABI proxy around a PC ID3D12Device. See d3d12_xs_device.h for the measured slots and the limits.
//
// How forwarding works: the proxy object's first word is a vtable built once from one thunk per slot. A thunk
// receives the proxy in RCX, replaces it with the PC device pointer from offset 8, and tail-calls the PC method at
// the same index through a 15-argument integer prototype. Every PC D3D12 device method takes integer/pointer
// arguments only, so the x64 registers and stack words line up exactly; methods that return a struct keep their
// hidden return pointer in RDX, which is passed through as the first argument unchanged. The measured exceptions
// are IUnknown (0-2), slot 14 (which wraps the heap), 26 (the title passes the return buffer in RCX and the device in
// RDX), 62, 70 (a raw buffer SRV named by GPU VA, never the PC CreatePlacedResource1), 76 and 133
// (CreatePlacedResourceX, handed to the device's registered handler), plus the heap proxy's 9/10 conversion from the PC hidden-pointer form to the Xbox RAX form. Slots
// 8/75 (command queues), 36 (fences) and 32 (a shared fence) wrap what they create in queue and fence proxies built
// the same way, and slots 31/34/35/45/46/50 hand the real objects behind heap, queue and fence proxies to the real
// device (see the command queue and fence section). Slots 17..24 translate title-backed CPU descriptor handles
// (adapted from upstream PR #7021; see "Descriptor ranges and title-backed heaps").
//
// A thunk declares 14 argument words, but the title passes only as many as the method takes, so the caller's frame
// may end right after the four register homes. The compiler owns a function's incoming stack-argument slots and may
// write them, a tail call included. Every function a thunk hands its words to therefore takes them in the same
// positions (slot 8: desc, riid, out as a1-a3; slot 50: a1-a5; and so on). A tail call then writes back only the
// values it read from those slots. A helper that moved a word to another stack position would overwrite the title's
// frame. xs_proxy_mock_test's mock hit this in an optimized build, and its thunks now pass their words by pointer.
//
// The public HLE Freminet/ScarlettX reports five more Xbox device slots (a third-party observation, not a Microsoft
// ABI); the generic thunk routes them before it forwards anything: 48 CreateCommandQueueX, 73 CreateCommandListX,
// 78 SetFrameIntervalX, 79 ScheduleFrameEventX and 80 WaitFrameEventX. The slots can be moved with PALMWAVE_XS_*_SLOT
// for a trace that disagrees. The frame-event calls answer from a per-device token counter: there is no vblank source
// behind the device before a presenter exists, so WaitFrameEventX returns at once with a fresh non-zero token.
#include "d3d12_xs_device.h"
#include "d3d12_trace.h"
#include "experimental_host_import.h"
#include "../src/shader_library.h"  // palmwave::fspx_payload_from_parts for slot 67

#include <windows.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <memory>
#include <utility>
#include <vector>

#include "d3d12_xs_command_list_iid.h"

namespace rt::d3d12_xs {
namespace {

constexpr unsigned kMaxSlots = 192;  // console slot 133 and a refused tail, not a PC interface claim
constexpr unsigned kMaxConfiguredSlot = 127;  // keep the existing environment-remap contract
static_assert(kMaxSlots > 133, "the proxy table must cover console placement slot 133");
static_assert(sizeof(void*) == 8, "the Xbox-ABI proxy is x64-only");
static_assert(sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) == 8,
              "the Xbox handle ABI is an 8-byte value in RAX; the heap thunk converts the PC hidden-pointer form");
static_assert(sizeof(D3D12_RESOURCE_DESC) == 56, "the measured Xbox description prefix is 56 bytes");

struct DeviceProxy {
    void** vtable;          // offset 0: the caller's ID3D12Device* points here
    ID3D12Device* real;     // offset 8: the PC device every thunk forwards to
    void** real_vtable;
    unsigned slot_count;    // number of valid entries in real_vtable
    std::atomic<unsigned long> refs;
    // The device's IDXGIDevice1 (see "IDXGIDevice1 on the device" below): a title's IDXGIDevice1* points here.
    void** dxgi_vtable;
    DxgiAdapterProvider adapter_provider;
    // Slot 70: finds the live buffer at a console VA on `real` (set_device_buffer_at_va_resolver); null refuses slot 70.
    std::atomic<BufferAtVaResolver> buffer_resolver{nullptr};
    std::atomic<int> gpu_thread_priority;
    std::atomic<unsigned> max_frame_latency;
    // The first-frame subset of SetFrameIntervalX/ScheduleFrameEventX/WaitFrameEventX. Tokens were never measured;
    // they start at 1 and only have to be fresh and non-zero for the boot sequence. Waiting returns at once: there is
    // no vblank source until a presenter exists, and a bounded zero wait cannot stall the first frame.
    std::atomic<std::uint32_t> frame_interval_us{0};
    std::atomic<std::uint32_t> frame_latency{1};
    std::atomic<bool> frame_event_scheduled{false};
    std::atomic<std::uint64_t> frame_next_token{1};
    // The upload arena console list slot 148 (AllocateUploadMemory) carves from; null until the first call. Opaque:
    // UploadArena is defined with the list proxy. Lists record on several threads, so the first call installs it by
    // compare-exchange.
    std::atomic<void*> upload_state{nullptr};
};

// Frees the upload arena in proxy_release; defined with the list proxy, where UploadArena is complete.
void destroy_upload_state(void* upload_state);

constexpr unsigned kHeapSlots = 11;  // IUnknown + ID3D12Object + DeviceChild + Pageable + GetDesc + 2 handles

struct HeapProxy {
    void** vtable;          // offset 0: the caller's ID3D12DescriptorHeap* points here
    ID3D12DescriptorHeap* real;
    DeviceProxy* device;    // keeps the Xbox-ABI device alive; GetDevice returns it
    void** real_vtable;
    std::atomic<unsigned long> refs;
    // Title backing (upstream PR #7021, adapted). custom_cpu_start is 0 for a plain heap. Otherwise slot 9 returns it,
    // and the descriptor-range table maps [custom_cpu_start, + byte_size) to the real heap's CPU start. `backing` and
    // `release_backing` are the allocation and the routine that frees it, both captured at creation and owned by the
    // proxy from a successful creation on: the last Release frees through that same pair, after the real heap.
    SIZE_T custom_cpu_start;
    DescriptorHeapBacking backing;
    void (*release_backing)(const DescriptorHeapBacking&, std::uint64_t);
};

std::atomic<std::uint64_t> g_hits[kMaxSlots];
std::atomic<bool> g_first[kMaxSlots];
std::atomic<bool> g_beyond_warned[kMaxSlots];
std::atomic<std::uint64_t> g_heap_hits[kHeapSlots];
std::atomic<bool> g_heap_first[kHeapSlots];
// The hit counters are for tests. They count from the first call of a *_slot_hits reader or of
// reset_device_proxy_stats, so the calls of a title, which reads no counter, write no shared counter.
std::atomic<bool> g_count_hits{false};

// For the readers and the reset. The flag is written one time only.
void start_hit_counts() {
    if (!g_count_hits.load(std::memory_order_relaxed)) g_count_hits.store(true, std::memory_order_relaxed);
}

void count_hit(std::atomic<std::uint64_t>& hits) {
    if (g_count_hits.load(std::memory_order_relaxed)) hits.fetch_add(1, std::memory_order_relaxed);
}

// True for the first call of a slot only. The flag is read first: a compare-exchange takes the cache line for
// writing also when it fails, and the flags of all slots share one or two lines.
bool first_call(std::atomic<bool>& seen) {
    if (seen.load(std::memory_order_relaxed)) return false;
    bool expected = false;
    return seen.compare_exchange_strong(expected, true, std::memory_order_relaxed);
}

bool trace_enabled() {
    static const bool enabled = [] {
        char value[8] = {};
        const DWORD length = GetEnvironmentVariableA("XS_TRACE", value, sizeof(value));
        return length > 0 && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

const char* slot_name(unsigned slot) {
    switch (slot) {
        case 0: return "QueryInterface";
        case 1: return "AddRef";
        case 2: return "Release";
        case 8: return "CreateCommandQueue -> queue proxy";
        case 9: return "CreateCommandAllocator (PC slot 9)";
        case 10: return "CreateGraphicsPipelineState (PC slot 10)";
        case 11: return "CreateComputePipelineState (PC slot 11)";
        case 14: return "CreateDescriptorHeap";
        case 16: return "CreateRootSignature";
        case 17: return "CreateConstantBufferView (CPU handle translated)";
        case 18: return "CreateShaderResourceView (CPU handle translated)";
        case 19: return "CreateUnorderedAccessView (CPU handle translated)";
        case 20: return "CreateRenderTargetView (CPU handle translated)";
        case 21: return "CreateDepthStencilView (CPU handle translated)";
        case 22: return "CreateSampler (CPU handle translated)";
        case 23: return "CopyDescriptors (CPU handles translated)";
        case 24: return "CopyDescriptorsSimple (CPU handles translated)";
        case 25: return "GetResourceAllocationInfo";
        case 26: return "GetCustomHeapProperties";
        case 27: return "CreateCommittedResource (PC slot 27)";
        case 31: return "CreateSharedHandle (proxies unwrapped)";
        case 32: return "OpenSharedHandle (a fence comes back wrapped)";
        case 34: return "MakeResident (proxies unwrapped)";
        case 35: return "Evict (proxies unwrapped)";
        case 36: return "CreateFence -> fence proxy";
        case 43: return "GetAdapterLuid";
        case 45: return "SetEventOnMultipleFenceCompletion (fence proxies unwrapped)";
        case 46: return "SetResidencyPriority (proxies unwrapped)";
        case 48: return "Xbox CreateCommandQueueX -> queue proxy";
        case 50: return "EnqueueMakeResident (proxies unwrapped)";
        case 62: return "Xbox CreateComputePipelineState";
        case 67: return "Xbox DeserializeComputePipelineStateX";
        case 70: return "Xbox CreateShaderResourceView at a GPU VA (device slot 70)";
        case 73: return "Xbox CreateCommandListX -> console list proxy";
        case 75: return "CreateCommandQueue1 -> queue proxy";
        case 76: return "Xbox resource creation";
        case 78: return "Xbox SetFrameIntervalX";
        case 79: return "Xbox ScheduleFrameEventX";
        case 80: return "Xbox WaitFrameEventX";
        case 133: return "Xbox CreatePlacedResourceX (a buffer on the title's own pages)";
        default: return "forwarded";
    }
}

void note_slot(unsigned slot) {
    if (slot >= kMaxSlots) return;
    count_hit(g_hits[slot]);
    const bool first = first_call(g_first[slot]);
    if (first) {
        std::fprintf(stderr, "[rt] d3d12_xs: device slot %u (%s) first call\n", slot, slot_name(slot));
    } else if (slot >= 3 && trace_enabled()) {
        std::fprintf(stderr, "[rt] d3d12_xs: device slot %u (%s) call\n", slot, slot_name(slot));
    }
}

// Xbox device slots (public HLE Freminet/ScarlettX, an observation, not Microsoft documentation). The defaults are
// the HLE's; an environment variable that names another slot in 3..127 replaces one (a whole-table move for a trace
// that disagrees). A value outside that range is logged once and ignored. The table is read once, on the first
// generic slot call, so a test that wants other slots sets the variables before it creates a device.
struct XboxDeviceSlotRoutes {
    unsigned frame_interval = 78;  // SetFrameIntervalX(output, interval_us, latency, flags)
    unsigned schedule_event = 79;  // ScheduleFrameEventX(event, offset_us, reserved, flags)
    unsigned wait_event = 80;      // WaitFrameEventX(event, timeout_ms, reserved, flags, &token)
    unsigned queue_x = 48;         // CreateCommandQueueX(desc, riid, out)
    unsigned list_x = 73;          // CreateCommandListX(desc, allocator, initial_state, riid, out)
};

unsigned env_device_slot(const char* name, unsigned fallback) {
    char value[32] = {};
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    if (length == 0) return fallback;
    if (length >= sizeof(value)) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s is too long for a device slot; the default is kept\n", name);
        return fallback;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long slot = std::strtoul(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' || slot < 3 || slot > kMaxConfiguredSlot) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s=%s is not a device slot in 3..127; the default is kept\n", name,
                     value);
        return fallback;
    }
    return static_cast<unsigned>(slot);
}

const XboxDeviceSlotRoutes& xbox_device_slot_routes() {
    static const XboxDeviceSlotRoutes routes = [] {
        XboxDeviceSlotRoutes r;
        const unsigned frame_interval = env_device_slot("PALMWAVE_XS_FRAME_INTERVAL_SLOT", r.frame_interval);
        const unsigned schedule_event = env_device_slot("PALMWAVE_XS_SCHEDULE_EVENT_SLOT", r.schedule_event);
        const unsigned wait_event = env_device_slot("PALMWAVE_XS_WAIT_EVENT_SLOT", r.wait_event);
        const unsigned queue_x = env_device_slot("PALMWAVE_XS_QUEUE_X_SLOT", r.queue_x);
        const unsigned list_x = env_device_slot("PALMWAVE_XS_LIST_X_SLOT", r.list_x);
        const bool moved = frame_interval != r.frame_interval || schedule_event != r.schedule_event ||
                           wait_event != r.wait_event || queue_x != r.queue_x || list_x != r.list_x;
        r.frame_interval = frame_interval;
        r.schedule_event = schedule_event;
        r.wait_event = wait_event;
        r.queue_x = queue_x;
        r.list_x = list_x;
        if (moved)
            std::fprintf(stderr,
                         "[rt] d3d12_xs: Xbox device slots: SetFrameIntervalX=%u ScheduleFrameEventX=%u "
                         "WaitFrameEventX=%u CreateCommandQueueX=%u CreateCommandListX=%u\n",
                         r.frame_interval, r.schedule_event, r.wait_event, r.queue_x, r.list_x);
        return r;
    }();
    return routes;
}

// The public HLE call shapes. SetFrameIntervalX's output and flags are unused while there is no output or pacing;
// ScheduleFrameEventX's reserved event pointer is a3 in the console call and a4 is its flags. The logged words are the
// caller's, so a trace names the slot even when an override moved it.
void log_xbox_route_once(unsigned slot, const char* name, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                         std::uint64_t a4, std::uint64_t a5) {
    static std::atomic<bool> logged[kMaxSlots];
    if (slot >= kMaxSlots) return;
    bool expected = false;
    if (!logged[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: device slot %u routed to %s (args 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx)\n", slot,
                 name, static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                 static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4),
                 static_cast<unsigned long long>(a5));
}

const char* heap_slot_name(unsigned slot) {
    switch (slot) {
        case 0: return "QueryInterface";
        case 1: return "AddRef";
        case 2: return "Release";
        case 7: return "GetDevice";
        case 8: return "GetDesc";
        case 9: return "GetCPUDescriptorHandleForHeapStart";
        case 10: return "GetGPUDescriptorHandleForHeapStart";
        default: return "forwarded";
    }
}

void note_heap_slot(unsigned slot) {
    if (slot >= kHeapSlots) return;
    count_hit(g_heap_hits[slot]);
    const bool first = first_call(g_heap_first[slot]);
    if (first) {
        std::fprintf(stderr, "[rt] d3d12_xs: heap slot %u (%s) first call\n", slot, heap_slot_name(slot));
    } else if (slot >= 3 && trace_enabled()) {
        std::fprintf(stderr, "[rt] d3d12_xs: heap slot %u (%s) call\n", slot, heap_slot_name(slot));
    }
}

std::uint64_t hr_to_u64(HRESULT hr) {
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(hr));
}

unsigned long proxy_add_ref(DeviceProxy* self) {
    note_slot(1);
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

unsigned long proxy_release(DeviceProxy* self) {
    note_slot(2);
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) {
        // Every list holds a device reference, so no list can still translate into the arena here.
        destroy_upload_state(self->upload_state.load(std::memory_order_acquire));
        ID3D12Device* real = self->real;
        delete self;
        if (real) real->Release();
    }
    return refs;
}

bool is_device_iid(const IID& iid) {
    if (IsEqualIID(iid, __uuidof(ID3D12Device))) return true;
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device14))) return true;
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device13))) return true;
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device12))) return true;
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device11))) return true;
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device10))) return true;
#endif
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device9))) return true;
#endif
    if (IsEqualIID(iid, __uuidof(ID3D12Device8))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device7))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device6))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device5))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device4))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device3))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device2))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device1))) return true;
    return false;
}

HRESULT proxy_query_interface(DeviceProxy* self, const IID* riid, void** out) {
    note_slot(0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (IsEqualIID(*riid, __uuidof(IUnknown)) || IsEqualIID(*riid, __uuidof(ID3D12Object))) {
        *out = self;
        proxy_add_ref(self);
        return S_OK;
    }
    if (IsEqualIID(*riid, __uuidof(IDXGIObject)) || IsEqualIID(*riid, __uuidof(IDXGIDevice)) ||
        IsEqualIID(*riid, __uuidof(IDXGIDevice1))) {
        *out = &self->dxgi_vtable;
        proxy_add_ref(self);
        return S_OK;
    }
    if (!is_device_iid(*riid)) return E_NOINTERFACE;

    // Only claim an interface the PC device really implements; the proxy vtable covers its slots.
    void* supported = nullptr;
    HRESULT hr = self->real->QueryInterface(*riid, &supported);
    if (FAILED(hr)) return hr;
    if (!supported) return E_NOINTERFACE;
    static_cast<IUnknown*>(supported)->Release();
    *out = self;
    proxy_add_ref(self);
    return S_OK;
}

// ---- IDXGIDevice1 on the device ---------------------------------------------------------------------------------
//
// A GDKX title finds its output through the device: QueryInterface for IDXGIDevice1, GetAdapter, EnumOutputs(0),
// and hands that output to SetFrameIntervalX (docs/d3d12_vulkan.md section 4.5, from the public samples). A PC
// ID3D12Device has no IDXGIDevice, so the proxy answers it itself: a second vtable pointer inside the proxy object,
// with the proxy's reference count and QueryInterface, so the two pointers are one COM object. Only IDXGIDevice and
// IDXGIDevice1 are answered (what the samples ask for). The 14 slots are the public dxgi.h order; that the console
// keeps it is the same assumption as for the queue and the fence.
//
// GetAdapter and GetParent hand out the adapter of the device behind the proxy. The provider says how: the default
// asks the host's DXGI for the adapter with the device's LUID (the host backend); set_device_adapter_provider puts
// the Vulkan layer's own adapter there, so that backend never loads the host's dxgi.dll for it.
constexpr unsigned kDxgiDeviceSlots = 14;  // IUnknown 3, IDXGIObject 4, IDXGIDevice 5, IDXGIDevice1 2

DeviceProxy* dxgi_owner(void* self) {
    return reinterpret_cast<DeviceProxy*>(static_cast<char*>(self) - offsetof(DeviceProxy, dxgi_vtable));
}

HRESULT host_adapter(ID3D12Device* real, REFIID riid, void** out) {
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    static const CreateFactoryFn create_factory = [] {
        const HMODULE dxgi = LoadLibraryExA("dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return dxgi ? reinterpret_cast<CreateFactoryFn>(
                          reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
                    : nullptr;
    }();
    if (!create_factory) return DXGI_ERROR_NOT_FOUND;
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = create_factory(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) return FAILED(hr) ? hr : DXGI_ERROR_NOT_FOUND;
    hr = factory->EnumAdapterByLuid(real->GetAdapterLuid(), riid, out);
    factory->Release();
    return hr;
}

HRESULT dxgi_adapter(DeviceProxy* self, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    const DxgiAdapterProvider provider = self->adapter_provider ? self->adapter_provider : &host_adapter;
    const HRESULT hr = provider(self->real, riid, out);
    if (FAILED(hr)) *out = nullptr;
    return hr;
}

void note_dxgi(const char* method) {
    d3d12_trace::record(method);
}

HRESULT WINAPI dxgi_query_interface(void* self, const IID* riid, void** out) {
    return proxy_query_interface(dxgi_owner(self), riid, out);
}
ULONG WINAPI dxgi_add_ref(void* self) { return proxy_add_ref(dxgi_owner(self)); }
ULONG WINAPI dxgi_release(void* self) { return proxy_release(dxgi_owner(self)); }

// IDXGIObject's private data is the device's: ID3D12Object has the same three methods with the same arguments.
HRESULT WINAPI dxgi_set_private_data(void* self, const GUID* name, UINT size, const void* data) {
    if (!name) return E_INVALIDARG;
    return dxgi_owner(self)->real->SetPrivateData(*name, size, data);
}
HRESULT WINAPI dxgi_set_private_data_interface(void* self, const GUID* name, const IUnknown* object) {
    if (!name) return E_INVALIDARG;
    return dxgi_owner(self)->real->SetPrivateDataInterface(*name, object);
}
HRESULT WINAPI dxgi_get_private_data(void* self, const GUID* name, UINT* size, void* data) {
    if (!name) return E_INVALIDARG;
    return dxgi_owner(self)->real->GetPrivateData(*name, size, data);
}
HRESULT WINAPI dxgi_get_parent(void* self, const IID* riid, void** out) {
    note_dxgi("XboxIDXGIDevice::GetParent");
    if (!riid) return E_POINTER;
    return dxgi_adapter(dxgi_owner(self), *riid, out);
}
HRESULT WINAPI dxgi_get_adapter(void* self, IDXGIAdapter** out) {
    note_dxgi("XboxIDXGIDevice::GetAdapter");
    return dxgi_adapter(dxgi_owner(self), __uuidof(IDXGIAdapter), reinterpret_cast<void**>(out));
}
// CreateSurface and QueryResourceResidency are D3D10/11-era methods no D3D12 title has a use for; what the console
// answers is not measured, so they are refused and say so once.
HRESULT WINAPI dxgi_create_surface(void*, const DXGI_SURFACE_DESC*, UINT, DXGI_USAGE, const DXGI_SHARED_RESOURCE*,
                                   IDXGISurface** out) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: IDXGIDevice::CreateSurface is not implemented; returning E_NOTIMPL\n");
    if (out) *out = nullptr;
    return E_NOTIMPL;
}
HRESULT WINAPI dxgi_query_resource_residency(void*, IUnknown* const*, DXGI_RESIDENCY*, UINT) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: IDXGIDevice::QueryResourceResidency is not implemented; returning E_NOTIMPL\n");
    return E_NOTIMPL;
}
// The priority and the frame latency are kept and read back; nothing acts on them (the Xbox presenter's latency is
// SetFrameIntervalX's). The ranges are the documented ones: -7..7, and 1..16 with 0 for the default of 3.
HRESULT WINAPI dxgi_set_gpu_thread_priority(void* self, INT priority) {
    if (priority < -7 || priority > 7) return E_INVALIDARG;
    dxgi_owner(self)->gpu_thread_priority.store(priority, std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI dxgi_get_gpu_thread_priority(void* self, INT* priority) {
    if (!priority) return E_POINTER;
    *priority = dxgi_owner(self)->gpu_thread_priority.load(std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI dxgi_set_maximum_frame_latency(void* self, UINT latency) {
    if (latency > 16) return DXGI_ERROR_INVALID_CALL;
    dxgi_owner(self)->max_frame_latency.store(latency ? latency : 3, std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI dxgi_get_maximum_frame_latency(void* self, UINT* latency) {
    if (!latency) return E_POINTER;
    *latency = dxgi_owner(self)->max_frame_latency.load(std::memory_order_relaxed);
    return S_OK;
}

void** get_dxgi_vtable() {
    static void* table[kDxgiDeviceSlots] = {
        reinterpret_cast<void*>(&dxgi_query_interface),          // 0
        reinterpret_cast<void*>(&dxgi_add_ref),                  // 1
        reinterpret_cast<void*>(&dxgi_release),                  // 2
        reinterpret_cast<void*>(&dxgi_set_private_data),         // 3  IDXGIObject
        reinterpret_cast<void*>(&dxgi_set_private_data_interface),
        reinterpret_cast<void*>(&dxgi_get_private_data),
        reinterpret_cast<void*>(&dxgi_get_parent),
        reinterpret_cast<void*>(&dxgi_get_adapter),              // 7  IDXGIDevice
        reinterpret_cast<void*>(&dxgi_create_surface),
        reinterpret_cast<void*>(&dxgi_query_resource_residency),
        reinterpret_cast<void*>(&dxgi_set_gpu_thread_priority),
        reinterpret_cast<void*>(&dxgi_get_gpu_thread_priority),
        reinterpret_cast<void*>(&dxgi_set_maximum_frame_latency),  // 12 IDXGIDevice1
        reinterpret_cast<void*>(&dxgi_get_maximum_frame_latency),
    };
    return table;
}

// ---- Descriptor ranges and title-backed heaps (adapted from upstream PR #7021, Khuraie, GPL-3.0) ----
//
// Every heap slot 14 creates gets one entry: its real CPU range and, for a title-backed heap, the title's range of the
// same size. Entries are added before the heap proxy is handed out and removed on its last Release before the real heap
// is released, so a handle translated under the lock always names a live real heap (using a heap's handles while it is
// being destroyed is the caller's error, as on the PC). The table and its mutex are immortal: a late COM Release during
// process teardown may still unregister. g_title_range_count lets every call skip the lock while no title-backed heap
// exists, which keeps the plain path exactly as before. The title's allocator and free routines are never called under
// this lock or the allocator lock.
//
// Aliasing is refused at registration: a title range may overlap no other range, and a real range may overlap no title
// range, so a handle has at most one meaning. Ranges never wrap (checked before registration), so the overlap test
// below cannot overflow.
struct DescriptorRange {
    const DeviceProxy* device;
    const HeapProxy* heap;
    SIZE_T real_start;    // 0: not tracked (unknown start); never matched
    SIZE_T custom_start;  // 0: no title backing
    SIZE_T byte_size;     // NumDescriptors * increment, the size of both ranges
    UINT increment;
    D3D12_DESCRIPTOR_HEAP_TYPE type;
};

std::mutex& descriptor_range_mutex() {
    static auto* m = new std::mutex;
    return *m;
}

std::vector<DescriptorRange>& descriptor_ranges() {
    static auto* v = new std::vector<DescriptorRange>;
    return *v;
}

std::atomic<std::size_t> g_title_range_count{0};
std::atomic<std::uint64_t> g_refused_handle_calls{0};

bool ranges_overlap(SIZE_T a, SIZE_T a_size, SIZE_T b, SIZE_T b_size) {
    return a_size && b_size && a < b + b_size && b < a + a_size;
}

enum class RangeRegistration { ok, out_of_memory, aliased };

RangeRegistration register_descriptor_range(const DescriptorRange& range) {
    std::lock_guard lock(descriptor_range_mutex());
    auto& ranges = descriptor_ranges();
    if (range.custom_start && range.real_start &&
        ranges_overlap(range.custom_start, range.byte_size, range.real_start, range.byte_size))
        return RangeRegistration::aliased;
    for (const DescriptorRange& other : ranges) {
        if (range.custom_start &&
            (ranges_overlap(range.custom_start, range.byte_size, other.custom_start, other.custom_start ? other.byte_size : 0) ||
             ranges_overlap(range.custom_start, range.byte_size, other.real_start, other.real_start ? other.byte_size : 0)))
            return RangeRegistration::aliased;
        if (range.real_start && other.custom_start &&
            ranges_overlap(range.real_start, range.byte_size, other.custom_start, other.byte_size))
            return RangeRegistration::aliased;
    }
    try {
        ranges.push_back(range);
    } catch (const std::bad_alloc&) {
        return RangeRegistration::out_of_memory;
    }
    if (range.custom_start) g_title_range_count.fetch_add(1, std::memory_order_release);
    return RangeRegistration::ok;
}

void unregister_descriptor_range(const HeapProxy* heap) {
    std::lock_guard lock(descriptor_range_mutex());
    auto& ranges = descriptor_ranges();
    for (auto it = ranges.begin(); it != ranges.end();) {
        if (it->heap != heap) {
            ++it;
            continue;
        }
        if (it->custom_start) g_title_range_count.fetch_sub(1, std::memory_order_release);
        it = ranges.erase(it);
    }
}

// Under descriptor_range_mutex. The rules are in d3d12_xs_device.h ("Title-backed descriptor heaps").
DescriptorHandleTranslation translate_locked(const DeviceProxy* device, SIZE_T ptr, std::uint64_t count,
                                             D3D12_DESCRIPTOR_HEAP_TYPE type, SIZE_T& out) {
    out = ptr;
    if (!ptr) return DescriptorHandleTranslation::unchanged;  // a null handle: the backend answers as on the PC
    const std::uint64_t need = count ? count : 1;
    for (const DescriptorRange& r : descriptor_ranges()) {
        if (r.custom_start && ptr >= r.custom_start && ptr - r.custom_start < r.byte_size) {
            const SIZE_T offset = ptr - r.custom_start;
            if (r.device != device || r.type != type || offset % r.increment != 0 ||
                need > (r.byte_size - offset) / r.increment)
                return DescriptorHandleTranslation::invalid;
            out = r.real_start + offset;
            return DescriptorHandleTranslation::translated;
        }
    }
    // An untracked handle can belong to a native heap created directly on the backend.
    return DescriptorHandleTranslation::unchanged;
}

DescriptorHandleTranslation translate_handle(const DeviceProxy* device, SIZE_T ptr, std::uint64_t count,
                                             D3D12_DESCRIPTOR_HEAP_TYPE type, SIZE_T& out) {
    out = ptr;
    if (g_title_range_count.load(std::memory_order_acquire) == 0) return DescriptorHandleTranslation::unchanged;
    std::lock_guard lock(descriptor_range_mutex());
    return translate_locked(device, ptr, count, type, out);
}

// Device slot 70's destination: one descriptor of `type` in a heap slot 14 registered for `device`, either inside its
// title range (translated to the real heap) or inside its real range (a plain heap, unchanged). Unlike slots 17..24,
// an untracked handle is not passed on: slot 70 has no PC counterpart that could validate it, so a handle with no
// registry entry, on another device's heap, of another heap type or off a descriptor boundary is invalid. A heap whose
// real start is unknown (real_start 0) cannot be written either. `out` is the real CPU handle.
bool registered_descriptor(const DeviceProxy* device, SIZE_T ptr, D3D12_DESCRIPTOR_HEAP_TYPE type, SIZE_T& out) {
    out = 0;
    if (!ptr) return false;
    std::lock_guard lock(descriptor_range_mutex());
    for (const DescriptorRange& r : descriptor_ranges()) {
        SIZE_T base = 0;
        if (r.custom_start && ptr >= r.custom_start && ptr - r.custom_start < r.byte_size)
            base = r.custom_start;
        else if (r.real_start && ptr >= r.real_start && ptr - r.real_start < r.byte_size)
            base = r.real_start;
        else
            continue;
        // Ranges never alias (register_descriptor_range), so the first range that holds `ptr` is its only meaning.
        const SIZE_T offset = ptr - base;
        if (r.device != device || r.type != type || !r.increment || !r.real_start || offset % r.increment != 0 ||
            r.byte_size - offset < r.increment)
            return false;
        out = r.real_start + offset;
        return true;
    }
    return false;
}

// A copy of a CPU-handle array with each range translated, for CopyDescriptors. Nothing throws (a thunk must not
// unwind into the title): 16 entries inline, more with new (std::nothrow). One lock covers the whole array.
class TranslatedHandles {
public:
    // False on allocation failure or an invalid range; nothing is forwarded then.
    bool fill(const DeviceProxy* device, const D3D12_CPU_DESCRIPTOR_HANDLE* in, const UINT* sizes, UINT n,
              D3D12_DESCRIPTOR_HEAP_TYPE type) {
        if (!in || !n) return true;
        D3D12_CPU_DESCRIPTOR_HANDLE* copy = inline_;
        if (n > std::size(inline_)) {
            heap_.reset(new (std::nothrow) D3D12_CPU_DESCRIPTOR_HANDLE[n]);
            if (!heap_) return false;
            copy = heap_.get();
        }
        std::lock_guard lock(descriptor_range_mutex());
        for (UINT i = 0; i < n; ++i)
            if (translate_locked(device, in[i].ptr, sizes ? sizes[i] : 1, type, copy[i].ptr) ==
                DescriptorHandleTranslation::invalid)
                return false;
        data_ = copy;
        return true;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE* data(const D3D12_CPU_DESCRIPTOR_HANDLE* fallback) const {
        return data_ ? data_ : fallback;
    }

private:
    D3D12_CPU_DESCRIPTOR_HANDLE inline_[16];
    std::unique_ptr<D3D12_CPU_DESCRIPTOR_HANDLE[]> heap_;
    const D3D12_CPU_DESCRIPTOR_HANDLE* data_ = nullptr;
};

// A device slot 17..24 call with an invalid handle: not forwarded (the methods are void, so the title cannot be told),
// counted, logged on the first call per slot (every call with XS_TRACE) and traced "refused".
void refuse_descriptor_call(unsigned slot, SIZE_T ptr) {
    g_refused_handle_calls.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<bool> warned[25];
    if ((slot < std::size(warned) && first_call(warned[slot])) || trace_enabled())
        std::fprintf(stderr,
                     "[rt] d3d12_xs: device slot %u (%s) refused: CPU descriptor handle 0x%llx is not a live span of "
                     "this device's heap of that type; the call is not forwarded\n",
                     slot, slot_name(slot), static_cast<unsigned long long>(ptr));
    d3d12_trace::record(slot_name(slot), "refused");
}

// The allocator slot 14 consults: an explicit one (tests) before the default (D3D12XboxCreateDevice's bridge).
std::mutex& heap_allocator_mutex() {
    static auto* m = new std::mutex;
    return *m;
}
DescriptorHeapAllocator g_explicit_heap_allocator{};
bool g_has_explicit_heap_allocator = false;
DescriptorHeapAllocator g_default_heap_allocator{};

DescriptorHeapAllocator current_heap_allocator() {
    std::lock_guard lock(heap_allocator_mutex());
    return g_has_explicit_heap_allocator ? g_explicit_heap_allocator : g_default_heap_allocator;
}

unsigned long heap_add_ref(HeapProxy* self) {
    note_heap_slot(1);
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

// The last Release: the range goes first (no translation can name this heap afterwards), then the proxy, the real
// heap and the device reference, and the title's backing last, through the pair that allocated it.
unsigned long heap_release(HeapProxy* self) {
    note_heap_slot(2);
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) {
        unregister_descriptor_range(self);
        ID3D12DescriptorHeap* real = self->real;
        DeviceProxy* device = self->device;
        const DescriptorHeapBacking backing = self->backing;
        const auto release_backing = self->release_backing;
        const bool backed = self->custom_cpu_start != 0;
        delete self;
        if (real) real->Release();
        if (device) proxy_release(device);
        if (backed && release_backing) release_backing(backing, kXmemDescriptorHeapAttributes);
    }
    return refs;
}

bool is_heap_iid(const IID& iid) {
    return IsEqualIID(iid, __uuidof(ID3D12DescriptorHeap)) || IsEqualIID(iid, __uuidof(ID3D12Pageable)) ||
           IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
           IsEqualIID(iid, __uuidof(IUnknown));
}

HRESULT heap_query_interface(HeapProxy* self, const IID* riid, void** out) {
    note_heap_slot(0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (!is_heap_iid(*riid)) return E_NOINTERFACE;
    *out = self;
    heap_add_ref(self);
    return S_OK;
}

// ID3D12DeviceChild::GetDevice on the heap must hand back the Xbox-ABI device, not the raw PC device.
HRESULT heap_get_device(HeapProxy* self, const IID* riid, void** out) {
    note_heap_slot(7);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (!self->device) return E_NOINTERFACE;
    return proxy_query_interface(self->device, riid, out);
}

// The PC methods write through the hidden return pointer (RDX) on the measured runtime; the Xbox ABI wants the
// 8-byte value in RAX. Call with both conventions and keep whichever produced a value, so a runtime that really
// returns in RAX also works. The extra register is harmless when the callee ignores it.
D3D12_CPU_DESCRIPTOR_HANDLE raw_heap_get_cpu_handle(ID3D12DescriptorHeap* real, void** real_vtable) {
    constexpr SIZE_T kUnset = ~static_cast<SIZE_T>(0);
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = kUnset;
    using Fn = D3D12_CPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*, D3D12_CPU_DESCRIPTOR_HANDLE*);
    const D3D12_CPU_DESCRIPTOR_HANDLE returned = reinterpret_cast<Fn>(real_vtable[9])(real, &handle);
    return handle.ptr != kUnset ? handle : returned;
}

// A title-backed heap answers with the title's address (PR #7021: the title checks it); slot 10 keeps the real GPU
// start, so the title's gpu - cpu delta still lands on the real shader-visible heap.
D3D12_CPU_DESCRIPTOR_HANDLE heap_get_cpu_handle(HeapProxy* self) {
    note_heap_slot(9);
    if (self->custom_cpu_start) return D3D12_CPU_DESCRIPTOR_HANDLE{self->custom_cpu_start};
    return raw_heap_get_cpu_handle(self->real, self->real_vtable);
}

D3D12_GPU_DESCRIPTOR_HANDLE heap_get_gpu_handle(HeapProxy* self) {
    note_heap_slot(10);
    constexpr UINT64 kUnset = ~static_cast<UINT64>(0);
    D3D12_GPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = kUnset;
    using Fn = D3D12_GPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*, D3D12_GPU_DESCRIPTOR_HANDLE*);
    const D3D12_GPU_DESCRIPTOR_HANDLE returned =
        reinterpret_cast<Fn>(self->real_vtable[10])(self->real, &handle);
    return handle.ptr != kUnset ? handle : returned;
}

template <unsigned Slot>
std::uint64_t WINAPI heap_slot(HeapProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                               std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                               std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                               std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        return hr_to_u64(heap_query_interface(self, reinterpret_cast<const IID*>(a1),
                                              reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return heap_add_ref(self);
    } else if constexpr (Slot == 2) {
        return heap_release(self);
    } else if constexpr (Slot == 7) {
        return hr_to_u64(heap_get_device(self, reinterpret_cast<const IID*>(a1),
                                         reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 9) {
        return static_cast<std::uint64_t>(heap_get_cpu_handle(self).ptr);
    } else if constexpr (Slot == 10) {
        return static_cast<std::uint64_t>(heap_get_gpu_handle(self).ptr);
    } else {
        note_heap_slot(Slot);
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
        return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }
}

template <std::size_t... I>
void fill_heap_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&heap_slot<static_cast<unsigned>(I)>) ), ...);
}

void** get_heap_vtable() {
    static void** vtable = [] {
        static void* storage[kHeapSlots];
        fill_heap_vtable(storage, std::make_index_sequence<kHeapSlots>{});
        return storage;
    }();
    return vtable;
}

// What slot 14 learned about the new heap. byte_size 0 or real_cpu_start 0: the real range is not tracked.
struct HeapCreation {
    D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    UINT increment = 0;
    SIZE_T byte_size = 0;
    SIZE_T real_cpu_start = 0;
    SIZE_T custom_cpu_start = 0;  // 0: no title backing
    DescriptorHeapBacking backing{};
    void (*release_backing)(const DescriptorHeapBacking&, std::uint64_t) = nullptr;
};

// On failure the caller keeps `real` and the backing in `creation` and must release both; on success the proxy owns
// them. The range is registered before QueryInterface can hand the proxy out, and removed again if that fails.
HRESULT create_heap_proxy(ID3D12DescriptorHeap* real, DeviceProxy* device, REFIID riid, void** out,
                          const HeapCreation& creation) {
    if (!real || !device) return E_POINTER;
    if (!out) return E_POINTER;
    *out = nullptr;

    HeapProxy* proxy = new (std::nothrow) HeapProxy;
    if (!proxy) return E_OUTOFMEMORY;
    proxy->vtable = get_heap_vtable();
    proxy->real = real;
    proxy->device = device;
    proxy->real_vtable = *reinterpret_cast<void***>(real);
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy->custom_cpu_start = creation.custom_cpu_start;
    proxy->backing = creation.backing;
    proxy->release_backing = creation.release_backing;
    proxy_add_ref(device);  // the heap keeps its Xbox-ABI device alive for GetDevice

    if (creation.byte_size && (creation.real_cpu_start || creation.custom_cpu_start)) {
        const DescriptorRange range{device,          proxy,
                                    creation.real_cpu_start, creation.custom_cpu_start,
                                    creation.byte_size,      creation.increment,
                                    creation.type};
        const RangeRegistration registered = register_descriptor_range(range);
        if (registered != RangeRegistration::ok) {
            std::fprintf(stderr,
                         "[rt] d3d12_xs: descriptor heap refused: %s (title 0x%llx, real 0x%llx, 0x%llx bytes)\n",
                         registered == RangeRegistration::aliased ? "its range overlaps a live heap's range"
                                                                  : "out of memory for its range entry",
                         static_cast<unsigned long long>(creation.custom_cpu_start),
                         static_cast<unsigned long long>(creation.real_cpu_start),
                         static_cast<unsigned long long>(creation.byte_size));
            delete proxy;
            proxy_release(device);
            return registered == RangeRegistration::aliased ? E_FAIL : E_OUTOFMEMORY;
        }
    }

    const HRESULT hr = heap_query_interface(proxy, &riid, out);
    if (FAILED(hr)) {
        // No reference escaped, and the caller still owns `real`, the backing and its reference to `device`.
        unregister_descriptor_range(proxy);
        delete proxy;
        proxy_release(device);
        return hr;
    }
    if (!d3d12_trace::enabled())
        std::fprintf(stderr, "[rt] d3d12_xs: heap proxy %p wraps PC heap %p (device proxy %p)\n",
                     static_cast<void*>(proxy), static_cast<void*>(real), static_cast<void*>(device));
    heap_release(proxy);
    return S_OK;
}

// ---- Command queue and fence proxies --------------------------------------------------------------------------
//
// Device slots 8 (CreateCommandQueue), 75 (CreateCommandQueue1), 36 (CreateFence) and 32 (OpenSharedHandle, for a
// fence) wrap what they create, so a title never holds a PC or Vulkan-layer queue or fence. The proxies forward
// the public chain in PC order, exactly like the device proxy, and a slot past the PC vtable is logged and refused
// with E_NOTIMPL. PresentX, SuspendX and ResumeX are queue methods past the public chain
// (docs/d3d12_command_objects.md §5.3). Their slots are not known and nothing is guessed: the first run of the
// executable names them in the log.
//
// Slot numbers are those of the Windows SDK 10.0.26100 d3d12.h: ID3D12CommandQueue has 19, ID3D12Fence 11 and
// ID3D12Fence1 12. That the console keeps these public slots, and keeps device slots 8/32/36/75, is an assumption
// the shipped binaries cannot test (§3.5: the package has no queue or fence client). Every queue and fence method
// takes integer and pointer arguments only; GetDesc returns its struct through the hidden pointer in RDX, which is
// forwarded unchanged (no Xbox variant is measured, §6).
//
// The refused range is the proxy's own table: 128 queue slots and 64 fence slots, every one a thunk. The table sits
// at the end of the first page of a 64 KiB reservation whose other pages are never committed, so a slot index past
// the table makes the vtable read itself fault, at an address that names the slot (a vectored handler logs it and
// lets the fault continue), instead of jumping through whatever memory follows. A title whose vtable read lands
// further away than that reservation is not covered.
//
// Objects cross back into the real runtime wherever a device or queue method takes them, and the real object is
// passed there: queue Signal/Wait (14/15) take a fence; device CreateSharedHandle (31) a device child,
// MakeResident/Evict (34/35), SetResidencyPriority (46) and EnqueueMakeResident (50) pageables, and
// SetEventOnMultipleFenceCompletion (45) and EnqueueMakeResident fences. The heap proxies of slot 14 are pageables
// and device children too and are unwrapped in the same places. Command lists from device slot 73 come back as
// console-shaped list proxies (the command-list section below): proxy_real_object knows them, queue
// ExecuteCommandLists (10) unwraps the list array (UnwrappedArray) and refuses a submission that holds a list with
// unexecuted console commands, and the list unwraps allocator, bundle and heap proxies itself. Lists from the PC
// slot 12 are not wrapped and pass through unchanged.
//
// QueryInterface answers only the interfaces the proxy models (the base chain and its own interface, Fence1 when
// the real fence has it), like the device and heap proxies, so the COM identity holds and no real object leaks to
// the title. Palmwave's own code reaches the real object through queue_proxy_real_queue / fence_proxy_real_fence /
// allocator_proxy_real_allocator.
//
// Command allocators (device slot 9) are the third kind: their public chain is 9 slots (Reset at 8). Slot 9 wraps
// only an allocator that answers kPalmwaveCommandAllocatorTag, because a list takes the title's allocator in
// CreateCommandList and Reset and only the Vulkan layer's lists resolve a proxy there (CommandAllocator::from);
// a PC runtime allocator is handed out unchanged until a command-list proxy unwraps it at Reset.

enum class ChildKind { queue, fence, allocator };

constexpr unsigned kQueueSlots = 128;        // the table: 19 public slots plus a refused tail
constexpr unsigned kFenceSlots = 64;         // the table: 12 public slots plus a refused tail
constexpr unsigned kQueuePublicSlots = 19;   // ID3D12CommandQueue
constexpr unsigned kFencePublicSlots = 11;   // ID3D12Fence
constexpr unsigned kFence1PublicSlots = 12;  // ID3D12Fence1
constexpr unsigned kAllocatorSlots = 64;     // the table: 9 public slots plus a refused tail
constexpr unsigned kAllocatorPublicSlots = 9;  // ID3D12CommandAllocator: the base chain and Reset (slot 8)
constexpr SIZE_T kTableRegion = 64 * 1024;   // one allocation granule: the table page plus 15 never-committed pages
static_assert(kQueueSlots > kQueuePublicSlots && kFenceSlots > kFence1PublicSlots &&
                  kAllocatorSlots > kAllocatorPublicSlots,
              "the tables need a tail");
static_assert(kQueueSlots * sizeof(void*) <= 4096 && kFenceSlots * sizeof(void*) <= 4096 &&
                  kAllocatorSlots * sizeof(void*) <= 4096,
              "a table must fit in the first page of its reservation");

constexpr unsigned kQueueSignal = 14, kQueueWait = 15;  // (ID3D12Fence*, UINT64)

struct ChildProxy {
    void** vtable;          // offset 0: the caller's ID3D12CommandQueue* / ID3D12Fence* points here
    IUnknown* real;         // offset 8: the PC or Vulkan-layer object every thunk forwards to
    DeviceProxy* device;    // keeps the Xbox-ABI device alive; GetDevice returns it
    void** real_vtable;
    unsigned slot_count;    // public slots the real object implements; the rest of the table is refused
    std::atomic<unsigned long> refs;
};

template <ChildKind K>
struct ChildStats {
    static constexpr unsigned kSlots = K == ChildKind::queue   ? kQueueSlots
                                       : K == ChildKind::fence ? kFenceSlots
                                                               : kAllocatorSlots;
    static inline std::atomic<std::uint64_t> hits[kSlots];
    static inline std::atomic<bool> first[kSlots];
    static inline std::atomic<bool> beyond_warned[kSlots];
    static inline std::atomic<std::uint64_t> overruns{0};
    // Where the table and its reservation are: written once, before the table is handed out.
    static inline std::atomic<std::uintptr_t> table_begin{0}, table_end{0}, region_end{0};
};

template <ChildKind K>
constexpr const char* child_kind_name() {
    return K == ChildKind::queue ? "queue" : K == ChildKind::fence ? "fence" : "allocator";
}

template <ChildKind K>
const char* child_public_name(unsigned slot) {
    static constexpr const char* object[] = {"QueryInterface", "AddRef", "Release", "GetPrivateData",
                                             "SetPrivateData", "SetPrivateDataInterface", "SetName", "GetDevice"};
    static constexpr const char* queue[] = {"UpdateTileMappings", "CopyTileMappings", "ExecuteCommandLists",
                                            "SetMarker", "BeginEvent", "EndEvent", "Signal", "Wait",
                                            "GetTimestampFrequency", "GetClockCalibration", "GetDesc"};
    static constexpr const char* fence[] = {"GetCompletedValue", "SetEventOnCompletion", "Signal",
                                            "GetCreationFlags"};
    if (slot < 8) return object[slot];
    if constexpr (K == ChildKind::queue) {
        if (slot < kQueuePublicSlots) return queue[slot - 8];
    } else if constexpr (K == ChildKind::fence) {
        if (slot < kFence1PublicSlots) return fence[slot - 8];
    } else {
        if (slot == 8) return "Reset";
    }
    return nullptr;
}

// Fixed names for the trace channel (d3d12_trace::record takes fixed strings): one per table slot, built once, in
// the trace grammar of runtime/README.md and check_d3d12_trace.py (an identifier with `::`, no spaces):
// XboxID3D12CommandQueue::Signal, and XboxID3D12CommandQueue::Slot40 for a slot past the public chain.
template <ChildKind K>
const char* child_trace_name(unsigned slot) {
    static const auto names = [] {
        static char storage[ChildStats<K>::kSlots][64];
        const char* iface = K == ChildKind::queue   ? "XboxID3D12CommandQueue"
                            : K == ChildKind::fence ? "XboxID3D12Fence"
                                                    : "XboxID3D12CommandAllocator";
        for (unsigned s = 0; s < ChildStats<K>::kSlots; ++s) {
            const char* method = child_public_name<K>(s);
            if (method)
                std::snprintf(storage[s], sizeof(storage[s]), "%s::%s", iface, method);
            else
                std::snprintf(storage[s], sizeof(storage[s]), "%s::Slot%u", iface, s);
        }
        return &storage;
    }();
    return (*names)[slot < ChildStats<K>::kSlots ? slot : 0];
}

template <ChildKind K>
void note_child_slot(unsigned slot) {
    using S = ChildStats<K>;
    if (slot >= S::kSlots) return;
    count_hit(S::hits[slot]);
    const bool first = first_call(S::first[slot]);
    const char* method = child_public_name<K>(slot);
    if (first) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s slot %u (%s) first call\n", child_kind_name<K>(), slot,
                     method ? method : "past the public chain");
    } else if (slot >= 3 && trace_enabled()) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s slot %u (%s) call\n", child_kind_name<K>(), slot,
                     method ? method : "past the public chain");
    }
}

// The counters reset_device_proxy_stats clears for one kind: per-slot hits, first-call and beyond-table flags, and
// the faulted reads past the table.
template <ChildKind K>
void reset_child_stats() {
    using S = ChildStats<K>;
    for (unsigned slot = 0; slot < S::kSlots; ++slot) {
        S::hits[slot].store(0, std::memory_order_relaxed);
        S::first[slot].store(false, std::memory_order_relaxed);
        S::beyond_warned[slot].store(false, std::memory_order_relaxed);
    }
    S::overruns.store(0, std::memory_order_relaxed);
}

// Frees a proxy whose last reference is gone, with the real object and the device reference it held.
void destroy_child(ChildProxy* self) {
    IUnknown* real = self->real;
    DeviceProxy* device = self->device;
    delete self;
    if (real) real->Release();
    if (device) proxy_release(device);
}

template <ChildKind K>
unsigned long child_add_ref(ChildProxy* self) {
    note_child_slot<K>(1);
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

template <ChildKind K>
unsigned long child_release(ChildProxy* self) {
    note_child_slot<K>(2);
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) destroy_child(self);
    return refs;
}

// The interfaces the proxy answers: the base chain and the object's own interface.
template <ChildKind K>
bool is_child_iid(const IID& iid) {
    if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
        IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Pageable)))
        return true;
    if constexpr (K == ChildKind::queue) {
        return IsEqualIID(iid, __uuidof(ID3D12CommandQueue));
    } else if constexpr (K == ChildKind::fence) {
        return IsEqualIID(iid, __uuidof(ID3D12Fence)) || IsEqualIID(iid, __uuidof(ID3D12Fence1));
    } else {
        return IsEqualIID(iid, __uuidof(ID3D12CommandAllocator));
    }
}

bool supports_iid(IUnknown* real, REFIID iid) {
    void* found = nullptr;
    const HRESULT hr = real->QueryInterface(iid, &found);
    if (FAILED(hr) || !found) return false;
    static_cast<IUnknown*>(found)->Release();
    return true;
}

// An interface the real object implements but the proxy does not model is refused where the PC runtime would answer:
// logged once per interface (the first 32 of them), so a title that stops after such a refusal shows what it asked
// for. Also reached from wrap_child, for the interface a title creates a queue or fence as.
void log_refused_iid(const char* kind, const IID& iid) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static IID seen[32];
    static unsigned count = 0;
    AcquireSRWLockExclusive(&lock);
    bool known = false;
    for (unsigned i = 0; i < count && !known; ++i) known = IsEqualIID(seen[i], iid) != 0;
    const bool log = !known && count < std::size(seen);
    if (log) seen[count++] = iid;
    ReleaseSRWLockExclusive(&lock);
    if (!log) return;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: %s proxy refuses {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}, which the real "
                 "object implements; E_NOINTERFACE\n",
                 kind, iid.Data1, iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3],
                 iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
}

template <ChildKind K>
HRESULT child_query_interface(ChildProxy* self, const IID* riid, void** out) {
    note_child_slot<K>(0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    // Only what the proxy models, and of that only what the real object implements (ID3D12Fence1 is optional).
    if (!is_child_iid<K>(*riid)) {
        if (supports_iid(self->real, *riid)) log_refused_iid(child_kind_name<K>(), *riid);
        return E_NOINTERFACE;
    }
    if (!supports_iid(self->real, *riid)) return E_NOINTERFACE;
    *out = self;
    child_add_ref<K>(self);
    return S_OK;
}

// ID3D12DeviceChild::GetDevice hands back the Xbox-ABI device, not the real one. Without an output it is the real
// object's own answer (what the PC runtime returns for a null pointer).
template <ChildKind K>
HRESULT child_get_device(ChildProxy* self, const IID* riid, void** out) {
    note_child_slot<K>(7);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure (bc59ef46)
    if (!riid) return E_POINTER;
    if (!out) {
        using Fn = HRESULT(WINAPI*)(IUnknown*, const IID*, void**);
        return reinterpret_cast<Fn>(self->real_vtable[7])(self->real, riid, nullptr);
    }
    if (!self->device) return E_NOINTERFACE;
    return proxy_query_interface(self->device, riid, out);
}

template <ChildKind K>
void** get_child_vtable();

template <ChildKind K>
bool is_child_proxy(const void* object) {
    return object && *reinterpret_cast<void* const*>(object) == reinterpret_cast<void*>(get_child_vtable<K>());
}

// Command-list proxy helpers, defined with the list proxy below (ListProxy is complete only there): the real list
// behind a list proxy, and the first console slot such a list recorded without executing it (0 when it recorded
// none, or when `object` is not a list proxy).
IUnknown* list_real_or_null(const void* object);
unsigned list_unsupported_slot(const void* object);

// The real object behind a heap, queue, fence, allocator or command-list proxy, or null when `object` is none of them.
IUnknown* proxy_real_object(const void* object) {
    if (!object) return nullptr;
    void* const vtable = *reinterpret_cast<void* const*>(object);
    if (vtable == reinterpret_cast<void*>(get_heap_vtable())) return static_cast<const HeapProxy*>(object)->real;
    if (vtable == reinterpret_cast<void*>(get_child_vtable<ChildKind::queue>()) ||
        vtable == reinterpret_cast<void*>(get_child_vtable<ChildKind::fence>()) ||
        vtable == reinterpret_cast<void*>(get_child_vtable<ChildKind::allocator>()))
        return static_cast<const ChildProxy*>(object)->real;
    return list_real_or_null(object);
}

// `object` with a proxy replaced by its real object; anything else (a real object, null) unchanged.
std::uint64_t unwrap_object(std::uint64_t object) {
    IUnknown* real = proxy_real_object(reinterpret_cast<const void*>(object));
    return real ? reinterpret_cast<std::uint64_t>(real) : object;
}

// A copy of an array of interface pointers with every proxy replaced by its real object. Nothing here throws, since
// an exception must not leave a thunk for the title's code: up to 16 entries are kept inline (no allocation, which
// also suits a per-frame ExecuteCommandLists), a longer array is allocated with new (std::nothrow).
class UnwrappedArray {
public:
    // Copies `count` entries (a UINT in every PC prototype) of `objects`. False on allocation failure.
    bool fill(std::uint64_t objects, std::uint64_t count) {
        const auto n = static_cast<std::uint32_t>(count);
        if (!objects || !n) return true;
        std::uint64_t* copy = inline_;
        if (n > std::size(inline_)) {
            heap_.reset(new (std::nothrow) std::uint64_t[n]);
            if (!heap_) return false;
            copy = heap_.get();
        }
        const auto* in = reinterpret_cast<const std::uint64_t*>(objects);
        for (std::uint32_t i = 0; i < n; ++i) copy[i] = unwrap_object(in[i]);
        data_ = copy;
        return true;
    }
    // The copy to pass on, or 0 when there was no array (the caller's argument then goes on unchanged).
    std::uint64_t data() const { return reinterpret_cast<std::uint64_t>(data_); }

private:
    std::uint64_t inline_[16];
    std::unique_ptr<std::uint64_t[]> heap_;
    std::uint64_t* data_ = nullptr;
};

// The vtable read for a slot past a table faults in the reservation behind it; name the slot. Only reads the
// exception record and writes with WriteFile, so it is safe whatever the faulting thread holds.
template <ChildKind K>
void report_table_overrun(std::uintptr_t address) {
    using S = ChildStats<K>;
    const std::uintptr_t begin = S::table_begin.load(std::memory_order_acquire);
    const std::uintptr_t end = S::table_end.load(std::memory_order_acquire);
    const std::uintptr_t limit = S::region_end.load(std::memory_order_acquire);
    if (!end || address < end || address >= limit) return;
    S::overruns.fetch_add(1, std::memory_order_relaxed);
    char line[160];
    unsigned long long slot = (address - begin) / sizeof(void*);
    char digits[24];
    int d = 0;
    do {
        digits[d++] = static_cast<char>('0' + slot % 10);
        slot /= 10;
    } while (slot && d < 23);
    int n = 0;
    const auto put = [&](const char* text) {
        while (*text && n < static_cast<int>(sizeof(line)) - 1) line[n++] = *text++;
    };
    put("[rt] d3d12_xs: ");
    put(child_kind_name<K>());
    put(" slot ");
    while (d > 0 && n < static_cast<int>(sizeof(line)) - 1) line[n++] = digits[--d];
    put(" is past the proxy table; the vtable read faults\n");
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(n), &written, nullptr);
}

LONG CALLBACK table_overrun_handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
    if (record && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const auto address = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
        report_table_overrun<ChildKind::queue>(address);
        report_table_overrun<ChildKind::fence>(address);
        report_table_overrun<ChildKind::allocator>(address);
    }
    return EXCEPTION_CONTINUE_SEARCH;  // diagnostics only: the fault goes on to the title's handlers or the crash
}

template <ChildKind K, unsigned Slot>
std::uint64_t WINAPI child_slot(ChildProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                                std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                                std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        d3d12_trace::record(child_trace_name<K>(Slot));
        return hr_to_u64(child_query_interface<K>(self, reinterpret_cast<const IID*>(a1),
                                                  reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return child_add_ref<K>(self);
    } else if constexpr (Slot == 2) {
        return child_release<K>(self);
    } else if constexpr (Slot == 7) {
        d3d12_trace::record(child_trace_name<K>(Slot));
        return hr_to_u64(child_get_device<K>(self, reinterpret_cast<const IID*>(a1),
                                             reinterpret_cast<void**>(a2)));
    } else {
        note_child_slot<K>(Slot);
        if (Slot >= self->slot_count) {
            bool expected = false;
            if (ChildStats<K>::beyond_warned[Slot].compare_exchange_strong(expected, true,
                                                                            std::memory_order_relaxed)) {
                std::fprintf(stderr,
                             "[rt] d3d12_xs: %s slot %u is beyond the PC %s vtable (%u slots); returning E_NOTIMPL "
                             "(args 0x%llx 0x%llx 0x%llx 0x%llx)\n",
                             child_kind_name<K>(), Slot, child_kind_name<K>(), self->slot_count,
                             static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                             static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4));
            }
            d3d12_trace::record(child_trace_name<K>(Slot), "unsupported");
            return hr_to_u64(E_NOTIMPL);
        }
        // Queue slot 10: ExecuteCommandLists(count, lists), void. A console list proxy that recorded a command it did
        // not execute is never submitted, so no partial list runs as if it were complete; the whole call is refused
        // (the list's Close already returned E_NOTIMPL). Otherwise the array is copied with every list proxy
        // replaced by its real list.
        UnwrappedArray unwrapped;
        if constexpr (K == ChildKind::queue && Slot == 10) {
            const auto count = static_cast<std::uint32_t>(a1);
            const auto* lists = reinterpret_cast<const std::uint64_t*>(a2);
            for (std::uint32_t i = 0; lists && i < count; ++i) {
                if (const unsigned slot = list_unsupported_slot(reinterpret_cast<const void*>(lists[i]))) {
                    static std::atomic<bool> logged{false};
                    if (first_call(logged))
                        std::fprintf(stderr,
                                     "[rt] d3d12_xs: ExecuteCommandLists refused: list %u of %u recorded console list "
                                     "slot %u, which is not executed; nothing in this call is submitted\n",
                                     i, count, slot);
                    d3d12_trace::record(child_trace_name<K>(Slot), "refused");
                    return 0;
                }
            }
            if (!unwrapped.fill(a2, a1)) {
                std::fprintf(stderr, "[rt] d3d12_xs: ExecuteCommandLists of %u lists not submitted: out of memory\n",
                             count);
                d3d12_trace::record(child_trace_name<K>(Slot), "failed");
                return 0;
            }
            if (unwrapped.data()) a2 = unwrapped.data();
        }
        d3d12_trace::record(child_trace_name<K>(Slot));  // "called": the real object's result is not inspected
        if constexpr (K == ChildKind::queue && (Slot == kQueueSignal || Slot == kQueueWait)) a1 = unwrap_object(a1);

        // The PC vtable entry with the real object as `this`; integer/pointer arguments, the hidden struct-return
        // pointer and stack arguments keep their positions, as in the device proxy.
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
        return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }
}

template <ChildKind K, std::size_t... I>
void fill_child_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&child_slot<K, static_cast<unsigned>(I)>)), ...);
}

// True when the fault logger is installed (once per process; a failure is logged once).
bool install_table_overrun_handler() {
    static const bool installed = [] {
        if (AddVectoredExceptionHandler(0, &table_overrun_handler)) return true;
        std::fprintf(stderr, "[rt] d3d12_xs: AddVectoredExceptionHandler failed (error %lu); a slot past a queue or "
                     "fence proxy table faults without being named\n", GetLastError());
        return false;
    }();
    return installed;
}

template <ChildKind K>
void** get_child_vtable() {
    static void** vtable = [] {
        using S = ChildStats<K>;
        constexpr std::size_t bytes = S::kSlots * sizeof(void*);
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const SIZE_T page = system.dwPageSize;
        if (void* base = page >= bytes ? VirtualAlloc(nullptr, kTableRegion, MEM_RESERVE, PAGE_NOACCESS) : nullptr) {
            if (VirtualAlloc(base, page, MEM_COMMIT, PAGE_READWRITE)) {
                auto* table = reinterpret_cast<void**>(static_cast<char*>(base) + page - bytes);
                fill_child_vtable<K>(table, std::make_index_sequence<S::kSlots>{});
                DWORD old = 0;
                if (!VirtualProtect(base, page, PAGE_READONLY, &old))  // the table works, but stays writable
                    std::fprintf(stderr, "[rt] d3d12_xs: %s proxy table stays writable (VirtualProtect error %lu)\n",
                                 child_kind_name<K>(), GetLastError());
                S::table_begin.store(reinterpret_cast<std::uintptr_t>(table), std::memory_order_release);
                S::region_end.store(reinterpret_cast<std::uintptr_t>(base) + kTableRegion, std::memory_order_release);
                // table_end is what "guarded" means (command_object_tables_guarded, the fault logger): set only when
                // a fault past the table will be named. Without the logger the region still faults, unnamed.
                if (install_table_overrun_handler())
                    S::table_end.store(reinterpret_cast<std::uintptr_t>(base) + page, std::memory_order_release);
                return table;
            }
            VirtualFree(base, 0, MEM_RELEASE);
        }
        // No reservation: the table still works, but a slot past it is not caught.
        std::fprintf(stderr, "[rt] d3d12_xs: %s proxy table without a guard region\n", child_kind_name<K>());
        static void* storage[S::kSlots];
        fill_child_vtable<K>(storage, std::make_index_sequence<S::kSlots>{});
        return storage;
    }();
    return vtable;
}

// Wraps `created` (one reference, owned by the proxy on success, released on failure) and returns `riid` from the
// proxy through `out`.
template <ChildKind K>
HRESULT wrap_child(IUnknown* created, DeviceProxy* device, const IID* riid, void** out) {
    if (!created) return E_FAIL;
    ChildProxy* proxy = new (std::nothrow) ChildProxy;
    if (!proxy) {
        created->Release();
        return E_OUTOFMEMORY;
    }
    proxy->vtable = get_child_vtable<K>();
    proxy->real = created;
    proxy->device = device;
    proxy->real_vtable = *reinterpret_cast<void***>(created);
    if constexpr (K == ChildKind::queue) {
        proxy->slot_count = kQueuePublicSlots;
    } else if constexpr (K == ChildKind::fence) {
        proxy->slot_count = supports_iid(created, __uuidof(ID3D12Fence1)) ? kFence1PublicSlots : kFencePublicSlots;
    } else {
        proxy->slot_count = kAllocatorPublicSlots;
    }
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy_add_ref(device);  // the object keeps its Xbox-ABI device alive for GetDevice

    const HRESULT hr = child_query_interface<K>(proxy, riid, out);
    if (FAILED(hr)) {
        destroy_child(proxy);  // no reference escaped
        return hr;
    }
    // Titles create fences at run time: the first proxy of each kind is logged, every one only with XS_TRACE.
    static std::atomic<bool> logged{false};
    if (!d3d12_trace::enabled() && (!logged.exchange(true, std::memory_order_relaxed) || trace_enabled()))
        std::fprintf(stderr, "[rt] d3d12_xs: %s proxy %p wraps %p (device proxy %p, %u public slots)\n",
                     child_kind_name<K>(), static_cast<void*>(proxy), static_cast<void*>(created),
                     static_cast<void*>(device), proxy->slot_count);
    proxy->refs.fetch_sub(1, std::memory_order_relaxed);  // drop the creation reference; `out` holds the caller's
    return S_OK;
}

// ---- Console command-list proxy (device slot 73) ------------------------------------------------------------------
//
// Adapted from upstream PR #2490 (NolanCotter/Palmwave commit 084c8bd0, GPL-3.0; see UPSTREAM.md), narrowed for
// Palmwave. Device slot 73 (CreateCommandListX) hands the title a console-shaped command list: a 160-slot table in
// console vtable order and the object body the console SDK's inline writers touch (docs/d3d12_command_objects.md
// §3.4-3.5a). The write cursor (+0x18) is pinned at both record limits (+0x28, +0x30), so an inline writer always
// finds the record buffer full and calls the vtable method instead; the proxy never interprets inline records. The
// flag bytes (+0x140..+0x143), the root-parameter header table (+0x344) and the pipeline record (+0x544) stay zero.
//
// Slot order: the PC order through slot 26 (measured 9, 10, 12-17, 19, 20, 25, 26), four console-only methods at
// 27-30, then the PC chain shifted +4 (measured at console 32, 33, 41, 43, 45, 48, 51, 53; 31 -> ExecuteBundle and
// the rest through 58 follow the same shift). Console 59 is not forwarded: the +4 shift would make it SetPredication,
// but a retail-title measurement puts an indirect/mesh dispatch there (docs/d3d12_vulkan.md §8.2 question 2). Console
// 60 is EndEvent (PC 58), 61 ExecuteIndirect (PC 59), 139 Reset(allocator, pso) (PC 10, measured on the retail
// executable), 146 the enhanced Barrier (PC GCL7 80) and 148 AllocateUploadMemory (the arena below).
//
// Console slots 50..54 (OMSetRenderTargets, ClearDepthStencilView, ClearRenderTargetView, ClearUnorderedAccessView*)
// translate their CPU descriptor handles like device slots 17..24; an invalid handle marks the list as below. Slot 51
// has a typed thunk for its FLOAT argument.
//
// Every other slot is unsupported: it is logged with its first argument words, traced "unsupported", returns
// E_NOTIMPL and marks the list. A marked list still closes its real list, but Close returns E_NOTIMPL and the
// queue refuses to submit it, so a partial list never executes as if it were complete. Reset clears the mark.
// Lists from the PC slot 12 are not wrapped: callers compiled against the PC headers (the synthetic GPU tests and
// fixtures) keep the PC vtable.

constexpr unsigned kListSlots = 160;
constexpr unsigned kListNotForwarded = ~0u;
constexpr unsigned kConsoleListClose = 9;
constexpr unsigned kConsoleExecuteBundle = 31;
constexpr unsigned kConsoleSetDescriptorHeaps = 32;
// CPU-descriptor-handle slots (adapted from upstream PR #7880, alexander-src, 0aba6c36, GPL-3.0): PC 46..50.
constexpr unsigned kConsoleOMSetRenderTargets = 50;
constexpr unsigned kConsoleClearDepthStencilView = 51;
constexpr unsigned kConsoleClearRenderTargetView = 52;
constexpr unsigned kConsoleClearUavUint = 53;
constexpr unsigned kConsoleClearUavFloat = 54;
constexpr unsigned kConsoleListResetX = 139;
constexpr unsigned kConsoleListBarrier = 146;
constexpr unsigned kConsoleAllocateUpload = 148;
constexpr unsigned kPcListReset = 10;

constexpr unsigned console_to_pc_list_slot(unsigned slot) {
    if (slot <= 26) return slot;                         // PC order
    if (slot <= 30) return kListNotForwarded;            // console-only extras
    if (slot <= 58) return slot - 4;                     // PC chain, shifted +4
    if (slot == 60) return 58;                           // EndEvent
    if (slot == 61) return 59;                           // ExecuteIndirect
    if (slot == kConsoleListResetX) return kPcListReset;  // Reset(allocator, pso)
    if (slot == kConsoleListBarrier) return 80;          // ID3D12GraphicsCommandList7::Barrier
    return kListNotForwarded;                            // 59 (disputed) and the unmeasured tail
}

// ---- Upload memory (console list slot 148, AllocateUploadMemory(size, alignment)) ----
//
// On the console the pointer AllocateUploadMemory returns is written by the CPU and handed to the GPU unchanged (the
// package passes it straight into a root SRV, docs/d3d12_command_objects.md §3.3a). A PC has two address spaces, so
// each arena chunk is a persistently mapped UPLOAD-heap buffer: the title gets the CPU pointer and writes the memory
// the GPU reads, and the six root-descriptor setters and the IA/SO view setters translate an arena address to the
// buffer's GPU address before forwarding. A device that cannot hand out a mapped buffer with a GPU address gets no
// memory (0), never host memory posing as GPU memory. Chunks are never reclaimed before the device is released:
// the console's reuse rule for this memory is not measured.

struct UploadChunk {
    UploadChunk* next;
    ID3D12Resource* resource;  // the mapped buffer, unmapped and released with the arena
    std::uint8_t* cpu_base;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_base;
    std::size_t size;
    std::size_t used;
};

struct UploadArena {
    UploadChunk* chunks = nullptr;
    std::mutex lock;
};

constexpr std::size_t kUploadChunkMin = 8u << 20;
constexpr std::uint64_t kUploadMaxAlignment = 64u << 10;  // every measured call asks for 16

UploadChunk* upload_chunk_new(DeviceProxy* device, std::size_t size) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* resource = nullptr;
    HRESULT hr = device->real->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource));
    if (FAILED(hr) || !resource) {
        std::fprintf(stderr, "[rt] d3d12_xs: upload arena chunk of %zu bytes not created (0x%08lx)\n", size,
                     static_cast<unsigned long>(FAILED(hr) ? hr : E_FAIL));
        return nullptr;
    }
    void* cpu = nullptr;
    hr = resource->Map(0, nullptr, &cpu);
    const D3D12_GPU_VIRTUAL_ADDRESS gpu = SUCCEEDED(hr) ? resource->GetGPUVirtualAddress() : 0;
    auto* chunk = SUCCEEDED(hr) && cpu && gpu ? new (std::nothrow) UploadChunk{} : nullptr;
    if (!chunk) {
        std::fprintf(stderr, "[rt] d3d12_xs: upload arena chunk of %zu bytes has no mapping or GPU address\n", size);
        if (SUCCEEDED(hr)) resource->Unmap(0, nullptr);
        resource->Release();
        return nullptr;
    }
    chunk->resource = resource;
    chunk->cpu_base = static_cast<std::uint8_t*>(cpu);
    chunk->gpu_base = gpu;
    chunk->size = size;
    return chunk;
}

// The aligned address for `need` bytes in `chunk`, advancing its cursor; 0 when the chunk has no room.
std::uint64_t upload_carve(UploadChunk* chunk, std::size_t need, std::uint64_t alignment) {
    const auto base = reinterpret_cast<std::uintptr_t>(chunk->cpu_base);
    const std::uintptr_t at = (base + chunk->used + (alignment - 1)) & ~static_cast<std::uintptr_t>(alignment - 1);
    const std::size_t offset = at - base;
    if (offset > chunk->size || need > chunk->size - offset) return 0;
    chunk->used = offset + need;
    return at;
}

std::uint64_t upload_alloc(DeviceProxy* device, std::uint64_t size, std::uint64_t alignment) {
    if (alignment < 16) alignment = 16;
    // Refused before any arithmetic: a zero size, a non-power-of-two or oversized alignment (unmeasured), and a size
    // that would wrap the chunk arithmetic (upstream review of #2490: such a call used to re-hand the same range).
    if (!size || (alignment & (alignment - 1)) != 0 || alignment > kUploadMaxAlignment ||
        size > (~static_cast<std::uint64_t>(0) >> 1) - alignment) {
        std::fprintf(stderr, "[rt] d3d12_xs: AllocateUploadMemory(%llu, %llu) refused\n",
                     static_cast<unsigned long long>(size), static_cast<unsigned long long>(alignment));
        return 0;
    }
    auto* arena = static_cast<UploadArena*>(device->upload_state.load(std::memory_order_acquire));
    if (!arena) {
        auto* fresh = new (std::nothrow) UploadArena;
        if (!fresh) return 0;
        void* expected = nullptr;
        if (device->upload_state.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel)) {
            arena = fresh;
        } else {
            delete fresh;
            arena = static_cast<UploadArena*>(expected);
        }
    }
    const auto need = static_cast<std::size_t>(size);
    std::lock_guard<std::mutex> lock(arena->lock);
    for (UploadChunk* chunk = arena->chunks; chunk; chunk = chunk->next)
        if (const std::uint64_t at = upload_carve(chunk, need, alignment)) return at;
    const std::size_t chunk_size = need + alignment > kUploadChunkMin ? need + alignment : kUploadChunkMin;
    UploadChunk* chunk = upload_chunk_new(device, chunk_size);
    if (!chunk) return 0;
    chunk->next = arena->chunks;
    arena->chunks = chunk;
    return upload_carve(chunk, need, alignment);  // fits: the chunk has `alignment` bytes of slack
}

// An address inside an arena chunk becomes the chunk's GPU address plus the offset; anything else (a resource VA the
// title already holds) passes through.
std::uint64_t upload_translate(DeviceProxy* device, std::uint64_t va) {
    auto* arena = static_cast<UploadArena*>(device->upload_state.load(std::memory_order_acquire));
    if (!arena || !va) return va;
    std::lock_guard<std::mutex> lock(arena->lock);
    for (const UploadChunk* chunk = arena->chunks; chunk; chunk = chunk->next) {
        const auto base = reinterpret_cast<std::uint64_t>(chunk->cpu_base);
        if (va >= base && va - base < chunk->size) return chunk->gpu_base + (va - base);
    }
    return va;
}

void destroy_upload_state(void* upload_state) {
    auto* arena = static_cast<UploadArena*>(upload_state);
    if (!arena) return;
    for (UploadChunk* chunk = arena->chunks; chunk;) {
        UploadChunk* next = chunk->next;
        chunk->resource->Unmap(0, nullptr);
        chunk->resource->Release();
        delete chunk;
        chunk = next;
    }
    delete arena;
}

// ---- The list proxy ----

struct ListProxy {
    void** vtable;                    // +0x00: the title's command-list pointer points here
    ID3D12GraphicsCommandList* real;  // +0x08
    DeviceProxy* device;              // +0x10: keeps the Xbox-ABI device alive; GetDevice returns it
    // +0x18..: the console object body. +0x18 holds the write cursor, +0x28 and +0x30 the record limits; the rest is
    // room for the measured flag bytes, root-parameter header table and pipeline record.
    alignas(8) std::uint8_t console_fields[0x600];
    std::atomic<unsigned long> refs{0};
    unsigned pc_slot_count = 0;  // slots the real list implements; a mapped slot past it is unsupported
    // The first console slot recorded since the last Reset that was not executed (0: none).
    std::atomic<unsigned> unsupported_slot{0};
    // PALMWAVE_EXPERIMENTAL_XBOX_FLUSH was exactly "1" when this list was created (see list_experimental_flush).
    bool experimental_flush = false;
    // The base GUID is shared with PC, but denotes the console table only when requested at Xbox factory slot 73.
    bool shared_console_base_iid = false;

    // The cursor and both limits point at +0x580, inside the body but past every measured field (the pipeline record
    // ends at +0x554), so even a writer that ignored the limit would not reach the header table or the record.
    void pin_cursor_at_limit() {
        const auto at_limit = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this) + 0x580);
        for (const std::size_t offset : {0x18u, 0x28u, 0x30u})
            std::memcpy(reinterpret_cast<std::uint8_t*>(this) + offset, &at_limit, sizeof(at_limit));
    }
};
static_assert(offsetof(ListProxy, console_fields) == 0x18, "the console body starts at +0x18");
static_assert(0x18 + sizeof(ListProxy::console_fields) >= 0x580 + 0x80,
              "the body covers the pipeline record and leaves slack past the pinned cursor");

struct ListStats {
    static inline std::atomic<std::uint64_t> hits[kListSlots];
    static inline std::atomic<bool> first[kListSlots];
    static inline std::atomic<bool> unsupported_warned[kListSlots];
};

// Fixed names for the trace channel, built once: XboxID3D12GraphicsCommandList::Close, ...::Slot27.
const char* list_trace_name(unsigned slot) {
    static const auto names = [] {
        static char storage[kListSlots][64];
        for (unsigned s = 0; s < kListSlots; ++s) {
            const char* method = s == kConsoleListClose          ? "Close"
                                 : s == kPcListReset             ? "Reset"
                                 : s == kConsoleListResetX       ? "ResetX"
                                 : s == kConsoleListBarrier      ? "Barrier"
                                 : s == kConsoleAllocateUpload   ? "AllocateUploadMemory"
                                 : s == kConsoleExecuteBundle    ? "ExecuteBundle"
                                 : s == kConsoleSetDescriptorHeaps ? "SetDescriptorHeaps"
                                 : s == kConsoleOMSetRenderTargets ? "OMSetRenderTargets"
                                 : s == kConsoleClearDepthStencilView ? "ClearDepthStencilView"
                                 : s == kConsoleClearRenderTargetView ? "ClearRenderTargetView"
                                 : s == kConsoleClearUavUint     ? "ClearUnorderedAccessViewUint"
                                 : s == kConsoleClearUavFloat    ? "ClearUnorderedAccessViewFloat"
                                                                   : nullptr;
            if (method)
                std::snprintf(storage[s], sizeof(storage[s]), "XboxID3D12GraphicsCommandList::%s", method);
            else
                std::snprintf(storage[s], sizeof(storage[s]), "XboxID3D12GraphicsCommandList::Slot%u", s);
        }
        return &storage;
    }();
    return (*names)[slot < kListSlots ? slot : 0];
}

void note_list_slot(unsigned slot, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4) {
    if (slot >= kListSlots) return;
    count_hit(ListStats::hits[slot]);
    const unsigned pc = console_to_pc_list_slot(slot);
    if (first_call(ListStats::first[slot]) || (slot >= 3 && trace_enabled())) {
        char target[32] = "console-only";
        if (pc != kListNotForwarded) std::snprintf(target, sizeof(target), "PC slot %u", pc);
        std::fprintf(stderr, "[rt] d3d12_xs: list slot %u (%s) call (args 0x%llx 0x%llx 0x%llx 0x%llx)\n", slot,
                     slot == kConsoleAllocateUpload ? "AllocateUploadMemory" : target,
                     static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                     static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4));
    }
}

// A console call the proxy does not execute: logged once per slot, traced every time, and the list is marked.
HRESULT list_unsupported(ListProxy* self, unsigned slot, unsigned pc) {
    unsigned none = 0;
    self->unsupported_slot.compare_exchange_strong(none, slot, std::memory_order_relaxed);
    if (slot < kListSlots && first_call(ListStats::unsupported_warned[slot])) {
        char reason[96];
        if (pc == kListNotForwarded)
            std::snprintf(reason, sizeof(reason), "is not modelled");
        else if (pc >= self->pc_slot_count)
            std::snprintf(reason, sizeof(reason), "maps to PC slot %u, past the real list's %u slots", pc,
                          self->pc_slot_count);
        else
            std::snprintf(reason, sizeof(reason),
                          "could not pass its input on (incomplete bundle, no memory or invalid CPU descriptor)");
        std::fprintf(stderr,
                     "[rt] d3d12_xs: list slot %u %s; not executed (E_NOTIMPL), the list's Close fails and its "
                     "submission is refused until Reset\n",
                     slot, reason);
    }
    d3d12_trace::record(list_trace_name(slot), "unsupported");
    return E_NOTIMPL;
}

// ---- EXPERIMENTAL, opt-in: console list slots 131/132 as a global barrier (a hypothesis, not a known contract) ----
//
// Off by default. Only with PALMWAVE_EXPERIMENTAL_XBOX_FLUSH set to exactly "1" when the list is created, and only for
// the four exact argument tuples below (all four 64-bit words compared; how many bits the console method consumes
// is unproved), the proxy records ONE real enhanced barrier on the backend list through ID3D12GraphicsCommandList7:
// a GLOBAL group with SyncBefore = SyncAfter = SYNC_ALL and AccessBefore = AccessAfter = ACCESS_COMMON. Anything
// else (flag off or another value, another slot, any changed word, no GCL7) takes list_unsupported unchanged.
//
// Why a barrier: the low word of the 131 tuples matches AMD PAL's public Gfx10AcquireMemGcrCntl layout (0xc3c0 =
// glkWb|glkInv|glvInv|gl1Inv|gl2Inv|gl2Wb; 0xc380 the same without glkWb), and the last word resembles a 256-byte-
// aligned full-range size. Nothing public maps 132's 0x700000/0x100000. This does NOT prove the slots only
// synchronize: a register write, query, memory write or resource transition they might perform is not emulated, and
// the barrier can be stronger or weaker than the console's operation. The result is traced "experimental-hypothesis",
// never "ok". See docs/BLOCKERS.md and docs/d3d12_command_objects.md §5.2.
constexpr unsigned kConsoleListExperiment131 = 131;
constexpr unsigned kConsoleListExperiment132 = 132;

struct ExperimentalFlushTuple {
    unsigned slot;
    std::uint64_t words[4];
};

// Observed locally (first two) and in upstream PR #7076's public loop measurement (last two). A statically found
// third 131 form is deliberately absent until it is seen at run time.
constexpr ExperimentalFlushTuple kExperimentalFlushTuples[] = {
    {kConsoleListExperiment132, {0x700000, 0, 0, 0x0000ffffffffff00ull}},
    {kConsoleListExperiment131, {0xc3c0, 0x80020000, 0, 0x0000ffffffffff00ull}},
    {kConsoleListExperiment132, {0x100000, 0, 0, 0x0000ffffffffff00ull}},
    {kConsoleListExperiment131, {0xc380, 0x20000, 0, 0x0000ffffffffff00ull}},
};

// Exactly "1" enables; unset, empty, "0", "true", "1 " or anything longer disables. Read when a list is created.
bool experimental_xbox_flush_requested() {
    char value[4] = {};
    const DWORD length = GetEnvironmentVariableA("PALMWAVE_EXPERIMENTAL_XBOX_FLUSH", value, sizeof(value));
    return length == 1 && value[0] == '1';
}

bool experimental_flush_tuple(unsigned slot, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                              std::uint64_t a4) {
    for (const ExperimentalFlushTuple& t : kExperimentalFlushTuples)
        if (t.slot == slot && t.words[0] == a1 && t.words[1] == a2 && t.words[2] == a3 && t.words[3] == a4)
            return true;
    return false;
}

// True when the global barrier was handed to the backend list; false sends the caller to list_unsupported. The list's
// unsupported mark is never read or cleared here, so a list poisoned earlier stays poisoned. Barrier is void: a
// backend that cannot record it marks its own list, and the proxy's Close returns that failure.
bool list_experimental_flush(ListProxy* self, unsigned slot, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                             std::uint64_t a4) {
    if (!self->experimental_flush || !experimental_flush_tuple(slot, a1, a2, a3, a4)) return false;
    ID3D12GraphicsCommandList7* gcl7 = nullptr;
    const HRESULT hr =
        self->real->QueryInterface(__uuidof(ID3D12GraphicsCommandList7), reinterpret_cast<void**>(&gcl7));
    if (FAILED(hr) || !gcl7) {
        if (gcl7) gcl7->Release();  // a non-conforming failure that still handed out a pointer
        static std::atomic<bool> warned{false};
        if (first_call(warned))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: EXPERIMENTAL list slot %u: the backend list has no "
                         "ID3D12GraphicsCommandList7 (0x%08lx); no barrier recorded, the slot stays unsupported\n",
                         slot, static_cast<unsigned long>(FAILED(hr) ? hr : E_NOINTERFACE));
        return false;
    }
    D3D12_GLOBAL_BARRIER global{};
    global.SyncBefore = D3D12_BARRIER_SYNC_ALL;
    global.SyncAfter = D3D12_BARRIER_SYNC_ALL;
    global.AccessBefore = D3D12_BARRIER_ACCESS_COMMON;
    global.AccessAfter = D3D12_BARRIER_ACCESS_COMMON;
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_GLOBAL;
    group.NumBarriers = 1;
    group.pGlobalBarriers = &global;
    gcl7->Barrier(1, &group);  // on the pointer QueryInterface returned, which may differ from self->real
    gcl7->Release();
    static std::atomic<bool> first_use[2];
    if (first_call(first_use[slot == kConsoleListExperiment132 ? 1 : 0]))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: *** EXPERIMENTAL HYPOTHESIS *** list slot %u (args 0x%llx 0x%llx 0x%llx 0x%llx) "
                     "recorded as a global SYNC_ALL/ACCESS_COMMON barrier because PALMWAVE_EXPERIMENTAL_XBOX_FLUSH=1. "
                     "Not a known Xbox contract; any non-synchronization effect of the console method is not "
                     "emulated. Results from this run are experimental.\n",
                     slot, static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                     static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4));
    d3d12_trace::record(list_trace_name(slot), "experimental-hypothesis");
    return true;
}

// Only this base chain shares the console vtable layout. PC graphics-command-list IIDs must never expose the
// shifted console table; callers obtain real PC lists through device slot 12.
bool is_list_base_iid(const IID& iid) {
    return IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
           IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12CommandList));
}

unsigned long list_add_ref(ListProxy* self) { return self->refs.fetch_add(1, std::memory_order_relaxed) + 1; }

void destroy_list(ListProxy* self) {
    IUnknown* real = self->real;
    DeviceProxy* device = self->device;
    delete self;
    if (real) real->Release();
    if (device) proxy_release(device);
}

unsigned long list_release(ListProxy* self) {
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) destroy_list(self);
    return refs;
}

HRESULT list_query_interface(ListProxy* self, const IID* riid, void** out) {
    note_list_slot(0, reinterpret_cast<std::uint64_t>(riid), reinterpret_cast<std::uint64_t>(out), 0, 0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    // A known console IID selects its required PC capability, not a PC vtable. A shared base GUID is accepted
    // only on objects created with that GUID by Xbox slot 73, including QI for their own creation interface.
    bool mapped = false;
    const IID& pc_iid = command_list_iid_for_pc(*riid, &mapped);
    const bool shared_base = self->shared_console_base_iid && IsEqualIID(*riid, __uuidof(ID3D12GraphicsCommandList));
    if (!mapped && !shared_base && !is_list_base_iid(pc_iid)) {
        if (supports_iid(self->real, pc_iid)) log_refused_iid("list", *riid);
        return E_NOINTERFACE;
    }
    if (!supports_iid(self->real, pc_iid)) return E_NOINTERFACE;
    *out = self;
    list_add_ref(self);
    return S_OK;
}

// As child_get_device: the Xbox-ABI device; with no output, the real list's own answer.
HRESULT list_get_device(ListProxy* self, const IID* riid, void** out) {
    note_list_slot(7, reinterpret_cast<std::uint64_t>(riid), reinterpret_cast<std::uint64_t>(out), 0, 0);
    if (out) *out = nullptr;
    if (!riid) return E_POINTER;
    if (!out) {
        using Fn = HRESULT(WINAPI*)(IUnknown*, const IID*, void**);
        return reinterpret_cast<Fn>((*reinterpret_cast<void***>(self->real))[7])(self->real, riid, nullptr);
    }
    return proxy_query_interface(self->device, riid, out);
}

// Cumulative slot count of the highest command-list interface the real list implements (Windows SDK d3d12.h).
unsigned detect_list_slot_count(IUnknown* real) {
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList8))) return 82;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList7))) return 81;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList6))) return 80;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList5))) return 79;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList4))) return 77;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList3))) return 68;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList2))) return 67;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList1))) return 66;
    return 60;
}

// A list's CPU descriptor handle, translated as device slots 17..24 do. False for an invalid one: the caller marks the
// list (list_unsupported) instead of recording a command with a wrong descriptor.
bool list_handle(ListProxy* self, SIZE_T ptr, std::uint64_t count, D3D12_DESCRIPTOR_HEAP_TYPE type,
                 D3D12_CPU_DESCRIPTOR_HANDLE& out) {
    return translate_handle(self->device, ptr, count, type, out.ptr) != DescriptorHandleTranslation::invalid;
}

using ListForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                             std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                             std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                             std::uint64_t, std::uint64_t);

template <unsigned Slot>
std::uint64_t WINAPI list_slot(ListProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                               std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                               std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                               std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        return hr_to_u64(list_query_interface(self, reinterpret_cast<const IID*>(a1), reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return list_add_ref(self);
    } else if constexpr (Slot == 2) {
        return list_release(self);
    } else if constexpr (Slot == 7) {
        return hr_to_u64(list_get_device(self, reinterpret_cast<const IID*>(a1), reinterpret_cast<void**>(a2)));
    } else {
        note_list_slot(Slot, a1, a2, a3, a4);
        constexpr unsigned pc = console_to_pc_list_slot(Slot);
        if constexpr (Slot == kConsoleAllocateUpload) {
            const std::uint64_t memory = upload_alloc(self->device, a1, a2);  // (size, alignment) -> pointer or 0
            if (!memory) d3d12_trace::record(list_trace_name(Slot), "failed");
            return memory;
        } else if constexpr (pc == kListNotForwarded) {
            if constexpr (Slot == kConsoleListExperiment131 || Slot == kConsoleListExperiment132) {
                // Opt-in experiment (above). The return register of this console method is unmeasured: 0 is passed
                // back with no claimed contract.
                if (list_experimental_flush(self, Slot, a1, a2, a3, a4)) return 0;
            }
            return hr_to_u64(list_unsupported(self, Slot, pc));
        } else {
            if (pc >= self->pc_slot_count) return hr_to_u64(list_unsupported(self, Slot, pc));
            const auto fn = reinterpret_cast<ListForwardFn>((*reinterpret_cast<void***>(self->real))[pc]);
            if constexpr (Slot == kConsoleListClose) {
                // The real list is closed either way, so a later Reset works; a list that recorded an unexecuted
                // command reports it here, the way a PC list reports a recording error.
                const std::uint64_t closed = fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13,
                                                a14);
                if (FAILED(static_cast<HRESULT>(static_cast<std::uint32_t>(closed)))) return closed;
                if (!self->unsupported_slot.load(std::memory_order_relaxed)) return closed;
                d3d12_trace::record(list_trace_name(Slot), "unsupported");
                return hr_to_u64(E_NOTIMPL);
            } else if constexpr (pc == kPcListReset) {
                // Reset(allocator, pso), console 10 and 139: the allocator proxy unwraps to the real allocator (a
                // wrapped pipeline state would too). A successful Reset starts a clean recording.
                const std::uint64_t reset = fn(self->real, unwrap_object(a1), unwrap_object(a2), a3, a4, a5, a6, a7,
                                               a8, a9, a10, a11, a12, a13, a14);
                if (SUCCEEDED(static_cast<HRESULT>(static_cast<std::uint32_t>(reset))))
                    self->unsupported_slot.store(0, std::memory_order_relaxed);
                return reset;
            } else if constexpr (Slot == kConsoleExecuteBundle) {
                // A bundle that recorded an unexecuted command makes this list incomplete too.
                if (list_unsupported_slot(reinterpret_cast<const void*>(a1)))
                    return hr_to_u64(list_unsupported(self, Slot, pc));
                return fn(self->real, unwrap_object(a1), a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
            } else if constexpr (Slot == kConsoleSetDescriptorHeaps) {
                UnwrappedArray heaps;
                if (!heaps.fill(a2, a1)) return hr_to_u64(list_unsupported(self, Slot, pc));  // out of memory
                return fn(self->real, a1, heaps.data() ? heaps.data() : a2, a3, a4, a5, a6, a7, a8, a9, a10, a11,
                          a12, a13, a14);
            } else if constexpr (Slot >= 41 && Slot <= 46) {
                // The six root-descriptor setters (index, GPU VA): an arena address becomes its GPU address.
                return fn(self->real, a1, upload_translate(self->device, a2), a3, a4, a5, a6, a7, a8, a9, a10, a11,
                          a12, a13, a14);
            } else if constexpr (Slot == 47) {
                // IASetIndexBuffer(view): translated in a copy, the title's struct is not written.
                if (!a1) return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
                D3D12_INDEX_BUFFER_VIEW view = *reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(a1);
                view.BufferLocation = upload_translate(self->device, view.BufferLocation);
                return fn(self->real, reinterpret_cast<std::uint64_t>(&view), a2, a3, a4, a5, a6, a7, a8, a9, a10,
                          a11, a12, a13, a14);
            } else if constexpr (Slot == 48) {
                // IASetVertexBuffers(start, count, views): up to the 32 input slots D3D12 has, translated in a copy.
                const auto count = static_cast<std::uint32_t>(a2);
                if (!a3 || !count || count > D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT)
                    return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
                D3D12_VERTEX_BUFFER_VIEW views[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
                std::memcpy(views, reinterpret_cast<const void*>(a3), count * sizeof(views[0]));
                for (std::uint32_t i = 0; i < count; ++i)
                    views[i].BufferLocation = upload_translate(self->device, views[i].BufferLocation);
                return fn(self->real, a1, a2, reinterpret_cast<std::uint64_t>(views), a4, a5, a6, a7, a8, a9, a10,
                          a11, a12, a13, a14);
            } else if constexpr (Slot == 49) {
                // SOSetTargets(start, count, views): both addresses of each view, up to the 4 stream-output slots.
                const auto count = static_cast<std::uint32_t>(a2);
                if (!a3 || !count || count > D3D12_SO_BUFFER_SLOT_COUNT)
                    return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
                D3D12_STREAM_OUTPUT_BUFFER_VIEW views[D3D12_SO_BUFFER_SLOT_COUNT];
                std::memcpy(views, reinterpret_cast<const void*>(a3), count * sizeof(views[0]));
                for (std::uint32_t i = 0; i < count; ++i) {
                    views[i].BufferLocation = upload_translate(self->device, views[i].BufferLocation);
                    views[i].BufferFilledSizeLocation =
                        upload_translate(self->device, views[i].BufferFilledSizeLocation);
                }
                return fn(self->real, a1, a2, reinterpret_cast<std::uint64_t>(views), a4, a5, a6, a7, a8, a9, a10,
                          a11, a12, a13, a14);
            } else if constexpr (Slot == kConsoleOMSetRenderTargets) {
                // OMSetRenderTargets(count, rtvs, singleRange, dsv), PR #7880 adapted: translated into copies, the
                // title's arrays are not written. A single range reads only rtvs[0] and checks all `count`
                // descriptors of its span; more than 8 targets, or any invalid handle, is not forwarded.
                const auto count = static_cast<std::uint32_t>(a1);
                const bool single_range = static_cast<std::uint32_t>(a3) != 0;
                if (count > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
                    return hr_to_u64(list_unsupported(self, Slot, pc));
                D3D12_CPU_DESCRIPTOR_HANDLE rtvs[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
                std::uint64_t rtv_word = a2;
                if (a2 && count) {
                    const auto* in = reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(a2);
                    const std::uint32_t entries = single_range ? 1 : count;
                    for (std::uint32_t i = 0; i < entries; ++i)
                        if (!list_handle(self, in[i].ptr, single_range ? count : 1, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                                         rtvs[i]))
                            return hr_to_u64(list_unsupported(self, Slot, pc));
                    rtv_word = reinterpret_cast<std::uint64_t>(rtvs);
                }
                D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
                std::uint64_t dsv_word = a4;
                if (a4) {
                    if (!list_handle(self, reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(a4)->ptr, 1,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_DSV, dsv))
                        return hr_to_u64(list_unsupported(self, Slot, pc));
                    dsv_word = reinterpret_cast<std::uint64_t>(&dsv);
                }
                return fn(self->real, a1, rtv_word, a3, dsv_word, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
            } else if constexpr (Slot == kConsoleClearRenderTargetView) {
                // ClearRenderTargetView(rtv, color, rectCount, rects): the color is a pointer, so the integer
                // thunk is exact here.
                D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
                if (!list_handle(self, static_cast<SIZE_T>(a1), 1, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtv))
                    return hr_to_u64(list_unsupported(self, Slot, pc));
                return fn(self->real, rtv.ptr, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
            } else if constexpr (Slot == kConsoleClearUavUint || Slot == kConsoleClearUavFloat) {
                // ClearUnorderedAccessView*(gpuHandle, cpuHandle, resource, values, rectCount, rects): only the
                // CPU handle is translated; the genuine GPU handle (from heap slot 10) goes on unchanged.
                D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
                if (!list_handle(self, static_cast<SIZE_T>(a2), 1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, cpu))
                    return hr_to_u64(list_unsupported(self, Slot, pc));
                return fn(self->real, a1, cpu.ptr, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
            } else {
                return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
            }
        }
    }
}

// Console slot 51 -> PC 47, ClearDepthStencilView(dsv, flags, FLOAT depth, UINT8 stencil, rectCount, rects). Depth is
// the fourth argument, so x64 passes it in XMM3, not R9: the integer list_slot thunk would forward R9 and leave Depth to
// chance. This typed thunk (adapted from upstream PR #7880) receives and forwards every argument with its real type.
void WINAPI list_clear_depth_stencil_view(ListProxy* self, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags,
                                          FLOAT depth, UINT8 stencil, UINT rect_count, const D3D12_RECT* rects) {
    std::uint32_t depth_bits = 0;
    std::memcpy(&depth_bits, &depth, sizeof(depth_bits));
    note_list_slot(kConsoleClearDepthStencilView, dsv.ptr, static_cast<std::uint64_t>(flags), depth_bits, stencil);
    constexpr unsigned pc = console_to_pc_list_slot(kConsoleClearDepthStencilView);
    static_assert(pc == 47, "console 51 is PC ClearDepthStencilView");
    D3D12_CPU_DESCRIPTOR_HANDLE translated{};
    if (pc >= self->pc_slot_count ||
        !list_handle(self, dsv.ptr, 1, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, translated)) {
        list_unsupported(self, kConsoleClearDepthStencilView, pc);  // void method: the list carries the failure
        return;
    }
    using ClearDsvFn = void(WINAPI*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT,
                                     UINT8, UINT, const D3D12_RECT*);
    reinterpret_cast<ClearDsvFn>((*reinterpret_cast<void***>(self->real))[pc])(self->real, translated, flags, depth,
                                                                               stencil, rect_count, rects);
}

template <std::size_t... I>
void fill_list_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&list_slot<static_cast<unsigned>(I)>)), ...);
    vtable[kConsoleClearDepthStencilView] = reinterpret_cast<void*>(&list_clear_depth_stencil_view);
}

void** get_list_vtable() {
    static void** vtable = [] {
        static void* storage[kListSlots];
        fill_list_vtable(storage, std::make_index_sequence<kListSlots>{});
        return storage;
    }();
    return vtable;
}

bool is_list_proxy_impl(const void* list) {
    return list && *reinterpret_cast<void* const*>(list) == reinterpret_cast<void*>(get_list_vtable());
}

IUnknown* list_real_or_null(const void* object) {
    return is_list_proxy_impl(object) ? static_cast<const ListProxy*>(object)->real : nullptr;
}

unsigned list_unsupported_slot(const void* object) {
    return is_list_proxy_impl(object)
               ? static_cast<const ListProxy*>(object)->unsupported_slot.load(std::memory_order_relaxed)
               : 0;
}

// Wraps `created` (one reference, owned by the proxy on success, released on failure) and returns `riid` from the
// proxy through `out`.
HRESULT wrap_list(IUnknown* created, DeviceProxy* device, const IID* riid, void** out, bool shared_console_base_iid) {
    if (!created) return E_FAIL;
    ListProxy* proxy = new (std::nothrow) ListProxy();
    if (!proxy) {
        created->Release();
        return E_OUTOFMEMORY;
    }
    proxy->vtable = get_list_vtable();
    proxy->real = static_cast<ID3D12GraphicsCommandList*>(created);
    proxy->device = device;
    std::memset(proxy->console_fields, 0, sizeof(proxy->console_fields));
    proxy->pin_cursor_at_limit();
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy->pc_slot_count = detect_list_slot_count(created);
    proxy->shared_console_base_iid = shared_console_base_iid;
    proxy->experimental_flush = experimental_xbox_flush_requested();
    if (proxy->experimental_flush) {
        static std::atomic<bool> announced{false};
        if (first_call(announced))
            std::fprintf(stderr, "[rt] d3d12_xs: *** EXPERIMENTAL *** PALMWAVE_EXPERIMENTAL_XBOX_FLUSH=1: console list "
                                 "slots 131/132 with four exact argument tuples become a global barrier (hypothesis "
                                 "only, not a compatibility claim)\n");
    }
    proxy_add_ref(device);  // the list keeps its Xbox-ABI device alive for GetDevice and the upload arena

    const HRESULT hr = list_query_interface(proxy, riid, out);
    if (FAILED(hr)) {
        destroy_list(proxy);  // no reference escaped
        return hr;
    }
    static std::atomic<bool> logged{false};
    if (!d3d12_trace::enabled() && (!logged.exchange(true, std::memory_order_relaxed) || trace_enabled()))
        std::fprintf(stderr, "[rt] d3d12_xs: list proxy %p wraps %p (device proxy %p, %u PC list slots)\n",
                     static_cast<void*>(proxy), static_cast<void*>(created), static_cast<void*>(device),
                     proxy->pc_slot_count);
    proxy->refs.fetch_sub(1, std::memory_order_relaxed);  // drop the creation reference; `out` holds the caller's
    return S_OK;
}

// Calls the real device's slot `Slot` with these five words in the positions the thunk received them (see "A thunk
// declares 14 argument words" at the top: no word may move to another stack position on the way).
template <unsigned Slot>
std::uint64_t forward_device(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                             std::uint64_t a4, std::uint64_t a5) {
    using Fn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t);
    return reinterpret_cast<Fn>(self->real_vtable[Slot])(self->real, a1, a2, a3, a4, a5);
}

// A device slot past the PC device's vtable (the real device predates the interface): E_NOTIMPL, logged once per
// slot. The generic thunk and the slots the proxy handles itself (31/34/35/45/46/50, 75) log the same line.
HRESULT refuse_beyond_device(unsigned slot, unsigned slot_count) {
    bool expected = false;
    if (slot < kMaxSlots && g_beyond_warned[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: device slot %u (%s) is beyond the PC device vtable (%u slots); returning "
                     "E_NOTIMPL\n",
                     slot, slot_name(slot), slot_count);
    return E_NOTIMPL;
}

// The console placement ABI (va, desc, state, clear, riid, out) is recorded in upstream #7218
// (tibzejoker, a3eae955dcec4fb65864ca6c10b4ca3f2999c2d3, GPL-3.0). A 128-entry table did not contain slot 133,
// so the caller could jump through an absent entry. Keep a callable thunk, clear the output, and refuse: this does
// not import pages or interpret console resource flags / the initial-state word. The pointed-to inputs are not read.
HRESULT proxy_refuse_placed_resource_x(std::uint64_t va, std::uint64_t desc, std::uint64_t state,
                                       std::uint64_t clear, void** out) {
    note_slot(133);
    if (out) *out = nullptr;
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed) || trace_enabled())
        std::fprintf(stderr,
                     "[rt] d3d12_xs: device slot 133 CreatePlacedResourceX refused: a host-page placement handler is not "
                     "available on this device; console flags/state are not translated (va=%p desc=%p state=0x%08x clear=%p)\n",
                     reinterpret_cast<void*>(va), reinterpret_cast<void*>(desc), static_cast<unsigned>(state),
                     reinterpret_cast<void*>(clear));
    return d3d12_trace::result("Xbox CreatePlacedResourceX (device slot 133)", E_NOTIMPL);
}

// Device slot 8: CreateCommandQueue(desc, riid, out). Without an output it is the device's own capability answer
// (S_FALSE for a valid description on the PC), as #558 does for slot 14.
HRESULT proxy_create_command_queue(DeviceProxy* self, const D3D12_COMMAND_QUEUE_DESC* desc, const IID* riid,
                                   void** out) {
    note_slot(8);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure, as slots 14/62/76
    if (!riid) return E_POINTER;
    // The caller's null output goes to the real device on purpose: its answer (S_FALSE, E_UNEXPECTED, ...) is the
    // one to give, although the SDK annotates the parameter as required.
#pragma warning(suppress : 6387)
    if (!out) return self->real->CreateCommandQueue(desc, *riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateCommandQueue(desc, __uuidof(ID3D12CommandQueue), &created);
    if (FAILED(hr)) return hr;
    return wrap_child<ChildKind::queue>(static_cast<IUnknown*>(created), self, riid, out);
}

// Xbox queue route (default slot 48). Adapted from upstream #7037 (GPL-3.0): only
// field +8 values 0..3 have been observed. Their console engine/affinity meaning is
// unverified, so this bounded compatibility mapping requests ordinary host queues.
// In particular, console value 1 is not permission to disable the host GPU timeout.
// Keep PC slot 8 strict and never silently discard an unobserved bit. See docs/BLOCKERS.md.
HRESULT proxy_create_command_queue_x(DeviceProxy* self, const D3D12_COMMAND_QUEUE_DESC* desc, const IID* riid,
                                     void** out) {
    if (out) *out = nullptr;
    if (!riid) return E_POINTER;
    if (!desc) return E_INVALIDARG;
    const auto value = static_cast<std::uint32_t>(desc->Flags);
    if (value & ~0x3u) {
        std::fprintf(stderr, "[rt] d3d12_xs: CreateCommandQueueX refused unsupported field+8=0x%x\n", value);
        return E_INVALIDARG;
    }
    D3D12_COMMAND_QUEUE_DESC pc_desc = *desc;
    pc_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    static std::atomic<std::uint32_t> logged{0};
    const std::uint32_t bit = 1u << value;
    if ((logged.fetch_or(bit, std::memory_order_relaxed) & bit) == 0)
        std::fprintf(stderr, "[rt] d3d12_xs: CreateCommandQueueX compatibility: field+8=0x%x -> host Flags=NONE; "
                             "console engine/affinity semantics unverified\n", value);
    return proxy_create_command_queue(self, &pc_desc, riid, out);
}

// Xbox device slot 78 (public HLE): SetFrameIntervalX(output, interval_us, latency, flags). The output and flags are
// not used before a presenter exists. The period is a vblank period in microseconds in the public samples (16667 for
// 60 Hz), and latency is the allowed frames in flight. S_OK even for 0: a boot must not abort here, and nothing paces.
HRESULT proxy_set_frame_interval_x(DeviceProxy* self, std::uint64_t output, std::uint64_t interval_us,
                                   std::uint64_t latency, std::uint64_t flags) {
    (void)output;
    (void)flags;
    self->frame_interval_us.store(static_cast<std::uint32_t>(interval_us), std::memory_order_relaxed);
    self->frame_latency.store(static_cast<std::uint32_t>(latency ? latency : 1), std::memory_order_relaxed);
    return S_OK;
}

// Xbox device slot 79 (public HLE): ScheduleFrameEventX(event, offset_us, pEvent, flags); our pacer has only the origin.
HRESULT proxy_schedule_frame_event_x(DeviceProxy* self, std::uint64_t event, std::uint64_t offset_us,
                                     std::uint64_t flags) {
    (void)event;
    (void)offset_us;
    (void)flags;
    self->frame_event_scheduled.store(true, std::memory_order_relaxed);
    return S_OK;
}

// Xbox device slot 80 (public HLE): WaitFrameEventX(event, timeout_ms, pEvent, flags, &token). It returns at once with
// a fresh non-zero token (the first-frame choice): there is no vblank source behind the device before a presenter, and
// a zero wait cannot stall the title. A null token pointer is E_POINTER, as a _Out_ parameter.
HRESULT proxy_wait_frame_event_x(DeviceProxy* self, std::uint64_t event, std::uint64_t timeout_ms,
                                 std::uint64_t flags, std::uint64_t token_word) {
    (void)event;
    (void)timeout_ms;
    (void)flags;
    auto* token = reinterpret_cast<std::uint64_t*>(token_word);
    if (!token) return E_POINTER;
    std::uint64_t value = self->frame_next_token.fetch_add(1, std::memory_order_relaxed);
    if (value == 0) value = self->frame_next_token.fetch_add(1, std::memory_order_relaxed);  // skip the null token
    *token = value;
    return S_OK;
}

// Xbox device slot 73 (public HLE): CreateCommandListX(desc, allocator, initial_state, riid, out). The console call is
// not the PC six-argument form: the type is the first word of D3D12XBOX_COMMAND_LIST_DESC and there is no node mask
// (the HLE passes 0). The second desc word is logged, not interpreted: its public layout is not established. The real
// list is created as ID3D12GraphicsCommandList and handed back behind the console list proxy; only the measured
// console IID, the shared base GUID when requested here, and layout-identical base interfaces are answered by the
// proxy. The shared GUID requires only the backend's base graphics-list interface. Only the direct, bundle and
// compute types get the graphics-shaped console table. Any other type word is refused with E_NOTIMPL instead of being
// coerced to DIRECT, as the HLE does: a sibling layer reports a separate console DMA-list vtable (Close 9, Reset 10,
// copies at 11-13; its type is derived as 6, docs/d3d12_command_objects.md §5.3), which is not modelled, and the
// graphics table would send those copies to ClearState/Draw*.
HRESULT proxy_create_command_list_x(DeviceProxy* self, std::uint64_t desc_word, std::uint64_t allocator_word,
                                    std::uint64_t initial_state_word, const IID* riid, void** out) {
    constexpr auto method = "XboxID3D12Device::CreateCommandListX";
    if (out) *out = nullptr;
    if (!desc_word || !allocator_word) return d3d12_trace::result(method, E_INVALIDARG);
    if (!riid) return d3d12_trace::result(method, E_POINTER);
    if (!out) return d3d12_trace::result(method, E_POINTER);

    std::uint32_t words[2] = {};
    std::memcpy(words, reinterpret_cast<const void*>(desc_word), sizeof(words));
    const auto type = static_cast<D3D12_COMMAND_LIST_TYPE>(words[0]);

    static std::atomic<bool> logged{false};
    if (first_call(logged))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 73 CreateCommandListX desc words 0x%08x 0x%08x -> node mask 0, type %d\n",
                     words[0], words[1], static_cast<int>(type));
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_BUNDLE &&
        type != D3D12_COMMAND_LIST_TYPE_COMPUTE) {
        static std::atomic<bool> refused[8];
        if (first_call(refused[words[0] < 7 ? words[0] : 7]))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: slot 73 CreateCommandListX of type %u refused (E_NOTIMPL): only the "
                         "direct/bundle/compute console list is modelled (desc words 0x%08x 0x%08x)\n",
                         words[0], words[0], words[1]);
        return d3d12_trace::result(method, E_NOTIMPL);
    }

    // Palmwave wraps its Vulkan allocators; a PC allocator comes back unwrapped and is passed as it is. Pipeline states
    // are not wrapped by this proxy, so initial_state is already the real object (a proxy there is passed through too).
    auto* allocator = allocator_proxy_real_allocator(reinterpret_cast<void*>(allocator_word));
    if (!allocator) allocator = reinterpret_cast<ID3D12CommandAllocator*>(allocator_word);
    auto* initial_state = reinterpret_cast<ID3D12PipelineState*>(initial_state_word);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateCommandList(0, type, allocator, initial_state,
                                                     __uuidof(ID3D12GraphicsCommandList), &created);
    if (FAILED(hr)) return d3d12_trace::result(method, hr);
    const bool shared_base = IsEqualIID(*riid, __uuidof(ID3D12GraphicsCommandList));
    return d3d12_trace::result(method, wrap_list(static_cast<IUnknown*>(created), self, riid, out, shared_base));
}

// Device slot 75: ID3D12Device9::CreateCommandQueue1(desc, creator_id, riid, out), called by slot index.
HRESULT proxy_create_command_queue1(DeviceProxy* self, std::uint64_t desc, std::uint64_t creator_id,
                                    const IID* riid, void** out) {
    note_slot(75);
    if (out) *out = nullptr;  // as slot 8
    if (self->slot_count <= 75) return refuse_beyond_device(75, self->slot_count);  // predates ID3D12Device9
    if (!riid) return E_POINTER;
    using Fn = HRESULT(WINAPI*)(ID3D12Device*, std::uint64_t, std::uint64_t, const IID*, void**);
    const auto create = reinterpret_cast<Fn>(self->real_vtable[75]);
    if (!out) return create(self->real, desc, creator_id, riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = create(self->real, desc, creator_id, &__uuidof(ID3D12CommandQueue), &created);
    if (FAILED(hr)) return hr;
    return wrap_child<ChildKind::queue>(static_cast<IUnknown*>(created), self, riid, out);
}

// Device slot 36: CreateFence(initial_value, flags, riid, out).
HRESULT proxy_create_fence(DeviceProxy* self, std::uint64_t initial_value, std::uint64_t flags, const IID* riid,
                           void** out) {
    note_slot(36);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    const auto fence_flags = static_cast<D3D12_FENCE_FLAGS>(static_cast<std::uint32_t>(flags));
#pragma warning(suppress : 6387)  // the caller's null output, on purpose (see slot 8)
    if (!out) return self->real->CreateFence(initial_value, fence_flags, *riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateFence(initial_value, fence_flags, __uuidof(ID3D12Fence), &created);
    if (FAILED(hr)) return hr;
    return wrap_child<ChildKind::fence>(static_cast<IUnknown*>(created), self, riid, out);
}

// True when `allocator` is one whose consumers resolve allocator proxies: Palmwave's own Vulkan-layer allocator,
// whose lists (CreateCommandList, Reset) find the real allocator through allocator_proxy_real_allocator. A PC
// runtime allocator does not answer the tag, and its lists would take a proxy for their own allocator.
bool resolves_allocator_proxies(IUnknown* allocator) {
    void* found = nullptr;
    if (FAILED(allocator->QueryInterface(kPalmwaveCommandAllocatorTag, &found)) || !found) return false;
    static_cast<IUnknown*>(found)->Release();  // the implementation pointer starts with its IUnknown chain
    return true;
}

// Device slot 9: CreateCommandAllocator(type, riid, out). An allocator proxy where the list side resolves it (see
// resolves_allocator_proxies); any other allocator goes back unchanged, logged once, so the host runtime's lists
// never see a proxy. Null output and null IID as slot 8.
HRESULT proxy_create_command_allocator(DeviceProxy* self, std::uint64_t type, const IID* riid, void** out) {
    note_slot(9);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    const auto list_type = static_cast<D3D12_COMMAND_LIST_TYPE>(static_cast<std::uint32_t>(type));
#pragma warning(suppress : 6387)  // the caller's null output, on purpose (see slot 8)
    if (!out) return self->real->CreateCommandAllocator(list_type, *riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateCommandAllocator(list_type, __uuidof(ID3D12CommandAllocator), &created);
    if (FAILED(hr)) return hr;
    if (!created) return E_FAIL;
    auto* allocator = static_cast<IUnknown*>(created);
    if (!resolves_allocator_proxies(allocator)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: allocators of this device are not wrapped: its command lists "
                                 "cannot resolve an allocator proxy\n");
        const HRESULT asked = allocator->QueryInterface(*riid, out);
        allocator->Release();
        return asked;
    }
    return wrap_child<ChildKind::allocator>(allocator, self, riid, out);
}

// Device slot 32: OpenSharedHandle(handle, riid, out). The object is opened as asked; when it is a fence (whatever
// interface was asked for, IUnknown included) the title gets a fence proxy, anything else (heaps, resources) is
// returned unchanged.
HRESULT proxy_open_shared_handle(DeviceProxy* self, std::uint64_t handle, const IID* riid, void** out) {
    note_slot(32);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    const auto shared = reinterpret_cast<HANDLE>(handle);
    if (!out) return self->real->OpenSharedHandle(shared, *riid, nullptr);
    void* opened = nullptr;
    HRESULT hr = self->real->OpenSharedHandle(shared, *riid, &opened);
    if (FAILED(hr) || !opened) return FAILED(hr) ? hr : E_FAIL;
    void* fence = nullptr;
    if (FAILED(static_cast<IUnknown*>(opened)->QueryInterface(__uuidof(ID3D12Fence), &fence)) || !fence) {
        *out = opened;  // not a fence
        return hr;
    }
    static_cast<IUnknown*>(opened)->Release();
    return wrap_child<ChildKind::fence>(static_cast<IUnknown*>(fence), self, riid, out);
}

// Device slots that take heap, queue or fence objects: the real runtime gets the real objects.
//   31 CreateSharedHandle(object, attributes, access, name, handle)
//   34 MakeResident(count, objects)      35 Evict(count, objects)
//   45 SetEventOnMultipleFenceCompletion(fences, values, count, flags, event)            (ID3D12Device1)
//   46 SetResidencyPriority(count, objects, priorities)                                  (ID3D12Device1)
//   50 EnqueueMakeResident(flags, count, objects, fence, value)                          (ID3D12Device3)
template <unsigned Slot>
HRESULT proxy_unwrapping_call(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                              std::uint64_t a4, std::uint64_t a5) {
    note_slot(Slot);
    if (Slot >= self->slot_count) return refuse_beyond_device(Slot, self->slot_count);  // predates the interface
    UnwrappedArray objects;
    std::uint64_t r1 = a1, r2 = a2, r3 = a3, r4 = a4;
    if constexpr (Slot == 31) {
        r1 = unwrap_object(a1);
    } else if constexpr (Slot == 34 || Slot == 35 || Slot == 46) {
        if (!objects.fill(a2, a1)) return E_OUTOFMEMORY;
        if (objects.data()) r2 = objects.data();
    } else if constexpr (Slot == 45) {
        if (!objects.fill(a1, a3)) return E_OUTOFMEMORY;
        if (objects.data()) r1 = objects.data();
    } else if constexpr (Slot == 50) {
        if (!objects.fill(a3, a2)) return E_OUTOFMEMORY;
        if (objects.data()) r3 = objects.data();
        r4 = unwrap_object(a4);
    }
    // r1-r4 differ from a1-a4 only in a position the method takes, so the title's frame has that word.
    return static_cast<HRESULT>(static_cast<std::uint32_t>(forward_device<Slot>(self, r1, r2, r3, r4, a5)));
}

HRESULT proxy_create_descriptor_heap(DeviceProxy* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc, const IID* riid,
                                     void** out) {
    note_slot(14);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure, as the PC device does
    if (!riid) return E_POINTER;
    // No output: a capability check, answered by the device (S_FALSE, as on the PC).
    if (!out) return self->real->CreateDescriptorHeap(desc, *riid, nullptr);

    void* created = nullptr;
    HRESULT hr = self->real->CreateDescriptorHeap(desc, __uuidof(ID3D12DescriptorHeap), &created);
    if (FAILED(hr) || !created) return FAILED(hr) ? hr : E_FAIL;

    ID3D12DescriptorHeap* heap = static_cast<ID3D12DescriptorHeap*>(created);
    constexpr auto method = "XboxID3D12Device::CreateDescriptorHeap";
    const auto refuse = [&](HRESULT code, const char* why) {
        std::fprintf(stderr, "[rt] d3d12_xs: slot 14 CreateDescriptorHeap refused (0x%08lx): %s\n",
                     static_cast<unsigned long>(code), why);
        heap->Release();
        return d3d12_trace::result(method, code);
    };

    // Track real ranges for alias rejection; native heaps created directly on the backend are not registered.
    // Translation only classifies handles inside a registered custom range (see translate_locked).
    HeapCreation creation;
    if (desc) {
        creation.type = desc->Type;
        creation.increment = self->real->GetDescriptorHandleIncrementSize(desc->Type);
        creation.real_cpu_start = raw_heap_get_cpu_handle(heap, *reinterpret_cast<void***>(heap)).ptr;
        constexpr SIZE_T kMax = (std::numeric_limits<SIZE_T>::max)();
        if (creation.increment && desc->NumDescriptors <= kMax / creation.increment)
            creation.byte_size = static_cast<SIZE_T>(desc->NumDescriptors) * creation.increment;
        if (creation.real_cpu_start && creation.real_cpu_start > kMax - creation.byte_size)
            return refuse(E_FAIL, "the real heap's CPU range wraps the address space");
    }

    // The title's backing (PR #7021): requested after the real heap exists, with NumDescriptors * increment bytes and
    // the D3D descriptor-heap attribute word. A heap of zero descriptors asks for nothing.
    const DescriptorHeapAllocator allocator = current_heap_allocator();
    if (allocator.allocate && desc && desc->NumDescriptors > 0) {
        if (!allocator.release) return refuse(E_FAIL, "the descriptor allocator has no paired release hook");
        if (!creation.byte_size || !creation.real_cpu_start)
            return refuse(E_FAIL, "no usable increment or real CPU start to map a title range onto");
        DescriptorHeapBacking backing;
        const DescriptorBackingStatus status =
            allocator.allocate(creation.byte_size, kXmemDescriptorHeapAttributes, &backing);
        if (status == DescriptorBackingStatus::out_of_memory)
            return refuse(E_OUTOFMEMORY, "the title's allocation hook returned no memory");
        if (status == DescriptorBackingStatus::invalid_hooks)
            return refuse(E_FAIL, "the title's __XMemAllocationHooks__ are invalid");
        if (status == DescriptorBackingStatus::allocated) {
            const auto address = reinterpret_cast<SIZE_T>(backing.address);
            if (!address || !allocator.release || address > (std::numeric_limits<SIZE_T>::max)() - creation.byte_size) {
                if (address && allocator.release) allocator.release(backing, kXmemDescriptorHeapAttributes);
                return refuse(E_FAIL, "the title's allocation is null, unreleasable or wraps the address space");
            }
            creation.custom_cpu_start = address;
            creation.backing = backing;
            creation.release_backing = allocator.release;
        } else if (status != DescriptorBackingStatus::not_hooked) {
            if (backing.address && allocator.release) allocator.release(backing, kXmemDescriptorHeapAttributes);
            return refuse(E_FAIL, "the descriptor-heap allocator returned an unknown status");
        }
    }

    hr = create_heap_proxy(heap, self, *riid, out, creation);
    if (FAILED(hr)) {
        heap->Release();  // the real heap first, then its title backing, as on the last Release
        if (creation.custom_cpu_start) creation.release_backing(creation.backing, kXmemDescriptorHeapAttributes);
    }
    return hr;
}

// Device slots 17..24 (PR #7021): the destination and source CPU handles are translated (rules in d3d12_xs_device.h).
// Each helper takes the words in the positions the thunk received them, and the forwarded call keeps those positions.
bool device_handle(DeviceProxy* self, unsigned slot, std::uint64_t word, D3D12_DESCRIPTOR_HEAP_TYPE type,
                   D3D12_CPU_DESCRIPTOR_HANDLE& out) {
    if (translate_handle(self, static_cast<SIZE_T>(word), 1, type, out.ptr) != DescriptorHandleTranslation::invalid)
        return true;
    refuse_descriptor_call(slot, static_cast<SIZE_T>(word));
    return false;
}

void proxy_create_cbv(DeviceProxy* self, std::uint64_t desc, std::uint64_t dest) {
    note_slot(17);
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    if (!device_handle(self, 17, dest, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, handle)) return;
    self->real->CreateConstantBufferView(reinterpret_cast<const D3D12_CONSTANT_BUFFER_VIEW_DESC*>(desc), handle);
}

void proxy_create_srv(DeviceProxy* self, std::uint64_t resource, std::uint64_t desc, std::uint64_t dest) {
    note_slot(18);
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    if (!device_handle(self, 18, dest, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, handle)) return;
    self->real->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(resource),
                                         reinterpret_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(desc), handle);
}

void proxy_create_uav(DeviceProxy* self, std::uint64_t resource, std::uint64_t counter, std::uint64_t desc,
                      std::uint64_t dest) {
    note_slot(19);
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    if (!device_handle(self, 19, dest, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, handle)) return;
    self->real->CreateUnorderedAccessView(reinterpret_cast<ID3D12Resource*>(resource),
                                          reinterpret_cast<ID3D12Resource*>(counter),
                                          reinterpret_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC*>(desc), handle);
}

void proxy_create_rtv(DeviceProxy* self, std::uint64_t resource, std::uint64_t desc, std::uint64_t dest) {
    note_slot(20);
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    if (!device_handle(self, 20, dest, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, handle)) return;
    self->real->CreateRenderTargetView(reinterpret_cast<ID3D12Resource*>(resource),
                                       reinterpret_cast<const D3D12_RENDER_TARGET_VIEW_DESC*>(desc), handle);
}

void proxy_create_dsv(DeviceProxy* self, std::uint64_t resource, std::uint64_t desc, std::uint64_t dest) {
    note_slot(21);
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    if (!device_handle(self, 21, dest, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, handle)) return;
    self->real->CreateDepthStencilView(reinterpret_cast<ID3D12Resource*>(resource),
                                       reinterpret_cast<const D3D12_DEPTH_STENCIL_VIEW_DESC*>(desc), handle);
}

void proxy_create_sampler(DeviceProxy* self, std::uint64_t desc, std::uint64_t dest) {
    note_slot(22);
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    if (!device_handle(self, 22, dest, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, handle)) return;
    self->real->CreateSampler(reinterpret_cast<const D3D12_SAMPLER_DESC*>(desc), handle);
}

// CopyDescriptors(numDest, destStarts, destSizes, numSrc, srcStarts, srcSizes, type): each range is checked over its
// whole size (a null size array means ranges of one), in copies; the caller's arrays are never written. With no
// title-backed heap the call goes on untouched.
void proxy_copy_descriptors(DeviceProxy* self, std::uint64_t num_dest, std::uint64_t dest_starts,
                            std::uint64_t dest_sizes, std::uint64_t num_src, std::uint64_t src_starts,
                            std::uint64_t src_sizes, std::uint64_t type_word) {
    note_slot(23);
    const auto type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(static_cast<std::uint32_t>(type_word));
    const auto* dest = reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(dest_starts);
    const auto* src = reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(src_starts);
    const auto* dsizes = reinterpret_cast<const UINT*>(dest_sizes);
    const auto* ssizes = reinterpret_cast<const UINT*>(src_sizes);
    if (g_title_range_count.load(std::memory_order_acquire) == 0) {
        self->real->CopyDescriptors(static_cast<UINT>(num_dest), dest, dsizes, static_cast<UINT>(num_src), src,
                                    ssizes, type);
        return;
    }
    TranslatedHandles dest_copy;
    TranslatedHandles src_copy;
    if (!dest_copy.fill(self, dest, dsizes, static_cast<UINT>(num_dest), type) ||
        !src_copy.fill(self, src, ssizes, static_cast<UINT>(num_src), type)) {
        refuse_descriptor_call(23, dest ? dest[0].ptr : 0);
        return;
    }
    self->real->CopyDescriptors(static_cast<UINT>(num_dest), dest_copy.data(dest), dsizes,
                                static_cast<UINT>(num_src), src_copy.data(src), ssizes, type);
}

// CopyDescriptorsSimple(num, destStart, srcStart, type): both spans of `num` descriptors.
void proxy_copy_descriptors_simple(DeviceProxy* self, std::uint64_t num, std::uint64_t dest_start,
                                   std::uint64_t src_start, std::uint64_t type_word) {
    note_slot(24);
    const auto count = static_cast<UINT>(num);
    const auto type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(static_cast<std::uint32_t>(type_word));
    D3D12_CPU_DESCRIPTOR_HANDLE dest{};
    D3D12_CPU_DESCRIPTOR_HANDLE src{};
    if (translate_handle(self, static_cast<SIZE_T>(dest_start), count, type, dest.ptr) ==
            DescriptorHandleTranslation::invalid ||
        translate_handle(self, static_cast<SIZE_T>(src_start), count, type, src.ptr) ==
            DescriptorHandleTranslation::invalid) {
        refuse_descriptor_call(24, static_cast<SIZE_T>(dest_start));
        return;
    }
    self->real->CopyDescriptorsSimple(count, dest, src, type);
}

// The Xbox slot 62 takes two more words between the description and the IID: (desc, w2, w3, riid, out). Measured
// on the package's XboxBvhBuilder.dll (its constructor passes w2 = w3 = 0 and IID_ID3D12PipelineState in the fourth
// argument), as the prior art had recorded. Their meaning is unknown; non-zero values are logged once.
HRESULT proxy_create_compute_pipeline_state(DeviceProxy* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                            std::uint64_t w2, std::uint64_t w3, const IID* riid, void** out) {
    note_slot(62);
    if (w2 || w3) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: slot 62 extra words 0x%llx 0x%llx (ignored)\n",
                         static_cast<unsigned long long>(w2), static_cast<unsigned long long>(w3));
    }
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    return self->real->CreateComputePipelineState(desc, *riid, out);
}

// The Xbox slot 67 creates a compute pipeline from native GPU code: (desc, riid, out), named
// DeserializeComputePipelineStateX in amd_fidelityfx_upscaler_xbox.dll's messages (call at RVA 0x56d9). The desc
// passes the root signature at +0x00, followed by pointer/size pairs for the shader's FSPX entries (docs/fspx.md):
// kind 0 (dispatch state) at +0x08/+0x10, kind 1 at +0x18/+0x20, kind 6 (the RDNA2 program) at +0x28/+0x30, and
// kind 7 (records) at +0x38/+0x40. Translates the console description to a standard D3D12_COMPUTE_PIPELINE_STATE_DESC
// with CS bytecode pointing at kind 6 and routes it to CreateComputePipelineState on the PC device (slot 11), which
// on the Vulkan device resolves through the translator. The PC slot 67 is CreateProtectedResourceSession1.
HRESULT proxy_create_compute_pipeline_from_native_code(DeviceProxy* self, const void* desc,
                                                       const IID* riid, void** out) {
    note_slot(67);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!desc) return E_INVALIDARG;
    if (!riid) return E_POINTER;

    // FSPX description layout (docs/fspx.md §How the DLL reads it):
    struct XboxNativeComputePipelineStateDesc {
        ID3D12RootSignature* root_signature;  // +0x00
        const void* kind0;                    // +0x08: dispatch state
        std::uint64_t kind0_size;             // +0x10: 28 bytes
        const void* kind1;                    // +0x18: unknown
        std::uint64_t kind1_size;             // +0x20: 20 bytes
        const void* kind6;                    // +0x28: RDNA2 program code
        std::uint64_t kind6_size;             // +0x30: program size in bytes
        const void* kind7;                    // +0x38: resource/hash records
        std::uint64_t kind7_size;             // +0x40: records size in bytes
    };
    const auto* xbox_desc = static_cast<const XboxNativeComputePipelineStateDesc*>(desc);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pc_desc{};
    pc_desc.pRootSignature = xbox_desc->root_signature;
    pc_desc.CS.pShaderBytecode = xbox_desc->kind6;
    pc_desc.CS.BytecodeLength = static_cast<SIZE_T>(xbox_desc->kind6_size);
    // With kind 0 (the DLL always passes it: its reader refuses a payload without one), the four pairs go on as one
    // FSPX payload, so the device matches the program by its kind-6 and kind-0 keys (docs/fspx.md, "Build and run-time
    // lookup"): two programs with the same code and different dispatch state stay apart. Every pair is checked before
    // it is read (palmwave::fspx_payload_from_parts); an inconsistent one refuses the call. Without kind 0 the kind-6
    // bytes go on alone, as before.
    std::vector<std::uint8_t> payload;
    if (xbox_desc->kind0 || xbox_desc->kind0_size) {
        const palmwave::FspxPart parts[] = {{0, xbox_desc->kind0, xbox_desc->kind0_size},
                                           {1, xbox_desc->kind1, xbox_desc->kind1_size},
                                           {6, xbox_desc->kind6, xbox_desc->kind6_size},
                                           {7, xbox_desc->kind7, xbox_desc->kind7_size}};
        payload = palmwave::fspx_payload_from_parts(parts);
        if (payload.empty()) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr,
                             "[rt] d3d12_xs: slot 67 refused: a pointer/size pair is inconsistent or too large "
                             "(kind0 %llu B, kind1 %llu B, kind6 %llu B, kind7 %llu B)\n",
                             static_cast<unsigned long long>(xbox_desc->kind0_size),
                             static_cast<unsigned long long>(xbox_desc->kind1_size),
                             static_cast<unsigned long long>(xbox_desc->kind6_size),
                             static_cast<unsigned long long>(xbox_desc->kind7_size));
            return E_INVALIDARG;
        }
        pc_desc.CS.pShaderBytecode = payload.data();
        pc_desc.CS.BytecodeLength = payload.size();
    }

    pc_desc.NodeMask = 0;
    pc_desc.CachedPSO = {nullptr, 0};
    pc_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    // Every failed call is logged with its number: the DLL creates its pipelines one after the other, and only the
    // first call of a slot is announced, so a miss on a later program would otherwise read as the first call's.
    static std::atomic<std::uint64_t> calls{0};
    const std::uint64_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const HRESULT hr = self->real->CreateComputePipelineState(&pc_desc, *riid, out);
    if (FAILED(hr))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 67 call %llu failed with 0x%08lx (kind 0 %llu B, kind 1 %llu B, kind 6 %llu B, "
                     "kind 7 %llu B)\n",
                     static_cast<unsigned long long>(call), static_cast<unsigned long>(hr),
                     static_cast<unsigned long long>(xbox_desc->kind0_size),
                     static_cast<unsigned long long>(xbox_desc->kind1_size),
                     static_cast<unsigned long long>(xbox_desc->kind6_size),
                     static_cast<unsigned long long>(xbox_desc->kind7_size));
    return hr;
}

HRESULT proxy_create_resource(DeviceProxy* self, const D3D12_HEAP_PROPERTIES* heap_properties,
                              D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC* xbox_desc,
                              D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE* clear_value,
                              const IID* riid, void** out) {
    note_slot(76);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!xbox_desc) return E_POINTER;
    if (!riid) return E_POINTER;

    // The measured description is 56 bytes plus Xbox extension words; D3D12_RESOURCE_DESC is exactly 56 bytes.
    // The Xbox layout word is 0x101 (tiled) or 0x1 (row-major buffer), and is not a PC layout value. Committed
    // resources take ROW_MAJOR for buffers and UNKNOWN for everything else, so the word is ignored and replaced.
    D3D12_RESOURCE_DESC pc_desc = *xbox_desc;
    const auto xbox_layout = static_cast<std::uint32_t>(pc_desc.Layout);
    pc_desc.Layout = pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                         : D3D12_TEXTURE_LAYOUT_UNKNOWN;

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 76 resource: dimension=%d xbox layout=0x%x -> PC layout=0x%x\n",
                     static_cast<int>(pc_desc.Dimension), xbox_layout, static_cast<unsigned>(pc_desc.Layout));
    }
    return self->real->CreateCommittedResource(heap_properties, heap_flags, &pc_desc, initial_state, clear_value,
                                               *riid, out);
}

// Device slot 26, GetCustomHeapProperties. Upstream PR #6964 measured the following call form under
// palmwave --run with a register probe in this slot's thunk: three calls right after device creation, each with the
// device proxy in RDX and not in RCX, R8 = 1 and R9 = 1, 2, 3 (node mask 1; DEFAULT, UPLOAD, READBACK). So the
// buffer for the returned D3D12_HEAP_PROPERTIES is in RCX and the device in RDX. That is how x64 passes them to a
// plain function that takes the device as its first argument and returns a struct; a PC C++ method has the device
// in RCX and the buffer in RDX. The forwarder took RCX for the proxy, read the PC vtable out of the title's stack
// buffer and faulted. The buffer's address goes back in RAX, as that convention returns it; what the title does with
// RAX is not measured.
D3D12_HEAP_PROPERTIES* proxy_get_custom_heap_properties(D3D12_HEAP_PROPERTIES* out, DeviceProxy* self,
                                                        UINT node_mask, D3D12_HEAP_TYPE heap_type) {
    note_slot(26);
    *out = self->real->GetCustomHeapProperties(node_mask, heap_type);
    return out;
}

// Device slot 133 is the console's CreatePlacedResourceX: (va, desc, state, clear, riid, out), measured on the retail
// executable (docs/retail_boot_chain.md section 3.4: riid IID_ID3D12Resource, the third word 0xFFFFFFFF, whose meaning
// is not established). `va` is the start of memory the title has just mapped and `desc` a 56-byte description. The
// proxy does not make the resource: the device behind it does, through the handler it registered
// (set_placed_at_va_handler). Without one the slot refuses like any slot past the PC vtable. The route follows Palmwave
// #7218 (tibzejoker, a3eae955dcec4fb65864ca6c10b4ca3f2999c2d3, GPL-3.0); its flag mask and layout rewrite are not kept.
// Every word keeps its thunk position (see "A thunk declares 14 argument words" at the top).
//
// Only semantics this path implements are passed, unchanged (d3d12_xs_device.h): a buffer, layout word 0x1 (ROW_MAJOR,
// the same value in the console's row-major word and the PC enum), flags among ALLOW_UNORDERED_ACCESS and
// DENY_SHADER_RESOURCE, no clear value. Nothing is masked or rewritten: a console bit whose meaning is not established
// (0x20000 included) is refused, not stripped. Every refusal leaves *out null and never reaches the handler.
//
// The one exception is an opt-in DIAGNOSTIC EXPERIMENT (experimental_host_import.h): with
// PALMWAVE_EXPERIMENTAL_XBOX_HOST_IMPORT exactly "1", the single call shape in experimental_placement_shape is handed to
// the same handler as a host description with flags ALLOW_UNORDERED_ACCESS and state word COMMON. That 0x20000 can be
// dropped and that 0xFFFFFFFF means "no state" (D3D12_BARRIER_LAYOUT_UNDEFINED on the PC) are hypotheses; nothing here
// establishes the console ABI. Any other shape, flag, state word or clear value goes through the strict checks below.
std::atomic<PlacedAtVaHandler> g_placed_at_va{nullptr};

constexpr std::uint32_t kExperimentalConsoleFlags = 0x00020004;  // ALLOW_UNORDERED_ACCESS | 0x20000 (meaning unknown)
constexpr std::uint32_t kExperimentalStateWord = 0xFFFFFFFFu;    // retail_boot_chain.md 3.4; meaning not established

// The whole experimental tuple, field by field. The DWORD at +4 (padding after Dimension) is not read: its content is
// undefined. Width only has to be nonzero here; the device's own bounds checks still apply to it.
bool experimental_placement_shape(const D3D12_RESOURCE_DESC& desc, std::uint32_t state, const void* clear) {
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && desc.Alignment == 0 && desc.Width != 0 &&
           desc.Height == 1 && desc.DepthOrArraySize == 1 && desc.MipLevels == 1 &&
           desc.Format == DXGI_FORMAT_UNKNOWN && desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0 &&
           static_cast<std::uint32_t>(desc.Layout) == static_cast<std::uint32_t>(D3D12_TEXTURE_LAYOUT_ROW_MAJOR) &&
           static_cast<std::uint32_t>(desc.Flags) == kExperimentalConsoleFlags && state == kExperimentalStateWord &&
           clear == nullptr;
}

HRESULT proxy_create_placed_resource_x(DeviceProxy* self, void* va, const D3D12_RESOURCE_DESC* xbox_desc,
                                       std::uint32_t initial_state, const void* clear, const IID* riid, void** out) {
    const PlacedAtVaHandler handler = g_placed_at_va.load(std::memory_order_acquire);
    if (!handler)
        return proxy_refuse_placed_resource_x(reinterpret_cast<std::uint64_t>(va),
                                             reinterpret_cast<std::uint64_t>(xbox_desc), initial_state,
                                             reinterpret_cast<std::uint64_t>(clear), out);
    note_slot(133);
    if (out) *out = nullptr;
    if (!out || !xbox_desc || !riid)
        return d3d12_trace::result("Xbox CreatePlacedResourceX (device slot 133)", E_POINTER);
    const D3D12_RESOURCE_DESC desc = *xbox_desc;
    const auto flags = static_cast<std::uint32_t>(desc.Flags);
    const auto layout_word = static_cast<std::uint32_t>(desc.Layout);
    constexpr std::uint32_t kBufferFlags =
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    const char* refusal = nullptr;
    HRESULT hr = S_OK;
    if (experimental_host_import::enabled() && experimental_placement_shape(desc, initial_state, clear)) {
        // A new description, built field by field; the title's is not written.
        D3D12_RESOURCE_DESC host{};
        host.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        host.Alignment = 0;
        host.Width = desc.Width;
        host.Height = 1;
        host.DepthOrArraySize = 1;
        host.MipLevels = 1;
        host.Format = DXGI_FORMAT_UNKNOWN;
        host.SampleDesc.Count = 1;
        host.SampleDesc.Quality = 0;
        host.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        host.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        constexpr std::uint32_t kHostState = D3D12_RESOURCE_STATE_COMMON;
        hr = handler(self->real, va, host, kHostState, *riid, out);
        if (FAILED(hr)) *out = nullptr;
        // Every experimental attempt, success or not, is logged and traced: the result depends on a hypothesis.
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 133 EXPERIMENTAL host import (PALMWAVE_EXPERIMENTAL_XBOX_HOST_IMPORT=1; a "
                     "hypothesis, not a measured console ABI): va=%p width=%llu flags 0x%x -> 0x%x "
                     "(ALLOW_UNORDERED_ACCESS; bit 0x20000 dropped, meaning unknown), state word 0x%x -> 0x%x (COMMON), "
                     "clear=null -> 0x%08lx\n",
                     va, static_cast<unsigned long long>(desc.Width), flags, static_cast<unsigned>(host.Flags),
                     initial_state, kHostState, static_cast<unsigned long>(hr));
        // boot_trace reads the last whitespace-delimited field as the outcome. Keep the experimental marker a
        // single token; the real backend result is already traced by create_at_host_address and logged above.
        d3d12_trace::record("Xbox CreatePlacedResourceX (device slot 133) EXPERIMENTAL host import", "experimental");
        return hr;
    }
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        refusal = "not a buffer";
        hr = E_NOTIMPL;
    } else if (layout_word != static_cast<std::uint32_t>(D3D12_TEXTURE_LAYOUT_ROW_MAJOR)) {
        refusal = "layout word is not row-major (0x1)";
        hr = E_NOTIMPL;
    } else if (flags & ~kBufferFlags) {
        refusal = "flag bits outside ALLOW_UNORDERED_ACCESS | DENY_SHADER_RESOURCE";
        hr = E_NOTIMPL;
    } else if (clear) {
        refusal = "a clear value on a buffer";
        hr = E_INVALIDARG;
    } else {
        hr = handler(self->real, va, desc, initial_state, *riid, out);
    }
    // Every failure is logged (not only the first call): a refused placement leaves the title without its resource,
    // and the reason is the proxy's refusal or the device's [rt] line just before this one.
    static std::atomic<bool> logged{false};
    if (FAILED(hr) || !logged.exchange(true, std::memory_order_relaxed) || trace_enabled())
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 133 CreatePlacedResourceX: va=%p dimension=%d width=%llu flags=0x%x "
                     "layout word=0x%x state word=0x%x clear=%p -> 0x%08lx%s%s\n",
                     va, static_cast<int>(desc.Dimension), static_cast<unsigned long long>(desc.Width), flags,
                     layout_word, initial_state, clear, static_cast<unsigned long>(hr), refusal ? " refused: " : "",
                     refusal ? refusal : "");
    if (FAILED(hr)) *out = nullptr;
    // A proxy refusal never reaches the backend's trace; keep it visible just like the no-handler refusal.
    return refusal ? d3d12_trace::result("Xbox CreatePlacedResourceX (device slot 133)", hr) : hr;
}

// ---- Device slot 70: a raw buffer SRV named by console GPU VA (contract in d3d12_xs_device.h) ----
//
// Adapted from upstream PR #7066 (tedin7, 8e700080066d7bbc0528aeaae5c8fb6c9cfdc347, GPL-3.0): the slot is handled by the
// proxy, the buffer is found by VA through a per-proxy resolver that takes a reference safely, and the view goes to the
// PC slot 18. Palmwave departs from it: no null-view fallback (every refusal leaves the destination untouched), no
// clamping of the requested width to the buffer, exact shape and flag checks instead of a RAW-bit test, a destination
// validated and translated through the descriptor-range registry, unreadable descriptions refused before any copy, and
// only bytes 0x00..0x2F of the view read.
constexpr std::size_t kSrvAtVaViewBytes = 0x30;     // the desktop view desc (0x28) and the VA at +0x28
constexpr std::size_t kSrvAtVaVaOffset = 0x28;
constexpr std::uint64_t kSrvAtVaMaxWidth = 0xFFFFFFFCull;  // a raw V#'s 32-bit byte count, in whole elements
static_assert(sizeof(D3D12_SHADER_RESOURCE_VIEW_DESC) == kSrvAtVaVaOffset, "the desktop SRV desc is 40 bytes");
static_assert(offsetof(D3D12_SHADER_RESOURCE_VIEW_DESC, Buffer) == 0x10, "Buffer view fields start at +0x10");
static_assert(offsetof(D3D12_RESOURCE_DESC, Alignment) == 8, "the DWORD at +4 of the resource desc is padding");
static_assert(D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT == 16, "raw views are 16-byte aligned");

std::atomic<SrvAtVaOutcome> g_srv_at_va_last{SrvAtVaOutcome::none};
std::atomic<std::uint64_t> g_srv_at_va_created{0};
std::atomic<std::uint64_t> g_srv_at_va_refused{0};

// True when every byte of [address, address + bytes) is committed, readable and not a guard page right now.
bool readable_range(std::uint64_t address, std::size_t bytes) {
    if (!address || !bytes || address > (std::numeric_limits<std::uint64_t>::max)() - bytes) return false;
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const std::uint64_t end = address + bytes;
    std::uint64_t at = address;
    while (at < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) != sizeof(info)) return false;
        if (info.State != MEM_COMMIT || !(info.Protect & kReadable) || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const std::uint64_t region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (region_end <= at) return false;
        at = region_end;
    }
    return true;
}

// memcpy that turns an access violation (the title unmapping the bytes after readable_range) into a refusal. No C++
// objects here: a function with __try cannot have any that need unwinding.
bool guarded_copy(void* destination, const void* source, std::size_t size) {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

template <class T>
T field_at(const std::uint8_t* bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes + offset, sizeof(T));
    return value;
}

const char* srv_at_va_reason(SrvAtVaOutcome outcome) {
    switch (outcome) {
        case SrvAtVaOutcome::unreadable: return "a description pointer is null or not committed readable memory";
        case SrvAtVaOutcome::unsupported_shape: return "not the supported raw-buffer shape";
        case SrvAtVaOutcome::invalid_range: return "zero, misaligned or overflowing VA or width";
        case SrvAtVaOutcome::invalid_destination:
            return "the destination is not a CBV_SRV_UAV slot of a heap this device created";
        case SrvAtVaOutcome::no_resolver: return "no buffer resolver on this device";
        case SrvAtVaOutcome::unknown_va: return "no live buffer of this device holds the whole range";
        case SrvAtVaOutcome::not_shader_readable: return "the buffer denies shader-resource access";
        case SrvAtVaOutcome::resolver_failed: return "the resolver failed or answered an inconsistent range";
        default: return "unknown";
    }
}

// Not forwarded: counted, logged (first time per reason, every time with XS_TRACE) and traced.
void refuse_srv_at_va(SrvAtVaOutcome outcome, const char* detail, std::uint64_t resource_desc, std::uint64_t view_desc,
                      std::uint64_t destination, std::uint64_t va, std::uint64_t width) {
    g_srv_at_va_refused.fetch_add(1, std::memory_order_relaxed);
    g_srv_at_va_last.store(outcome, std::memory_order_relaxed);
    static std::atomic<bool> warned[16];
    const auto index = static_cast<unsigned>(outcome);
    if ((index < std::size(warned) && first_call(warned[index])) || trace_enabled())
        std::fprintf(stderr,
                     "[rt] d3d12_xs: device slot 70 refused (%s%s%s): args 0x%llx 0x%llx 0x%llx, va 0x%llx width %llu; "
                     "nothing written, the destination is unchanged\n",
                     srv_at_va_reason(outcome), detail ? ": " : "", detail ? detail : "",
                     static_cast<unsigned long long>(resource_desc), static_cast<unsigned long long>(view_desc),
                     static_cast<unsigned long long>(destination), static_cast<unsigned long long>(va),
                     static_cast<unsigned long long>(width));
    const bool unsupported = outcome == SrvAtVaOutcome::unsupported_shape;
    d3d12_trace::record(slot_name(70), unsupported ? "unsupported" : "refused");
}

// The supported shape, read field by field (padding is never read). Null when it matches, else what differs.
const char* srv_at_va_shape_mismatch(const std::uint8_t* resource, const std::uint8_t* view) {
    const auto dimension = field_at<std::uint32_t>(resource, offsetof(D3D12_RESOURCE_DESC, Dimension));
    const auto alignment = field_at<std::uint64_t>(resource, offsetof(D3D12_RESOURCE_DESC, Alignment));
    const auto height = field_at<std::uint32_t>(resource, offsetof(D3D12_RESOURCE_DESC, Height));
    const auto depth = field_at<std::uint16_t>(resource, offsetof(D3D12_RESOURCE_DESC, DepthOrArraySize));
    const auto mips = field_at<std::uint16_t>(resource, offsetof(D3D12_RESOURCE_DESC, MipLevels));
    const auto format = field_at<std::uint32_t>(resource, offsetof(D3D12_RESOURCE_DESC, Format));
    const auto samples = field_at<std::uint32_t>(resource, offsetof(D3D12_RESOURCE_DESC, SampleDesc));
    const auto quality = field_at<std::uint32_t>(resource, offsetof(D3D12_RESOURCE_DESC, SampleDesc) + 4);
    const auto layout = field_at<std::uint32_t>(resource, offsetof(D3D12_RESOURCE_DESC, Layout));
    const auto flags = field_at<std::uint32_t>(resource, offsetof(D3D12_RESOURCE_DESC, Flags));
    if (dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return "resource Dimension is not BUFFER";
    if (alignment != 0) return "resource Alignment is not 0";
    if (height != 1 || depth != 1 || mips != 1) return "resource Height/DepthOrArraySize/MipLevels are not 1";
    if (format != DXGI_FORMAT_UNKNOWN) return "resource Format is not UNKNOWN";
    if (samples != 1 || quality != 0) return "resource SampleDesc is not {1, 0}";
    if (layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR) return "resource Layout is not ROW_MAJOR";
    if (flags != D3D12_RESOURCE_FLAG_NONE) return "resource Flags are not NONE";

    const auto view_format = field_at<std::uint32_t>(view, offsetof(D3D12_SHADER_RESOURCE_VIEW_DESC, Format));
    const auto view_dimension =
        field_at<std::uint32_t>(view, offsetof(D3D12_SHADER_RESOURCE_VIEW_DESC, ViewDimension));
    const auto mapping =
        field_at<std::uint32_t>(view, offsetof(D3D12_SHADER_RESOURCE_VIEW_DESC, Shader4ComponentMapping));
    constexpr std::size_t kBuffer = offsetof(D3D12_SHADER_RESOURCE_VIEW_DESC, Buffer);
    const auto first = field_at<std::uint64_t>(view, kBuffer + offsetof(D3D12_BUFFER_SRV, FirstElement));
    const auto stride = field_at<std::uint32_t>(view, kBuffer + offsetof(D3D12_BUFFER_SRV, StructureByteStride));
    const auto view_flags = field_at<std::uint32_t>(view, kBuffer + offsetof(D3D12_BUFFER_SRV, Flags));
    if (view_format != DXGI_FORMAT_R32_TYPELESS) return "view Format is not R32_TYPELESS";
    if (view_dimension != D3D12_SRV_DIMENSION_BUFFER) return "view ViewDimension is not BUFFER";
    if (mapping != D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING) return "view Shader4ComponentMapping is not the default";
    if (first != 0) return "view FirstElement is not 0 (an offset into the VA has no established meaning)";
    if (stride != 0) return "view StructureByteStride is not 0";
    if (view_flags != D3D12_BUFFER_SRV_FLAG_RAW) return "view Flags are not exactly RAW";
    return nullptr;
}

// Device slot 70. Every word keeps its thunk position (see "A thunk declares 14 argument words" at the top).
void proxy_create_srv_at_va_x(DeviceProxy* self, std::uint64_t resource_desc, std::uint64_t view_desc,
                              std::uint64_t destination) {
    // Copies of exactly the bytes the contract names: the resource desc (56) and the first 0x30 bytes of the view.
    std::uint8_t resource[sizeof(D3D12_RESOURCE_DESC)];
    std::uint8_t view[kSrvAtVaViewBytes];
    const auto refuse = [&](SrvAtVaOutcome outcome, const char* detail, std::uint64_t va, std::uint64_t width) {
        refuse_srv_at_va(outcome, detail, resource_desc, view_desc, destination, va, width);
    };
    if (!readable_range(resource_desc, sizeof(resource)) || !readable_range(view_desc, sizeof(view)) ||
        !guarded_copy(resource, reinterpret_cast<const void*>(resource_desc), sizeof(resource)) ||
        !guarded_copy(view, reinterpret_cast<const void*>(view_desc), sizeof(view)))
        return refuse(SrvAtVaOutcome::unreadable, nullptr, 0, 0);

    const auto width = field_at<std::uint64_t>(resource, offsetof(D3D12_RESOURCE_DESC, Width));
    const auto va = field_at<std::uint64_t>(view, kSrvAtVaVaOffset);
    if (const char* mismatch = srv_at_va_shape_mismatch(resource, view))
        return refuse(SrvAtVaOutcome::unsupported_shape, mismatch, va, width);
    const auto elements = field_at<std::uint32_t>(
        view, offsetof(D3D12_SHADER_RESOURCE_VIEW_DESC, Buffer) + offsetof(D3D12_BUFFER_SRV, NumElements));
    if (!width || width % 4 != 0 || width > kSrvAtVaMaxWidth)
        return refuse(SrvAtVaOutcome::invalid_range, "Width is 0, not a multiple of 4 or above 0xFFFFFFFC", va, width);
    if (elements != width / 4)
        return refuse(SrvAtVaOutcome::unsupported_shape, "view NumElements is not Width / 4", va, width);
    if (!va || va % D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT != 0 || va > (std::numeric_limits<std::uint64_t>::max)() - width)
        return refuse(SrvAtVaOutcome::invalid_range, "VA is 0, not 16-byte aligned or VA + Width overflows", va, width);

    SIZE_T real_destination = 0;
    if (!registered_descriptor(self, static_cast<SIZE_T>(destination), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                               real_destination))
        return refuse(SrvAtVaOutcome::invalid_destination, nullptr, va, width);

    const BufferAtVaResolver resolver = self->buffer_resolver.load(std::memory_order_acquire);
    if (!resolver) return refuse(SrvAtVaOutcome::no_resolver, nullptr, va, width);
    BufferAtVa hit{};
    const BufferAtVaStatus status = resolver(self->real, va, width, &hit);
    // Whatever the status, a reference handed back is released exactly once.
    ID3D12Resource* const buffer = hit.resource;
    const auto drop = [&] {
        if (buffer) buffer->Release();
    };
    if (status != BufferAtVaStatus::found) {
        drop();
        if (status == BufferAtVaStatus::not_found) return refuse(SrvAtVaOutcome::unknown_va, nullptr, va, width);
        if (status == BufferAtVaStatus::not_shader_readable)
            return refuse(SrvAtVaOutcome::not_shader_readable, nullptr, va, width);
        return refuse(SrvAtVaOutcome::resolver_failed, "the resolver reported a failure", va, width);
    }
    // The answer must hold the whole request; nothing is cut to fit. The resolver's checks are repeated here.
    if (!buffer || va < hit.start || va - hit.start > hit.size || width > hit.size - (va - hit.start) ||
        (va - hit.start) % 4 != 0) {
        drop();
        return refuse(SrvAtVaOutcome::resolver_failed, "the answered range does not hold the request", va, width);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC host{};
    host.Format = DXGI_FORMAT_R32_TYPELESS;
    host.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    host.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    host.Buffer.FirstElement = (va - hit.start) / 4;
    host.Buffer.NumElements = elements;
    host.Buffer.StructureByteStride = 0;
    host.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    self->real->CreateShaderResourceView(buffer, &host, D3D12_CPU_DESCRIPTOR_HANDLE{real_destination});
    drop();
    g_srv_at_va_created.fetch_add(1, std::memory_order_relaxed);
    g_srv_at_va_last.store(SrvAtVaOutcome::created, std::memory_order_relaxed);
    static std::atomic<bool> logged{false};
    if (first_call(logged) || trace_enabled())
        std::fprintf(stderr,
                     "[rt] d3d12_xs: device slot 70 wrote a raw buffer view: va 0x%llx, %llu bytes, buffer start 0x%llx "
                     "(first element %llu), destination 0x%llx -> 0x%llx\n",
                     static_cast<unsigned long long>(va), static_cast<unsigned long long>(width),
                     static_cast<unsigned long long>(hit.start),
                     static_cast<unsigned long long>(host.Buffer.FirstElement),
                     static_cast<unsigned long long>(destination), static_cast<unsigned long long>(real_destination));
    d3d12_trace::record(slot_name(70), "ok");
}

// The Xbox routes at their configured indices (PALMWAVE_XS_*_SLOT). False when `slot` is none of them. Slot 70 asks
// first too, so a route moved onto 70 keeps the existing environment-override priority over the built-in handler.
// The route dispatch is adapted from upstream PR #7066 (tedin7, 8e700080, GPL-3.0).
bool route_xbox_device_slot(DeviceProxy* self, unsigned slot, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                            std::uint64_t a4, std::uint64_t a5, std::uint64_t& result) {
    const XboxDeviceSlotRoutes& routes = xbox_device_slot_routes();
    if (slot == routes.frame_interval) {
        log_xbox_route_once(slot, "SetFrameIntervalX", a1, a2, a3, a4, a5);
        result = hr_to_u64(proxy_set_frame_interval_x(self, a1, a2, a3, a4));
    } else if (slot == routes.schedule_event) {
        log_xbox_route_once(slot, "ScheduleFrameEventX", a1, a2, a3, a4, a5);
        result = hr_to_u64(proxy_schedule_frame_event_x(self, a1, a2, a4));
    } else if (slot == routes.wait_event) {
        log_xbox_route_once(slot, "WaitFrameEventX", a1, a2, a3, a4, a5);
        result = hr_to_u64(proxy_wait_frame_event_x(self, a1, a2, a4, a5));
    } else if (slot == routes.queue_x) {
        log_xbox_route_once(slot, "CreateCommandQueueX", a1, a2, a3, a4, a5);
        result = hr_to_u64(proxy_create_command_queue_x(self, reinterpret_cast<const D3D12_COMMAND_QUEUE_DESC*>(a1),
                                                        reinterpret_cast<const IID*>(a2), reinterpret_cast<void**>(a3)));
    } else if (slot == routes.list_x) {
        log_xbox_route_once(slot, "CreateCommandListX", a1, a2, a3, a4, a5);
        result = hr_to_u64(proxy_create_command_list_x(self, a1, a2, a3, reinterpret_cast<const IID*>(a4),
                                                       reinterpret_cast<void**>(a5)));
    } else {
        return false;
    }
    return true;
}

template <unsigned Slot>
std::uint64_t WINAPI proxy_slot(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                                std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                                std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        return hr_to_u64(proxy_query_interface(self, reinterpret_cast<const IID*>(a1),
                                               reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return proxy_add_ref(self);
    } else if constexpr (Slot == 2) {
        return proxy_release(self);
    } else if constexpr (Slot == 8) {
        return hr_to_u64(proxy_create_command_queue(self, reinterpret_cast<const D3D12_COMMAND_QUEUE_DESC*>(a1),
                                                    reinterpret_cast<const IID*>(a2), reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 9) {
        return hr_to_u64(proxy_create_command_allocator(self, a1, reinterpret_cast<const IID*>(a2),
                                                        reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 36) {
        return hr_to_u64(proxy_create_fence(self, a1, a2, reinterpret_cast<const IID*>(a3),
                                            reinterpret_cast<void**>(a4)));
    } else if constexpr (Slot == 32) {
        return hr_to_u64(proxy_open_shared_handle(self, a1, reinterpret_cast<const IID*>(a2),
                                                  reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 31 || Slot == 34 || Slot == 35 || Slot == 45 || Slot == 46 || Slot == 50) {
        return hr_to_u64(proxy_unwrapping_call<Slot>(self, a1, a2, a3, a4, a5));
    } else if constexpr (Slot == 75) {
        return hr_to_u64(proxy_create_command_queue1(self, a1, a2, reinterpret_cast<const IID*>(a3),
                                                     reinterpret_cast<void**>(a4)));
    } else if constexpr (Slot == 14) {
        return hr_to_u64(proxy_create_descriptor_heap(
            self, reinterpret_cast<const D3D12_DESCRIPTOR_HEAP_DESC*>(a1),
            reinterpret_cast<const IID*>(a2), reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 17) {
        proxy_create_cbv(self, a1, a2);
        return 0;  // void methods: the return register carries nothing
    } else if constexpr (Slot == 18) {
        proxy_create_srv(self, a1, a2, a3);
        return 0;
    } else if constexpr (Slot == 19) {
        proxy_create_uav(self, a1, a2, a3, a4);
        return 0;
    } else if constexpr (Slot == 20) {
        proxy_create_rtv(self, a1, a2, a3);
        return 0;
    } else if constexpr (Slot == 21) {
        proxy_create_dsv(self, a1, a2, a3);
        return 0;
    } else if constexpr (Slot == 22) {
        proxy_create_sampler(self, a1, a2);
        return 0;
    } else if constexpr (Slot == 23) {
        proxy_copy_descriptors(self, a1, a2, a3, a4, a5, a6, a7);
        return 0;
    } else if constexpr (Slot == 24) {
        proxy_copy_descriptors_simple(self, a1, a2, a3, a4);
        return 0;
    } else if constexpr (Slot == 62) {
        return hr_to_u64(proxy_create_compute_pipeline_state(
            self, reinterpret_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(a1), a2, a3,
            reinterpret_cast<const IID*>(a4), reinterpret_cast<void**>(a5)));
    } else if constexpr (Slot == 67) {
        return hr_to_u64(proxy_create_compute_pipeline_from_native_code(
            self, reinterpret_cast<const void*>(a1), reinterpret_cast<const IID*>(a2),
            reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 70) {
        note_slot(70);
        // A configured Xbox route moved onto 70 keeps its priority over the built-in view.
        std::uint64_t routed = 0;
        if (route_xbox_device_slot(self, Slot, a1, a2, a3, a4, a5, routed)) return routed;
        proxy_create_srv_at_va_x(self, a1, a2, a3);
        return 0;  // void method: the return register carries nothing
    } else if constexpr (Slot == 76) {
        return hr_to_u64(proxy_create_resource(
            self, reinterpret_cast<const D3D12_HEAP_PROPERTIES*>(a1),
            static_cast<D3D12_HEAP_FLAGS>(static_cast<std::uint32_t>(a2)),
            reinterpret_cast<const D3D12_RESOURCE_DESC*>(a3),
            static_cast<D3D12_RESOURCE_STATES>(static_cast<std::uint32_t>(a4)),
            reinterpret_cast<const D3D12_CLEAR_VALUE*>(a5), reinterpret_cast<const IID*>(a6),
            reinterpret_cast<void**>(a7)));
    } else if constexpr (Slot == 133) {
        return hr_to_u64(proxy_create_placed_resource_x(
            self, reinterpret_cast<void*>(a1), reinterpret_cast<const D3D12_RESOURCE_DESC*>(a2),
            static_cast<std::uint32_t>(a3), reinterpret_cast<const void*>(a4), reinterpret_cast<const IID*>(a5),
            reinterpret_cast<void**>(a6)));
    } else {
        if constexpr (Slot == 26) {
            // The console form has the device in RDX. A PC C++ caller has the proxy in RCX and its return buffer in
            // RDX; that call goes to the forwarder below, as before.
            if (is_device_proxy(reinterpret_cast<const void*>(a1)))
                return reinterpret_cast<std::uint64_t>(proxy_get_custom_heap_properties(
                    reinterpret_cast<D3D12_HEAP_PROPERTIES*>(self), reinterpret_cast<DeviceProxy*>(a1),
                    static_cast<UINT>(a2), static_cast<D3D12_HEAP_TYPE>(static_cast<std::uint32_t>(a3))));
        }
        note_slot(Slot);
        // Xbox device slots, before the PC vtable is indexed: the public HLE's 78/79/80 would otherwise reach
        // CreateReservedResource2/CreateSampler2 or fall past an 80-slot table, and 48/73 would reach unrelated PC
        // methods. The check is before the slot-count refusal because slot 80 is outside a PC ID3D12Device11 table.
        std::uint64_t routed = 0;
        if (route_xbox_device_slot(self, Slot, a1, a2, a3, a4, a5, routed)) return routed;
        if (Slot >= self->slot_count) return hr_to_u64(refuse_beyond_device(Slot, self->slot_count));

        // The PC vtable entry, called with the PC device as `this`. Integer/pointer arguments, the hidden
        // struct-return pointer and any stack arguments keep their positions; XMM registers are untouched.
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
        return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }
}

template <std::size_t... I>
void fill_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&proxy_slot<static_cast<unsigned>(I)>) ), ...);
}

void** get_vtable() {
    static void** vtable = [] {
        static void* storage[kMaxSlots];
        fill_vtable(storage, std::make_index_sequence<kMaxSlots>{});
        return storage;
    }();
    return vtable;
}

bool supports(ID3D12Device* real, REFIID iid) {
    void* found = nullptr;
    const HRESULT hr = real->QueryInterface(iid, &found);
    if (FAILED(hr) || !found) return false;
    static_cast<IUnknown*>(found)->Release();
    return true;
}

// Highest device interface the PC runtime exposes; its cumulative slot count (from d3d12.h) bounds forwarding.
// Windows SDK 10.0.26100 defines ID3D12Device through ID3D12Device14 only; there is no Device15/Device16 header.
unsigned detect_slot_count(ID3D12Device* real) {
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device14))) return 83;
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device13))) return 82;
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device12))) return 81;
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device11))) return 80;
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device10))) return 79;
#endif
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device9))) return 76;
#endif
    if (supports(real, __uuidof(ID3D12Device8))) return 73;
    if (supports(real, __uuidof(ID3D12Device7))) return 68;
    if (supports(real, __uuidof(ID3D12Device6))) return 66;
    if (supports(real, __uuidof(ID3D12Device5))) return 65;
    if (supports(real, __uuidof(ID3D12Device4))) return 57;
    if (supports(real, __uuidof(ID3D12Device3))) return 51;
    if (supports(real, __uuidof(ID3D12Device2))) return 48;
    if (supports(real, __uuidof(ID3D12Device1))) return 47;
    return 44;
}

}  // namespace

HRESULT create_device_proxy(ID3D12Device* real, REFIID riid, void** out) {
    if (!real) return E_POINTER;
    if (!out) return E_POINTER;
    *out = nullptr;

    DeviceProxy* proxy = new (std::nothrow) DeviceProxy;
    if (!proxy) return E_OUTOFMEMORY;
    proxy->vtable = get_vtable();
    proxy->real = real;
    proxy->real_vtable = *reinterpret_cast<void***>(real);
    proxy->slot_count = detect_slot_count(real);
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy->dxgi_vtable = get_dxgi_vtable();
    proxy->adapter_provider = nullptr;
    proxy->buffer_resolver.store(nullptr, std::memory_order_relaxed);
    proxy->gpu_thread_priority.store(0, std::memory_order_relaxed);
    proxy->max_frame_latency.store(3, std::memory_order_relaxed);

    const unsigned slot_count = proxy->slot_count;
    HRESULT hr = proxy_query_interface(proxy, &riid, out);
    if (FAILED(hr)) {
        // No reference escaped, and the caller still owns its reference to `real`.
        delete proxy;
        return hr;
    }
    if (!d3d12_trace::enabled())
        std::fprintf(stderr, "[rt] d3d12_xs: device proxy %p wraps PC device %p (%u PC vtable slots)\n",
                     static_cast<void*>(proxy), static_cast<void*>(real), slot_count);
    proxy_release(proxy);  // drop the creation reference; `out` holds the caller's reference
    return S_OK;
}

bool is_device_proxy(const void* device) {
    return device && *reinterpret_cast<void* const*>(device) == reinterpret_cast<void*>(get_vtable());
}

void* device_proxy_from_dxgi_device(void* dxgi_device) {
    if (!dxgi_device || *static_cast<void***>(dxgi_device) != get_dxgi_vtable()) return nullptr;
    return dxgi_owner(dxgi_device);
}

void* device_proxy_primary(void* device) {
    if (is_device_proxy(device)) return device;
    return device_proxy_from_dxgi_device(device);
}

bool set_device_adapter_provider(void* device, DxgiAdapterProvider provider) {
    auto* proxy = static_cast<DeviceProxy*>(device_proxy_primary(device));
    if (!proxy) {
        // Not installing one silently would leave the host's adapter in its place.
        std::fprintf(stderr, "[rt] d3d12_xs: set_device_adapter_provider: %p is not a device proxy; no provider set\n",
                     device);
        return false;
    }
    proxy->adapter_provider = provider;
    return true;
}

bool set_device_buffer_at_va_resolver(void* device, BufferAtVaResolver resolver) {
    auto* proxy = static_cast<DeviceProxy*>(device_proxy_primary(device));
    if (!proxy) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: set_device_buffer_at_va_resolver: %p is not a device proxy; no resolver set\n",
                     device);
        return false;
    }
    proxy->buffer_resolver.store(resolver, std::memory_order_release);
    return true;
}

SrvAtVaOutcome last_srv_at_va_outcome() { return g_srv_at_va_last.load(std::memory_order_relaxed); }
std::uint64_t srv_at_va_created_calls() { return g_srv_at_va_created.load(std::memory_order_relaxed); }
std::uint64_t srv_at_va_refused_calls() { return g_srv_at_va_refused.load(std::memory_order_relaxed); }
void reset_srv_at_va_stats() {
    g_srv_at_va_last.store(SrvAtVaOutcome::none, std::memory_order_relaxed);
    g_srv_at_va_created.store(0, std::memory_order_relaxed);
    g_srv_at_va_refused.store(0, std::memory_order_relaxed);
}

bool is_heap_proxy(const void* heap) {
    return heap && *reinterpret_cast<void* const*>(heap) == reinterpret_cast<void*>(get_heap_vtable());
}

ID3D12DescriptorHeap* heap_proxy_real_heap(const void* heap) {
    if (!is_heap_proxy(heap)) return nullptr;
    return static_cast<const HeapProxy*>(heap)->real;
}

void set_descriptor_heap_allocator(const DescriptorHeapAllocator* allocator) {
    std::lock_guard lock(heap_allocator_mutex());
    g_has_explicit_heap_allocator = allocator != nullptr;
    g_explicit_heap_allocator = allocator ? *allocator : DescriptorHeapAllocator{};
}

void set_default_descriptor_heap_allocator(const DescriptorHeapAllocator* allocator) {
    std::lock_guard lock(heap_allocator_mutex());
    g_default_heap_allocator = allocator ? *allocator : DescriptorHeapAllocator{};
}

DescriptorHandleTranslation translate_cpu_descriptor_range(const void* device, D3D12_CPU_DESCRIPTOR_HANDLE start,
                                                           std::uint64_t count, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE* out) {
    const auto* proxy = static_cast<const DeviceProxy*>(device_proxy_primary(const_cast<void*>(device)));
    SIZE_T translated = start.ptr;
    const DescriptorHandleTranslation result = translate_handle(proxy, start.ptr, count, type, translated);
    if (out) out->ptr = translated;
    return result;
}

std::size_t title_descriptor_range_count() { return g_title_range_count.load(std::memory_order_acquire); }

std::size_t tracked_descriptor_heap_count() {
    std::lock_guard lock(descriptor_range_mutex());
    return descriptor_ranges().size();
}

std::uint64_t refused_descriptor_handle_calls() { return g_refused_handle_calls.load(std::memory_order_relaxed); }

unsigned device_proxy_slot_count(const void* device) {
    if (!is_device_proxy(device)) return 0;
    return static_cast<const DeviceProxy*>(device)->slot_count;
}

ID3D12Device* device_proxy_real_device(const void* device) {
    if (!is_device_proxy(device)) return nullptr;
    return static_cast<const DeviceProxy*>(device)->real;
}

void set_placed_at_va_handler(PlacedAtVaHandler handler) {
    g_placed_at_va.store(handler, std::memory_order_release);
}

bool device_slot_overridden(unsigned slot) {
    if (slot <= 2 || slot == 8 || slot == 9 || slot == 14 || (slot >= 17 && slot <= 24) || slot == 26 ||
        slot == 31 || slot == 32 || slot == 34 ||
        slot == 35 || slot == 36 || slot == 45 || slot == 46 || slot == 50 || slot == 62 || slot == 67 ||
        slot == 70 || slot == 75 || slot == 76 || slot == 133)
        return true;
    const XboxDeviceSlotRoutes& routes = xbox_device_slot_routes();
    return slot == routes.frame_interval || slot == routes.schedule_event || slot == routes.wait_event ||
           slot == routes.queue_x || slot == routes.list_x;
}

bool is_queue_proxy(const void* queue) { return is_child_proxy<ChildKind::queue>(queue); }

bool is_list_proxy(const void* list) { return is_list_proxy_impl(list); }

ID3D12GraphicsCommandList* list_proxy_real_list(const void* list) {
    return is_list_proxy_impl(list) ? static_cast<const ListProxy*>(list)->real : nullptr;
}

unsigned list_proxy_table_size() { return kListSlots; }

unsigned list_proxy_slot_count(const void* list) {
    return is_list_proxy_impl(list) ? static_cast<const ListProxy*>(list)->pc_slot_count : 0;
}

unsigned long list_proxy_ref_count(const void* list) {
    return is_list_proxy_impl(list) ? static_cast<const ListProxy*>(list)->refs.load(std::memory_order_relaxed) : 0;
}

unsigned list_proxy_unsupported_slot(const void* list) { return list_unsupported_slot(list); }

std::uint64_t list_slot_hits(unsigned slot) {
    start_hit_counts();
    return slot < kListSlots ? ListStats::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool is_fence_proxy(const void* fence) { return is_child_proxy<ChildKind::fence>(fence); }
bool is_allocator_proxy(const void* allocator) { return is_child_proxy<ChildKind::allocator>(allocator); }

ID3D12CommandAllocator* allocator_proxy_real_allocator(const void* allocator) {
    if (!is_allocator_proxy(allocator)) return nullptr;
    return reinterpret_cast<ID3D12CommandAllocator*>(static_cast<const ChildProxy*>(allocator)->real);
}

ID3D12CommandQueue* queue_proxy_real_queue(const void* queue) {
    if (!is_queue_proxy(queue)) return nullptr;
    return reinterpret_cast<ID3D12CommandQueue*>(static_cast<const ChildProxy*>(queue)->real);
}

ID3D12Fence* fence_proxy_real_fence(const void* fence) {
    if (!is_fence_proxy(fence)) return nullptr;
    return reinterpret_cast<ID3D12Fence*>(static_cast<const ChildProxy*>(fence)->real);
}

unsigned queue_proxy_slot_count(const void* queue) {
    return is_queue_proxy(queue) ? static_cast<const ChildProxy*>(queue)->slot_count : 0;
}

unsigned fence_proxy_slot_count(const void* fence) {
    return is_fence_proxy(fence) ? static_cast<const ChildProxy*>(fence)->slot_count : 0;
}

unsigned queue_proxy_table_size() { return kQueueSlots; }
unsigned fence_proxy_table_size() { return kFenceSlots; }
unsigned allocator_proxy_table_size() { return kAllocatorSlots; }
unsigned allocator_proxy_slot_count(const void* allocator) {
    return is_allocator_proxy(allocator) ? static_cast<const ChildProxy*>(allocator)->slot_count : 0;
}

unsigned long command_object_proxy_ref_count(const void* object) {
    if (!is_queue_proxy(object) && !is_fence_proxy(object) && !is_allocator_proxy(object)) return 0;
    return static_cast<const ChildProxy*>(object)->refs.load(std::memory_order_relaxed);
}

std::uint64_t queue_slot_hits(unsigned slot) {
    using S = ChildStats<ChildKind::queue>;
    start_hit_counts();
    return slot < S::kSlots ? S::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool queue_slot_first_call_seen(unsigned slot) {
    using S = ChildStats<ChildKind::queue>;
    return slot < S::kSlots && S::first[slot].load(std::memory_order_relaxed);
}

std::uint64_t fence_slot_hits(unsigned slot) {
    using S = ChildStats<ChildKind::fence>;
    start_hit_counts();
    return slot < S::kSlots ? S::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool fence_slot_first_call_seen(unsigned slot) {
    using S = ChildStats<ChildKind::fence>;
    return slot < S::kSlots && S::first[slot].load(std::memory_order_relaxed);
}

std::uint64_t queue_table_overruns() {
    return ChildStats<ChildKind::queue>::overruns.load(std::memory_order_relaxed);
}

std::uint64_t fence_table_overruns() {
    return ChildStats<ChildKind::fence>::overruns.load(std::memory_order_relaxed);
}

bool command_object_tables_guarded() {
    get_child_vtable<ChildKind::queue>();
    get_child_vtable<ChildKind::fence>();
    get_child_vtable<ChildKind::allocator>();
    return ChildStats<ChildKind::queue>::table_end.load(std::memory_order_acquire) &&
           ChildStats<ChildKind::fence>::table_end.load(std::memory_order_acquire) &&
           ChildStats<ChildKind::allocator>::table_end.load(std::memory_order_acquire);
}

std::uint64_t allocator_slot_hits(unsigned slot) {
    using S = ChildStats<ChildKind::allocator>;
    start_hit_counts();
    return slot < S::kSlots ? S::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool allocator_slot_first_call_seen(unsigned slot) {
    using S = ChildStats<ChildKind::allocator>;
    return slot < S::kSlots && S::first[slot].load(std::memory_order_relaxed);
}

std::uint64_t allocator_table_overruns() {
    return ChildStats<ChildKind::allocator>::overruns.load(std::memory_order_relaxed);
}

bool heap_slot_overridden(unsigned slot) {
    return slot <= 2 || slot == 7 || slot == 9 || slot == 10;
}

std::uint64_t device_slot_hits(unsigned slot) {
    start_hit_counts();
    return slot < kMaxSlots ? g_hits[slot].load(std::memory_order_relaxed) : 0;
}

bool device_slot_first_call_seen(unsigned slot) {
    return slot < kMaxSlots && g_first[slot].load(std::memory_order_relaxed);
}

unsigned long device_proxy_ref_count(const void* device) {
    if (!is_device_proxy(device)) return 0;
    return static_cast<const DeviceProxy*>(device)->refs.load(std::memory_order_relaxed);
}

std::uint64_t heap_slot_hits(unsigned slot) {
    start_hit_counts();
    return slot < kHeapSlots ? g_heap_hits[slot].load(std::memory_order_relaxed) : 0;
}

bool heap_slot_first_call_seen(unsigned slot) {
    return slot < kHeapSlots && g_heap_first[slot].load(std::memory_order_relaxed);
}

void reset_device_proxy_stats() {
    start_hit_counts();
    for (unsigned slot = 0; slot < kMaxSlots; ++slot) {
        g_hits[slot].store(0, std::memory_order_relaxed);
        g_first[slot].store(false, std::memory_order_relaxed);
        g_beyond_warned[slot].store(false, std::memory_order_relaxed);
    }
    for (unsigned slot = 0; slot < kHeapSlots; ++slot) {
        g_heap_hits[slot].store(0, std::memory_order_relaxed);
        g_heap_first[slot].store(false, std::memory_order_relaxed);
    }
    reset_child_stats<ChildKind::queue>();
    reset_child_stats<ChildKind::fence>();
    reset_child_stats<ChildKind::allocator>();
    for (unsigned slot = 0; slot < kListSlots; ++slot) {
        ListStats::hits[slot].store(0, std::memory_order_relaxed);
        ListStats::first[slot].store(false, std::memory_order_relaxed);
        ListStats::unsupported_warned[slot].store(false, std::memory_order_relaxed);
    }
}

}  // namespace rt::d3d12_xs
