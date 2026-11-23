// Xbox-ABI queue and fence proxies (d3d12_xs_device.h) over synthetic COM objects, with no D3D12 runtime and no GPU:
//   - devices that stop at ID3D12Device, Device1, Device3 and Device9: the slots of a missing interface (45/46,
//     50, 75) are refused without reaching the device, the others get the real objects behind the proxies;
//   - fences with and without ID3D12Fence1: 11 or 12 public slots, QueryInterface and slot 11 to match;
//   - creation that fails, that returns S_OK with no object, a null IID or output, and an interface the real object
//     has but the proxy does not model;
//   - OpenSharedHandle of a fence (as ID3D12Fence, IUnknown, Fence1), of a non-fence object, of an invalid handle,
//     and with a null output or IID, with the references of every object it touches;
//   - allocation failures in the proxy (operator new, the arrays it unwraps), injected in this executable only;
//   - device slot 26 called the console's way (the struct's buffer in RCX, the device in RDX) and the PC way;
//   - device slot 25 the same, with the description array as the stack word, passed through unchanged (--slot25 too);
//   - console command lists from device slot 73: the object body, the console->PC slot table, ownership, unwrapping
//     (allocator, bundle, heap array, queue ExecuteCommandLists), unsupported slots failing Close and refusing the
//     submission until Reset, AllocateUploadMemory over a mapped mock buffer, and the unwrapped PC path of slot 12;
//   - --fail-virtualalloc / --fail-virtualprotect / --fail-veh: the guard region's setup failing, injected by
//     patching this executable's own import table before the first proxy table is built (one process each);
//   - device slot 70 as the console calls it (resource desc, 64-byte view desc with the GPU VA at +0x28, CPU handle):
//     a raw view of the live buffer through PC slot 18 with the translated handle, never PC slot 70; every refusal
//     (shape, range, destination, VA, resolver, unreadable bytes) writes nothing and leaves no reference. --slot70 runs
//     it with the D3D12 trace on; --env-slots-70 moves a configured route onto 70, which must win;
//   - --resource-routes: device slots 92-95, 108-110 and 112 to ID3D12Device4/ID3D12Device8 through separate
//     queried interface objects (95/108 in the console and the PC struct-return form), and their explicit refusals;
//   - --host-import-experiment on|off: device slot 133's opt-in experiment (PALMWAVE_EXPERIMENTAL_XBOX_HOST_IMPORT, set
//     by CTest for the process, never by the test): the one experimental tuple reaches the handler as UAV / COMMON
//     only when the variable is "1"; every near miss is refused before the handler either way.
//   - device slot 127 (ten words, ID3D12Device10::CreateCommittedResource3): every word reaches the PC slot 76 of the
//     interface pointer QueryInterface returned, unchanged (unknown flags included, refused by the mock backend);
//     --slot127 runs it with the D3D12 trace on; --env-slots-127 moves a configured route onto 127, which must win.
// Every device, queue, fence and resource here is a mock that records the calls it gets. No game data, no Xbox DLLs.
#include "../shims/d3d12_xs_device.h"
#include "../shims/d3d12_trace.h"
#include "../shims/experimental_host_import.h"

#include <d3d12.h>
#include <windows.h>

#include <crtdbg.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <new>
#include <utility>

namespace {

const char* g_label = "xs_proxy_mock";

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "%s line %d: %s\n", g_label, __LINE__, #condition); return 1; } \
} while (0)

// ---- Allocation failures: this executable's global operator new fails the next g_fail_news calls ----

int g_fail_news = 0;     // single-threaded: armed right before the call it is meant for
int g_failed_news = 0;   // allocations refused so far

}  // namespace

// The annotations are vcruntime_new.h's, so /analyze sees the same contract as for the CRT's own operator new.
_NODISCARD _Ret_notnull_ _Post_writable_byte_size_(size) _VCRT_ALLOCATOR
void* __CRTDECL operator new(std::size_t size) {
    if (g_fail_news > 0) {
        --g_fail_news;
        ++g_failed_news;
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
_NODISCARD _Ret_maybenull_ _Success_(return != NULL) _Post_writable_byte_size_(size) _VCRT_ALLOCATOR
void* __CRTDECL operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}
void __CRTDECL operator delete(void* p) noexcept { std::free(p); }
void __CRTDECL operator delete(void* p, std::size_t) noexcept { std::free(p); }
void __CRTDECL operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

namespace xs = rt::d3d12_xs;

// ---- Mocks ----

// command_list answers ID3D12GraphicsCommandList only; list answers the whole chain through
// ID3D12GraphicsCommandList8 (the Vulkan layer's shape), so the console IID's ID3D12GraphicsCommandList6 and the
// GCL7 Barrier are reachable.
enum class Kind { device, queue, fence, resource, allocator, command_list, list, heap };

constexpr unsigned kMockSlots = 192;
constexpr std::uint64_t kMockGpuBase = 0x00007f0000000000ull;  // mock buffers' GPU addresses, unlike any CPU one
// The console command-list IID the 1.0.2.1 executable passes to device slot 73 (d3d12_xs_command_list_iid.h).
constexpr GUID kConsoleListIid{0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
constexpr std::uint64_t kFenceHandle = 0x1000, kResourceHandle = 0x2000;  // the handles the mock device opens
constexpr std::uint64_t kCompletedValue = 7;                            // what a mock fence's slot 8 returns
constexpr UINT kHeapMarker = 0x26262626;  // the VisibleNodeMask a mock device's slot 26 answers
// Mock descriptor heaps' real CPU/GPU starts (pool index << 24 apart) and the title backings handed out below: fake
// addresses in disjoint regions, never dereferenced by the proxy.
constexpr std::uint64_t kMockHeapCpuBase = 0x0000500000000000ull;
constexpr std::uint64_t kMockHeapGpuBase = 0x0000580000000000ull;
// {5d1e0c4a-2b7f-4e61-9a3d-8c4b2f1e6a70}: an interface the mock queue implements that the proxy does not model.
constexpr GUID kExtraIid{0x5d1e0c4a, 0x2b7f, 0x4e61, {0x9a, 0x3d, 0x8c, 0x4b, 0x2f, 0x1e, 0x6a, 0x70}};

// A separate ID3D12Device4 or ID3D12Device8 interface object of a mock device (section 11): QueryInterface for that
// IID returns this, not the device, so a route that used the base pointer or never released would show. Only the PC
// slots of the routes are typed entries; every other slot counts as a stray call.
struct RouteAlias {
    void** vtable;
    long refs;                     // 1, held by the test; every route's query must give its reference back
    unsigned calls[kMockSlots];    // per PC slot entered on this object
    unsigned total_calls;          // every entry but AddRef/Release
    std::uint64_t words[8];        // the words after `this` of the last call (the hidden buffer first for 56/68)
    std::uint8_t desc_bytes[2][sizeof(D3D12_RESOURCE_DESC1)];  // descriptions as they arrived, padding included
    HRESULT result;                // what the next create answers (S_OK makes an object)
    void* made;                    // the object the last successful create wrote
    D3D12_RESOURCE_ALLOCATION_INFO answer;  // 56/68
    D3D12_RESOURCE_ALLOCATION_INFO1 per[2];  // 56/68, written for the first two resources
};

struct Mock {
    void** vtable;
    Kind kind;
    long refs;
    int device_level;        // device: highest ID3D12DeviceN it answers (0 = ID3D12Device only)
    bool fence1;             // fence: answers ID3D12Fence1; device: the fences it makes do
    bool allocator_tag;      // allocator: answers kPalmwaveCommandAllocatorTag; device: the allocators it makes do
    bool full_chain_lists;   // device: CreateCommandList makes a Kind::list mock, else Kind::command_list
    void* mapped;            // resource from CreateCommittedResource: the memory Map returns, freed at refs 0
    std::uint64_t gpu_va;    // resource from CreateCommittedResource: what GetGPUVirtualAddress returns
    bool create_null;       // device: creation returns create_result with no object
    HRESULT create_result;   // device: what creation returns (S_OK makes the object)
    Mock* last_child;        // device: the object its last creation or open made
    // device: QueryInterface(ID3D12Device10) is counted, refused when refuse_device10 is set, and answered with
    // device10_alias (a separate interface object) when that is set.
    unsigned device10_queries;
    bool refuse_device10;
    Mock* device10_alias;
    bool validate_queue_desc;  // device: opt in to the PC queue-description contract
    bool has_queue_desc;
    D3D12_COMMAND_QUEUE_DESC last_queue_desc;  // snapshot before a forwarded local copy expires
    // list: the ID3D12GraphicsCommandList7 QueryInterface (counted; refused, or answered with another object), what
    // Close (slot 9) returns, and the first group/barrier of the last Barrier (GCL7 slot 80) as it arrived.
    unsigned gcl7_queries;
    bool refuse_gcl7;
    Mock* gcl7_alias;
    HRESULT close_result;
    std::uint64_t barrier_groups;
    D3D12_BARRIER_GROUP barrier_group;
    D3D12_GLOBAL_BARRIER barrier_global;
    // heap: what slots 9/10 answer (distinct per mock heap, never dereferenced). device slot 23: the first source
    // handles of CopyDescriptors as received (its destinations go to array[23]). list slot 46: the DSV handle value
    // behind OMSetRenderTargets' pointer, as received.
    std::uint64_t heap_cpu;
    std::uint64_t heap_gpu;
    std::uint64_t copy_src[4];
    std::uint64_t om_dsv;
    unsigned calls[kMockSlots];
    std::uint64_t args[kMockSlots][5];
    std::uint64_t array[kMockSlots][4];  // device: the first entries of the array argument, as received
    // device slot 18 (CreateShaderResourceView): the description as it arrived (the caller's copy is temporary).
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;
    // device: the separate objects QueryInterface returns for ID3D12Device4 / ID3D12Device8 (null: the device itself).
    RouteAlias* device4_alias;
    RouteAlias* device8_alias;
};

Mock g_pool[512];
unsigned g_pool_used = 0;

void** mock_vtable();

Mock* new_mock(Kind kind) {
    if (g_pool_used == std::size(g_pool)) {
        std::fprintf(stderr, "%s: mock pool exhausted\n", g_label);
        std::exit(1);
    }
    Mock* m = &g_pool[g_pool_used++];
    std::memset(m, 0, sizeof(*m));
    m->vtable = mock_vtable();
    m->kind = kind;
    return m;
}

bool mock_has(const Mock* m, REFIID iid) {
    if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object))) return true;
    if (m->kind == Kind::device) {
        if (IsEqualIID(iid, __uuidof(ID3D12Device))) return true;
        const IID levels[] = {__uuidof(ID3D12Device1), __uuidof(ID3D12Device2), __uuidof(ID3D12Device3),
                              __uuidof(ID3D12Device4), __uuidof(ID3D12Device5), __uuidof(ID3D12Device6),
                              __uuidof(ID3D12Device7), __uuidof(ID3D12Device8), __uuidof(ID3D12Device9),
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device10),
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device11),
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device12),
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device13),
#endif
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device14),
#endif
        };
        for (int i = 0; i < m->device_level && i < static_cast<int>(std::size(levels)); ++i)
            if (IsEqualIID(iid, levels[i])) return true;
        return false;
    }
    if (IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Pageable))) return true;
    switch (m->kind) {
        case Kind::queue: return IsEqualIID(iid, __uuidof(ID3D12CommandQueue)) || IsEqualIID(iid, kExtraIid);
        case Kind::fence:
            return IsEqualIID(iid, __uuidof(ID3D12Fence)) || (m->fence1 && IsEqualIID(iid, __uuidof(ID3D12Fence1)));
        case Kind::resource: return IsEqualIID(iid, __uuidof(ID3D12Resource));
        case Kind::command_list:
            return IsEqualIID(iid, __uuidof(ID3D12CommandList)) ||
                   IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList));
        case Kind::allocator:
            return IsEqualIID(iid, __uuidof(ID3D12CommandAllocator)) ||
                   (m->allocator_tag && IsEqualIID(iid, xs::kPalmwaveCommandAllocatorTag));
        case Kind::list: {
            const IID lists[] = {__uuidof(ID3D12CommandList),          __uuidof(ID3D12GraphicsCommandList),
                                 __uuidof(ID3D12GraphicsCommandList1), __uuidof(ID3D12GraphicsCommandList2),
                                 __uuidof(ID3D12GraphicsCommandList3), __uuidof(ID3D12GraphicsCommandList4),
                                 __uuidof(ID3D12GraphicsCommandList5), __uuidof(ID3D12GraphicsCommandList6),
                                 __uuidof(ID3D12GraphicsCommandList7), __uuidof(ID3D12GraphicsCommandList8)};
            for (const IID& list_iid : lists)
                if (IsEqualIID(iid, list_iid)) return true;
            return false;
        }
        case Kind::heap: return IsEqualIID(iid, __uuidof(ID3D12DescriptorHeap));
        default: return false;
    }
}

HRESULT mock_query(Mock* m, const IID* riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (riid && m->kind == Kind::list && IsEqualIID(*riid, __uuidof(ID3D12GraphicsCommandList7))) {
        ++m->gcl7_queries;
        if (m->refuse_gcl7) return E_NOINTERFACE;
        if (m->gcl7_alias) {  // a separate interface object: the caller must use (and release) this pointer
            *out = m->gcl7_alias;
            ++m->gcl7_alias->refs;
            return S_OK;
        }
    }
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (riid && m->kind == Kind::device && IsEqualIID(*riid, __uuidof(ID3D12Device10))) {
        ++m->device10_queries;
        if (m->refuse_device10) return E_NOINTERFACE;
        if (m->device10_alias && mock_has(m, *riid)) {
            *out = m->device10_alias;
            ++m->device10_alias->refs;
            return S_OK;
        }
    }
#endif
    if (!riid || !mock_has(m, *riid)) return E_NOINTERFACE;
    if (m->kind == Kind::device) {
        RouteAlias* alias = IsEqualIID(*riid, __uuidof(ID3D12Device4))   ? m->device4_alias
                            : IsEqualIID(*riid, __uuidof(ID3D12Device8)) ? m->device8_alias
                                                                         : nullptr;
        if (alias) {
            *out = alias;
            ++alias->refs;
            return S_OK;
        }
    }
    *out = m;
    ++m->refs;
    return S_OK;
}

// A device creating (or opening) a `kind` object as `riid`: S_FALSE without an output, as the PC runtime answers a
// capability check; the device's create_result and create_null otherwise.
HRESULT mock_create(Mock* device, Kind kind, std::uint64_t riid, std::uint64_t out_word) {
    auto** out = reinterpret_cast<void**>(out_word);
    if (!out) return S_FALSE;
    *out = nullptr;
    if (device->create_null || FAILED(device->create_result)) return device->create_result;
    Mock* child = new_mock(kind);
    child->fence1 = device->fence1;
    child->allocator_tag = device->allocator_tag;
    if (kind == Kind::heap) {
        const auto index = static_cast<std::uint64_t>(child - g_pool);
        child->heap_cpu = kMockHeapCpuBase + (index << 24);
        child->heap_gpu = kMockHeapGpuBase + (index << 24);
    }
    device->last_child = child;
    return mock_query(child, reinterpret_cast<const IID*>(riid), out);
}

// What the mock device's GetDescriptorHandleIncrementSize (slot 15) answers: distinct per type, so a span checked
// with the wrong type's increment shows.
UINT mock_increment(std::uint64_t type) {
    switch (static_cast<std::uint32_t>(type)) {
        case D3D12_DESCRIPTOR_HEAP_TYPE_RTV: return 64;
        case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: return 16;
        default: return 32;  // CBV_SRV_UAV and SAMPLER
    }
}

void copy_array(Mock* m, unsigned slot, std::uint64_t objects, std::uint64_t count) {
    const auto* in = reinterpret_cast<const std::uint64_t*>(objects);
    for (std::uint32_t i = 0; in && i < static_cast<std::uint32_t>(count) && i < 4; ++i) m->array[slot][i] = in[i];
}

// `words` are the call's first five words (a1..a5), copied into the thunk's own frame.
std::uint64_t mock_call(Mock* m, unsigned slot, const std::uint64_t* words) {
    const std::uint64_t a1 = words[0], a2 = words[1], a3 = words[2], a4 = words[3];
    if (m->kind == Kind::device) {
        // CreateShaderResourceView(resource, desc, handle): small test words in a2 are markers, not descriptions.
        if (slot == 18) {
            std::memset(&m->srv, 0, sizeof(m->srv));
            if (a2 > 0xFFFF) std::memcpy(&m->srv, reinterpret_cast<const void*>(a2), sizeof(m->srv));
        }
        switch (slot) {
            case 8: {  // CreateCommandQueue: capture values, never retain a temporary description.
                m->has_queue_desc = a1 != 0;
                if (a1) m->last_queue_desc = *reinterpret_cast<const D3D12_COMMAND_QUEUE_DESC*>(a1);
                if (m->validate_queue_desc &&
                    (!a1 || (m->last_queue_desc.Flags & ~D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT))) {
                    if (a3) *reinterpret_cast<void**>(a3) = nullptr;
                    return static_cast<std::uint32_t>(E_INVALIDARG);
                }
                return static_cast<std::uint32_t>(mock_create(m, Kind::queue, a2, a3));
            }
            case 9: return static_cast<std::uint32_t>(mock_create(m, Kind::allocator, a2, a3));  // CreateCmdAllocator
            case 14: return static_cast<std::uint32_t>(mock_create(m, Kind::heap, a2, a3));      // CreateDescriptorHeap
            case 15: return mock_increment(a1);                       // GetDescriptorHandleIncrementSize(type)
            case 23: {  // CopyDescriptors(numDest, destStarts, destSizes, numSrc, srcStarts, ...): copies the proxy owns
                copy_array(m, 23, a2, a1);
                const auto* src = reinterpret_cast<const std::uint64_t*>(words[4]);
                for (std::uint32_t i = 0; src && i < static_cast<std::uint32_t>(a4) && i < 4; ++i)
                    m->copy_src[i] = src[i];
                break;
            }
            case 75: return static_cast<std::uint32_t>(mock_create(m, Kind::queue, a3, a4));   // CreateCommandQueue1
            case 36: return static_cast<std::uint32_t>(mock_create(m, Kind::fence, a3, a4));   // CreateFence
            case 32:                                                                          // OpenSharedHandle
                if (a1 == kFenceHandle) return static_cast<std::uint32_t>(mock_create(m, Kind::fence, a2, a3));
                if (a1 == kResourceHandle) return static_cast<std::uint32_t>(mock_create(m, Kind::resource, a2, a3));
                return static_cast<std::uint32_t>(E_INVALIDARG);
            case 34: case 35: case 46: copy_array(m, slot, a2, a1); break;  // (count, objects, ...)
            case 45: copy_array(m, slot, a1, a3); break;                    // (fences, values, count, ...)
            case 26: {  // GetCustomHeapProperties, the PC method: (return buffer, node mask, heap type)
                auto* out = reinterpret_cast<D3D12_HEAP_PROPERTIES*>(a1);
                if (!out) break;
                *out = {};
                out->Type = static_cast<D3D12_HEAP_TYPE>(static_cast<std::uint32_t>(a3));
                out->CreationNodeMask = static_cast<UINT>(a2);
                out->VisibleNodeMask = kHeapMarker;
                return a1;  // a struct-returning method hands the buffer's address back in RAX
            }
            case 50: copy_array(m, slot, a3, a2); break;                    // (flags, count, objects, fence, value)
            default: break;
        }
        return S_OK;
    }
    if (m->kind == Kind::fence && slot == 8) return kCompletedValue;  // GetCompletedValue
    if (m->kind == Kind::heap && (slot == 9 || slot == 10)) {
        // GetCPU/GPUDescriptorHandleForHeapStart, the PC way: through the hidden return pointer (and in RAX too).
        const std::uint64_t value = slot == 9 ? m->heap_cpu : m->heap_gpu;
        if (a1) std::memcpy(reinterpret_cast<void*>(a1), &value, sizeof(value));
        return value;
    }
    if (m->kind == Kind::queue && slot == 10) copy_array(m, slot, a2, a1);  // ExecuteCommandLists(count, lists)
    if (m->kind == Kind::resource) {
        if (slot == 8) {  // Map(subresource, range, &data)
            if (a3) *reinterpret_cast<void**>(a3) = m->mapped;
            return static_cast<std::uint32_t>(m->mapped ? S_OK : E_FAIL);
        }
        if (slot == 11) return m->gpu_va;  // GetGPUVirtualAddress
    }
    if (m->kind == Kind::list || m->kind == Kind::command_list) {
        // The view setters get a copy the proxy owns only for the call, so what arrived is recorded now.
        if (slot == 28) copy_array(m, slot, a2, a1);  // SetDescriptorHeaps(count, heaps)
        if (slot == 43 && a1)                         // IASetIndexBuffer(view)
            m->array[slot][0] = reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(a1)->BufferLocation;
        if (slot == 44 && a3) {                       // IASetVertexBuffers(start, count, views)
            const auto* views = reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(a3);
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(a2) && i < 4; ++i)
                m->array[slot][i] = views[i].BufferLocation;
        }
        if (slot == 45 && a3) {                       // SOSetTargets(start, count, views)
            const auto* views = reinterpret_cast<const D3D12_STREAM_OUTPUT_BUFFER_VIEW*>(a3);
            m->array[slot][0] = views[0].BufferLocation;
            m->array[slot][1] = views[0].BufferFilledSizeLocation;
        }
        if (slot == 46) {                             // OMSetRenderTargets(count, rtvs, singleRange, dsv)
            const auto count = static_cast<std::uint32_t>(a1);
            const std::uint32_t entries = static_cast<std::uint32_t>(a3) ? 1 : count;
            copy_array(m, slot, a2, entries);
            m->om_dsv = a4 ? reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(a4)->ptr : 0;
        }
        if (slot == 80) {                             // GCL7 Barrier(groups, group): the caller's structs are temporary
            m->barrier_groups = a1;
            const auto* group = reinterpret_cast<const D3D12_BARRIER_GROUP*>(a2);
            if (group && static_cast<std::uint32_t>(a1) >= 1) {
                m->barrier_group = *group;
                if (group->Type == D3D12_BARRIER_TYPE_GLOBAL && group->NumBarriers >= 1 && group->pGlobalBarriers)
                    m->barrier_global = group->pGlobalBarriers[0];
            }
        }
        if (slot == 9) return static_cast<std::uint32_t>(m->close_result);  // Close
    }
    return S_OK;
}

template <unsigned Slot>
std::uint64_t WINAPI mock_slot(Mock* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4,
                               std::uint64_t a5) {
    if constexpr (Slot == 0) {
        return static_cast<std::uint32_t>(mock_query(self, reinterpret_cast<const IID*>(a1),
                                                     reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return static_cast<std::uint64_t>(++self->refs);
    } else if constexpr (Slot == 2) {
        const long refs = --self->refs;
        if (refs == 0 && self->mapped) {
            VirtualFree(self->mapped, 0, MEM_RELEASE);
            self->mapped = nullptr;
        }
        return static_cast<std::uint64_t>(refs);
    } else {
        // A caller may pass fewer words than this thunk declares (OpenSharedHandle passes three), so its frame
        // has no room past the four register homes. The words are copied into this frame and passed by pointer:
        // forwarding them as stack arguments let MSVC tail-call mock_call and write into the caller's saved
        // registers (an optimized build corrupted the test's own locals that way).
        ++self->calls[Slot];
        const std::uint64_t words[] = {a1, a2, a3, a4, a5};
        std::memcpy(self->args[Slot], words, sizeof(words));
        return mock_call(self, Slot, words);
    }
}

// PC ID3D12Device::CreateCommandList takes six words after `this`, one more than the generic mock thunk declares, so
// it gets its own entry in the table: the out pointer is a6 (first stack argument on x64) and has to be written.
HRESULT WINAPI mock_create_command_list(Mock* self, std::uint64_t node_mask, std::uint64_t type,
                                        std::uint64_t allocator, std::uint64_t initial_state, const IID* riid,
                                        void** out) {
    ++self->calls[12];
    self->args[12][0] = node_mask;
    self->args[12][1] = type;
    self->args[12][2] = allocator;
    self->args[12][3] = initial_state;
    self->args[12][4] = reinterpret_cast<std::uint64_t>(riid);
    if (!out) return S_FALSE;
    *out = nullptr;
    if (self->create_null || FAILED(self->create_result)) return self->create_result;
    Mock* child = new_mock(self->full_chain_lists ? Kind::list : Kind::command_list);
    self->last_child = child;
    return mock_query(child, riid, out);
}

// PC ID3D12Device::CreateCommittedResource takes seven words after `this`; the upload arena behind console list slot
// 148 calls it. The buffer is real memory (Map returns it) with a GPU address that differs from it, so a translated
// address is told apart from an untranslated one.
HRESULT WINAPI mock_create_committed_resource(Mock* self, const D3D12_HEAP_PROPERTIES* heap, std::uint64_t flags,
                                              const D3D12_RESOURCE_DESC* desc, std::uint64_t state,
                                              const D3D12_CLEAR_VALUE* clear, const IID* riid, void** out) {
    ++self->calls[27];
    if (self->kind != Kind::device) {
        // The mocks share one table: on a PC command list slot 27 is the void ExecuteBundle(bundle). Record the words
        // as received instead of reading them as a heap description; the return value is unchanged and carries no
        // contract for a void method.
        const std::uint64_t words[] = {reinterpret_cast<std::uint64_t>(heap), flags,
                                       reinterpret_cast<std::uint64_t>(desc), state,
                                       reinterpret_cast<std::uint64_t>(clear)};
        std::memcpy(self->args[27], words, sizeof(words));
        return out ? E_INVALIDARG : S_FALSE;
    }
    (void)state;
    (void)clear;
    self->args[27][0] = heap ? static_cast<std::uint64_t>(heap->Type) : 0;
    self->args[27][1] = desc ? desc->Width : 0;
    if (!out) return S_FALSE;
    *out = nullptr;
    if (self->create_null || FAILED(self->create_result)) return self->create_result;
    if (!desc) return E_INVALIDARG;
    Mock* child = new_mock(Kind::resource);
    child->mapped = VirtualAlloc(nullptr, static_cast<SIZE_T>(desc->Width), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!child->mapped) return E_OUTOFMEMORY;
    child->gpu_va = kMockGpuBase + (static_cast<std::uint64_t>(child - g_pool) << 32);
    self->last_child = child;
    return mock_query(child, riid, out);
}

// PC ID3D12GraphicsCommandList::ClearDepthStencilView(dsv, flags, FLOAT depth, UINT8 stencil, rectCount, rects) is
// slot 47: depth is the fourth argument, so it arrives in XMM3. This typed entry reads it from there, as the real
// method does, and records (dsv, flags, depth bits, stencil, rectCount) in args[47] and the rect pointer in
// array[47][0]. The mocks share one table; no test calls slot 47 on another kind, which only counts the call.
std::uint64_t WINAPI mock_clear_depth_stencil_view(Mock* self, std::uint64_t dsv, std::uint64_t flags, float depth,
                                                   std::uint64_t stencil, std::uint64_t rect_count,
                                                   std::uint64_t rects) {
    ++self->calls[47];
    if (self->kind != Kind::list && self->kind != Kind::command_list) return S_OK;
    std::uint32_t depth_bits = 0;
    std::memcpy(&depth_bits, &depth, sizeof(depth_bits));
    const std::uint64_t words[] = {dsv, static_cast<std::uint32_t>(flags), depth_bits, stencil & 0xff,
                                   static_cast<std::uint32_t>(rect_count)};
    std::memcpy(self->args[47], words, sizeof(words));
    self->array[47][0] = rects;
    return 0;
}

// PC slot 76: on a device, ID3D12Device10::CreateCommittedResource3(heap, flags, desc1, layout, clear, session,
// castableCount, castableFormats, riid, out), ten words after `this`. The call is recorded as it arrived (the UINT
// words as 32-bit values, the description's bytes padding included). Like the Vulkan backend, a flag bit outside the
// PC set (0x7FF) is refused with E_INVALIDARG; a null output is the capability check (S_FALSE). Every other kind
// shares the table and keeps the generic five-word behaviour.
struct Committed3Seen {
    unsigned calls;
    const Mock* device;  // the object the call arrived on
    const D3D12_HEAP_PROPERTIES* heap;
    std::uint32_t heap_flags;
    const void* desc;
    unsigned char desc_bytes[sizeof(D3D12_RESOURCE_DESC1)];
    std::uint32_t layout;
    const void* clear;
    const void* session;
    std::uint32_t castable_count;
    const void* castable;
    const IID* riid;
    void** out;
};
Committed3Seen g_committed3;

std::uint64_t WINAPI mock_slot76(Mock* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4,
                                 std::uint64_t a5, std::uint64_t a6, std::uint64_t a7, std::uint64_t a8,
                                 std::uint64_t a9, std::uint64_t a10) {
    if (self->kind != Kind::device) return mock_slot<76>(self, a1, a2, a3, a4, a5);
    ++self->calls[76];
    Committed3Seen& s = g_committed3;
    ++s.calls;
    s.device = self;
    s.heap = reinterpret_cast<const D3D12_HEAP_PROPERTIES*>(a1);
    s.heap_flags = static_cast<std::uint32_t>(a2);
    s.desc = reinterpret_cast<const void*>(a3);
    if (a3) std::memcpy(s.desc_bytes, s.desc, sizeof(s.desc_bytes));
    s.layout = static_cast<std::uint32_t>(a4);
    s.clear = reinterpret_cast<const void*>(a5);
    s.session = reinterpret_cast<const void*>(a6);
    s.castable_count = static_cast<std::uint32_t>(a7);
    s.castable = reinterpret_cast<const void*>(a8);
    s.riid = reinterpret_cast<const IID*>(a9);
    s.out = reinterpret_cast<void**>(a10);
    if (!s.out) return S_FALSE;
    *s.out = nullptr;
    if (!a3 || (static_cast<std::uint32_t>(reinterpret_cast<const D3D12_RESOURCE_DESC1*>(a3)->Flags) & ~0x7FFu))
        return static_cast<std::uint32_t>(E_INVALIDARG);
    return static_cast<std::uint32_t>(mock_create(self, Kind::resource, a9, a10));
}

// PC ID3D12Device::GetResourceAllocationInfo (slot 25) as the MSVC C++ method is called: `this`, the hidden return
// buffer, the visible node mask, the count, and the description array as the first stack word. The answer is set per
// test; the descriptions are copied as they arrived. Other mock kinds keep the generic entry.
struct AllocInfoCall {
    int calls = 0;
    Mock* self = nullptr;
    void* out = nullptr;
    std::uint64_t mask = 0, count = 0;
    const D3D12_RESOURCE_DESC* descs = nullptr;
    D3D12_RESOURCE_DESC seen[2]{};
    D3D12_RESOURCE_ALLOCATION_INFO answer{};
} g_alloc_info;

std::uint64_t WINAPI mock_get_resource_allocation_info(Mock* self, std::uint64_t out, std::uint64_t mask,
                                                       std::uint64_t count, std::uint64_t descs, std::uint64_t a5) {
    if (self->kind != Kind::device) return mock_slot<25>(self, out, mask, count, descs, a5);
    ++self->calls[25];
    ++g_alloc_info.calls;
    g_alloc_info.self = self;
    g_alloc_info.out = reinterpret_cast<void*>(out);
    g_alloc_info.mask = mask;
    g_alloc_info.count = count;
    g_alloc_info.descs = reinterpret_cast<const D3D12_RESOURCE_DESC*>(descs);
    for (std::uint32_t i = 0; descs && i < static_cast<std::uint32_t>(count) && i < 2; ++i)
        std::memcpy(&g_alloc_info.seen[i], &g_alloc_info.descs[i], sizeof(D3D12_RESOURCE_DESC));  // padding included
    std::memcpy(reinterpret_cast<void*>(out), &g_alloc_info.answer, sizeof(g_alloc_info.answer));
    return out;
}

template <std::size_t... I>
void fill_mock_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&mock_slot<static_cast<unsigned>(I)>)), ...);
}

void** mock_vtable() {
    static void* table[kMockSlots];
    static const bool filled = [] {
        fill_mock_vtable(table, std::make_index_sequence<kMockSlots>{});
        table[12] = reinterpret_cast<void*>(&mock_create_command_list);
        table[25] = reinterpret_cast<void*>(&mock_get_resource_allocation_info);
        table[27] = reinterpret_cast<void*>(&mock_create_committed_resource);
        table[47] = reinterpret_cast<void*>(&mock_clear_depth_stencil_view);
        table[76] = reinterpret_cast<void*>(&mock_slot76);
        return true;
    }();
    (void)filled;
    return table;
}

// ---- Calls by slot, the way a title compiled against the console headers makes them ----

using SlotFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

std::uint64_t raw(const void* object, unsigned slot, std::uint64_t a1 = 0, std::uint64_t a2 = 0,
                  std::uint64_t a3 = 0, std::uint64_t a4 = 0, std::uint64_t a5 = 0, std::uint64_t a6 = 0) {
    void* const* vtable = *reinterpret_cast<void* const* const*>(object);
    return reinterpret_cast<SlotFn>(vtable[slot])(const_cast<void*>(object), a1, a2, a3, a4, a5, a6, 0, 0, 0, 0, 0,
                                                  0, 0, 0);
}

// A call with exactly ten words after `this` (device slot 127): the caller's frame ends after the seventh stack word.
using Slot10Fn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                        std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                        std::uint64_t);
std::uint64_t raw10(const void* object, unsigned slot, const std::uint64_t (&w)[10]) {
    void* const* vtable = *reinterpret_cast<void* const* const*>(object);
    return reinterpret_cast<Slot10Fn>(vtable[slot])(const_cast<void*>(object), w[0], w[1], w[2], w[3], w[4], w[5],
                                                    w[6], w[7], w[8], w[9]);
}

HRESULT hr_of(std::uint64_t value) {
    return static_cast<HRESULT>(static_cast<std::uint32_t>(value));
}

template <class T>
std::uint64_t word(const T* p) {
    return reinterpret_cast<std::uint64_t>(p);
}

const IID kQueueIid = __uuidof(ID3D12CommandQueue);
const IID kFenceIid = __uuidof(ID3D12Fence);
const IID kFence1Iid = __uuidof(ID3D12Fence1);
const IID kUnknownIid = __uuidof(IUnknown);
const IID kResourceIid = __uuidof(ID3D12Resource);
const IID kDevice1Iid = __uuidof(ID3D12Device1);
const D3D12_COMMAND_QUEUE_DESC kQueueDesc{};

// A device proxy over a new mock device. The proxy takes the mock's one reference.
void* make_device(Mock*& mock, int level, bool fence1, bool allocator_tag = true, bool full_chain_lists = false) {
    mock = new_mock(Kind::device);
    mock->device_level = level;
    mock->fence1 = fence1;
    mock->allocator_tag = allocator_tag;
    mock->full_chain_lists = full_chain_lists;
    mock->refs = 1;
    void* device = nullptr;
    if (FAILED(xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(mock), __uuidof(ID3D12Device), &device)))
        return nullptr;
    return device;
}

void* make_queue(void* device) {
    void* queue = nullptr;
    return SUCCEEDED(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&queue)))) ? queue : nullptr;
}

void* make_fence(void* device, const IID& iid = kFenceIid) {
    void* fence = nullptr;
    return SUCCEEDED(hr_of(raw(device, 36, 0, 0, word(&iid), word(&fence)))) ? fence : nullptr;
}

unsigned long release(void* object) {
    return static_cast<unsigned long>(raw(object, 2));
}

// Slot 133 is callable even on a base PC device, but never fabricates a placed resource or calls a host slot.
int check_placed_resource_refusal() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 0, false);
    EXPECT(device != nullptr);
    struct Output { std::uint64_t before; void* value; std::uint64_t after; };
    Output out{0x1122334455667788ull, reinterpret_cast<void*>(1), 0x8877665544332211ull};
    const auto hits = xs::device_slot_hits(133);
    const auto refs = xs::device_proxy_ref_count(device);
    const auto host_refs = mock->refs;
    rt::d3d12_trace::set_enabled(true);
    // Deliberately unreadable input pointers: a refused operation must not dereference them. Only out is writable.
    const HRESULT result = hr_of(raw(device, 133, 0x40000000, 1, 0xFFFFFFFFull, 1, 1, word(&out.value)));
    rt::d3d12_trace::set_enabled(false);
    EXPECT(result == E_NOTIMPL && out.value == nullptr);
    EXPECT(out.before == 0x1122334455667788ull && out.after == 0x8877665544332211ull);
    EXPECT(xs::device_slot_hits(133) == hits + 1 && xs::device_slot_first_call_seen(133));
    EXPECT(xs::device_slot_overridden(133) && xs::device_proxy_ref_count(device) == refs && mock->refs == host_refs);
    EXPECT(hr_of(raw(device, 133)) == E_NOTIMPL);  // null output is safe on an unsupported method
    EXPECT(hr_of(raw(device, 191)) == E_NOTIMPL);  // the expanded tail also has a callable refusal
    EXPECT(mock->calls[133] == 0 && mock->calls[191] == 0);
    EXPECT(release(device) == 0);
    std::printf("slot 133: unsupported, output cleared, canaries/references intact, no host placement call\n");
    return 0;
}

// ---- 1. Device interface levels ----

int check_levels() {
    struct Level { int level; unsigned slots; } const levels[] = {{0, 44}, {1, 47}, {3, 51}, {9, 76}};
    for (const Level& l : levels) {
        Mock* mock = nullptr;
        void* device = make_device(mock, l.level, true);
        EXPECT(device && xs::device_proxy_slot_count(device) == l.slots);
        void* queue = make_queue(device);
        void* fence = make_fence(device);
        EXPECT(xs::is_queue_proxy(queue) && xs::is_fence_proxy(fence));
        const auto real_queue = word(xs::queue_proxy_real_queue(queue));
        const auto real_fence = word(xs::fence_proxy_real_fence(fence));
        EXPECT(real_queue && real_fence && real_queue != word(queue) && real_fence != word(fence));
        // What every residency call gets, with the proxies in it replaced by their real objects.
        const std::uint64_t objects[4] = {word(queue), word(fence), real_queue, 0};
        const std::uint64_t unwrapped[4] = {real_queue, real_fence, real_queue, 0};
        const std::uint64_t fences[2] = {word(fence), word(fence)};
        const std::uint64_t values[2] = {1, 2};
        const D3D12_RESIDENCY_PRIORITY priorities[4] = {};
        const auto same = [](const std::uint64_t* got, const std::uint64_t* want, int n) {
            return std::memcmp(got, want, n * sizeof(std::uint64_t)) == 0;
        };

        // 34 MakeResident, with a count whose high dword is not zero: the PC prototype takes a UINT.
        EXPECT(hr_of(raw(device, 34, 0xdeadbeef00000002ull, word(objects))) == S_OK);
        EXPECT(mock->calls[34] == 1 && static_cast<std::uint32_t>(mock->args[34][0]) == 2);
        EXPECT(same(mock->array[34], unwrapped, 2) && mock->array[34][2] == 0);
        // 31 CreateSharedHandle
        EXPECT(hr_of(raw(device, 31, word(fence), 0, GENERIC_ALL, 0, 0)) == S_OK);
        EXPECT(mock->calls[31] == 1 && mock->args[31][0] == real_fence);

        // 45/46: ID3D12Device1
        const HRESULT r45 = hr_of(raw(device, 45, word(fences), word(values), 2, 0, 0));
        const HRESULT r46 = hr_of(raw(device, 46, 4, word(objects), word(priorities)));
        if (l.level >= 1) {
            EXPECT(r45 == S_OK && mock->calls[45] == 1 && mock->array[45][0] == real_fence &&
                   mock->array[45][1] == real_fence);
            EXPECT(r46 == S_OK && mock->calls[46] == 1 && same(mock->array[46], unwrapped, 4));
        } else {
            EXPECT(r45 == E_NOTIMPL && r46 == E_NOTIMPL && mock->calls[45] == 0 && mock->calls[46] == 0);
            void* device1 = nullptr;
            EXPECT(FAILED(hr_of(raw(device, 0, word(&kDevice1Iid), word(&device1)))) && !device1);
        }
        // 50: ID3D12Device3, with the fence argument unwrapped too
        const HRESULT r50 = hr_of(raw(device, 50, 0, 4, word(objects), word(fence), 1));
        if (l.level >= 3) {
            EXPECT(r50 == S_OK && mock->calls[50] == 1 && same(mock->array[50], unwrapped, 4) &&
                   mock->args[50][3] == real_fence);
        } else {
            EXPECT(r50 == E_NOTIMPL && mock->calls[50] == 0);
        }
        // 75: ID3D12Device9::CreateCommandQueue1
        void* queue1 = reinterpret_cast<void*>(1);  // cleared on every failure (bc59ef46's convention)
        const HRESULT r75 = hr_of(raw(device, 75, word(&kQueueDesc), 0, word(&kQueueIid), word(&queue1)));
        if (l.level >= 9) {
            EXPECT(r75 == S_OK && xs::is_queue_proxy(queue1) && mock->calls[75] == 1);
            EXPECT(release(queue1) == 0 && mock->last_child->refs == 0);
        } else {
            EXPECT(r75 == E_NOTIMPL && !queue1 && mock->calls[75] == 0);
        }

        // 133 CreatePlacedResourceX, past every PC device vtable. With no handler: refused, the output cleared, not a
        // jump through a missing entry (the table had 128 entries).
        {
            void* placed = reinterpret_cast<void*>(1);
            const GUID resource_iid = __uuidof(ID3D12Resource);
            D3D12_RESOURCE_DESC none{};
            xs::set_placed_at_va_handler(nullptr);
            EXPECT(hr_of(raw(device, 133, 0x7f0000000000ull, word(&none), 0xFFFFFFFFull, 0, word(&resource_iid),
                             word(&placed))) == E_NOTIMPL &&
                   !placed && mock->calls[127] == 0);
            EXPECT(xs::device_slot_overridden(133));
        }
        // With a handler: a description the proxy implements reaches it unchanged, byte for byte, with the real
        // device and the title's va, state word and riid in their positions. Everything else is refused by the proxy
        // itself: the output null and the handler never called. A failing handler's result comes back with the
        // output null.
        {
            static int handler_calls = 0;
            static void* seen_real = nullptr;
            static void* seen_va = nullptr;
            static D3D12_RESOURCE_DESC seen_desc{};
            static GUID seen_iid{};
            static std::uint32_t seen_state = 0;
            static HRESULT handler_result = S_OK;
            const xs::PlacedAtVaHandler handler = [](ID3D12Device* real_device, void* va,
                                                     const D3D12_RESOURCE_DESC& desc, std::uint32_t state,
                                                     REFIID iid, void** out) -> HRESULT {
                ++handler_calls;
                seen_real = real_device;
                seen_va = va;
                seen_desc = desc;
                seen_iid = iid;
                seen_state = state;
                if (FAILED(handler_result)) {
                    *out = reinterpret_cast<void*>(0xbad);  // a sloppy handler: the proxy must still return null
                    return handler_result;
                }
                *out = reinterpret_cast<void*>(0x1330);
                return S_OK;
            };
            xs::set_placed_at_va_handler(handler);
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = 0x10000;
            buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x1);  // the console's row-major word, = PC ROW_MAJOR
            const GUID resource_iid = __uuidof(ID3D12Resource);
            void* const va = reinterpret_cast<void*>(0x7f0000000000ull);
            const auto call = [&](const D3D12_RESOURCE_DESC* desc, std::uint64_t state, const void* clear,
                                  void** placed) {
                return hr_of(raw(device, 133, word(va), word(desc), state, word(clear), word(&resource_iid),
                                 word(placed)));
            };

            // Forwarded unchanged: each implemented flag set, and both state words (the proxy does not judge it).
            const std::uint32_t accepted_flags[] = {0x0, 0x4, 0x8, 0xC};
            for (const std::uint32_t flags : accepted_flags) {
                for (const std::uint64_t state : {std::uint64_t{0}, std::uint64_t{0xFFFFFFFFull}}) {
                    D3D12_RESOURCE_DESC desc = buffer;
                    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(flags);
                    void* placed = nullptr;
                    const int before = handler_calls;
                    EXPECT(call(&desc, state, nullptr, &placed) == S_OK && placed == reinterpret_cast<void*>(0x1330));
                    EXPECT(handler_calls == before + 1);
                    EXPECT(std::memcmp(&seen_desc, &desc, sizeof(desc)) == 0);
                    EXPECT(seen_real == xs::device_proxy_real_device(device) && seen_va == va &&
                           seen_state == static_cast<std::uint32_t>(state));
                    EXPECT(std::memcmp(&seen_iid, &resource_iid, sizeof(GUID)) == 0);
                }
            }

            // Refused by the proxy: unknown flag bits (the console's 0x20000 alone and with UAV, PC bits this path
            // does not implement), a non-row-major layout word, a texture, a clear value, null pointers.
            const auto refused = [&](const D3D12_RESOURCE_DESC* desc, const void* clear, HRESULT want) {
                void* placed = reinterpret_cast<void*>(1);
                const int before = handler_calls;
                const HRESULT hr = call(desc, 0, clear, &placed);
                return hr == want && !placed && handler_calls == before;
            };
            for (const std::uint32_t flags : {0x20000u, 0x20004u, 0x20204u, 0x200u, 0x1u, 0x2u, 0x10u, 0x20u, 0x100u,
                                              0x400u, 0x80000000u}) {
                D3D12_RESOURCE_DESC desc = buffer;
                desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(flags);
                EXPECT(refused(&desc, nullptr, E_NOTIMPL));
            }
            // The default process (no experiment): the exact experimental tuple, retail state word included, is refused
            // as strictly as any other console bit. The opt-in case is --host-import-experiment; CTest unsets the
            // variable for this process.
            EXPECT(!rt::experimental_host_import::enabled());
            {
                D3D12_RESOURCE_DESC desc = buffer;
                desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x20004);
                void* placed = reinterpret_cast<void*>(1);
                const int before = handler_calls;
                EXPECT(call(&desc, 0xFFFFFFFFull, nullptr, &placed) == E_NOTIMPL && !placed && handler_calls == before);
            }
            for (const std::uint32_t layout : {0x0u, 0x2u, 0x3u, 0x101u, 0xFFFFFFFFu}) {
                D3D12_RESOURCE_DESC desc = buffer;
                desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(layout);
                EXPECT(refused(&desc, nullptr, E_NOTIMPL));
            }
            D3D12_RESOURCE_DESC texture = buffer;
            texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            texture.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);  // the Xbox tiled word
            EXPECT(refused(&texture, nullptr, E_NOTIMPL));
            const D3D12_CLEAR_VALUE clear_value{};
            EXPECT(refused(&buffer, &clear_value, E_INVALIDARG));
            EXPECT(refused(nullptr, nullptr, E_POINTER));
            {
                const int before = handler_calls;
                void* placed = reinterpret_cast<void*>(1);
                EXPECT(hr_of(raw(device, 133, word(va), word(&buffer), 0, 0, 0, word(&placed))) == E_POINTER &&
                       !placed && handler_calls == before);  // null riid
                EXPECT(hr_of(raw(device, 133, word(va), word(&buffer), 0, 0, word(&resource_iid), 0)) == E_POINTER &&
                       handler_calls == before);  // null output
            }

            // The handler's failure comes back as it is, with the output null even if the handler wrote it.
            handler_result = E_NOTIMPL;
            {
                void* placed = reinterpret_cast<void*>(1);
                EXPECT(call(&buffer, 0xFFFFFFFFull, nullptr, &placed) == E_NOTIMPL && !placed);
            }
            handler_result = S_OK;
            xs::set_placed_at_va_handler(nullptr);
        }

        Mock* const queue_mock = reinterpret_cast<Mock*>(real_queue);
        Mock* const fence_mock = reinterpret_cast<Mock*>(real_fence);
        EXPECT(release(queue) == 0 && release(fence) == 0);
        EXPECT(queue_mock->refs == 0 && fence_mock->refs == 0);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: devices up to ID3D12Device, Device1, Device3, Device9: 44/47/51/76 slots, 45/46/50/75 refused "
                "without the interface, residency arrays and fences unwrapped otherwise\n", g_label);
    return 0;
}

// ---- 2. Fences with and without ID3D12Fence1 ----

int check_fence1() {
    for (const bool fence1 : {false, true}) {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, fence1);
        EXPECT(device);
        const unsigned long device_refs = xs::device_proxy_ref_count(device);
        void* fence = make_fence(device);
        EXPECT(xs::is_fence_proxy(fence) && xs::fence_proxy_slot_count(fence) == (fence1 ? 12u : 11u));
        Mock* const fence_mock = mock->last_child;
        void* as_fence1 = nullptr;
        const HRESULT qi = hr_of(raw(fence, 0, word(&kFence1Iid), word(&as_fence1)));
        if (fence1) {
            EXPECT(qi == S_OK && as_fence1 == fence && release(as_fence1) == 1);
        } else {
            EXPECT(qi == E_NOINTERFACE && !as_fence1);
        }
        EXPECT(raw(fence, 8) == kCompletedValue);  // GetCompletedValue forwarded
        const HRESULT slot11 = hr_of(raw(fence, 11));
        if (fence1) {
            EXPECT(slot11 == S_OK && fence_mock->calls[11] == 1);
        } else {
            EXPECT(slot11 == E_NOTIMPL && fence_mock->calls[11] == 0);  // not forwarded past an 11-slot vtable
        }
        EXPECT(release(fence) == 0 && fence_mock->refs == 0);

        // Asked for as ID3D12Fence1: made as ID3D12Fence, then refused when the fence lacks Fence1, and released.
        void* created1 = nullptr;
        const HRESULT made1 = hr_of(raw(device, 36, 0, 0, word(&kFence1Iid), word(&created1)));
        if (fence1) {
            EXPECT(made1 == S_OK && xs::is_fence_proxy(created1) && release(created1) == 0);
        } else {
            EXPECT(made1 == E_NOINTERFACE && !created1);
        }
        EXPECT(mock->last_child->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: fences without and with ID3D12Fence1: 11/12 slots, QueryInterface, slot 11 and creation as "
                "Fence1 refused or forwarded to match\n", g_label);
    return 0;
}

// ---- 3. Creation failures and refused interfaces ----

int check_create_failures() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    void* out = reinterpret_cast<void*>(1);

    mock->create_null = true;  // S_OK with no object
    mock->create_result = S_OK;
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))) == E_FAIL && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 36, 0, 0, word(&kFenceIid), word(&out))) == E_FAIL && !out);
    mock->create_null = false;
    mock->create_result = E_OUTOFMEMORY;  // the real failure, passed on
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))) == E_OUTOFMEMORY && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 75, word(&kQueueDesc), 0, word(&kQueueIid), word(&out))) == E_OUTOFMEMORY && !out);
    mock->create_result = S_OK;

    const unsigned calls8 = mock->calls[8];
    out = reinterpret_cast<void*>(1);  // a null IID: E_POINTER, the output cleared, nothing created
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), 0, word(&out))) == E_POINTER && !out && mock->calls[8] == calls8);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 36, 0, 0, 0, word(&out))) == E_POINTER && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 75, word(&kQueueDesc), 0, 0, word(&out))) == E_POINTER && !out);
    // No output: the device's own answer (S_FALSE here), as the PC runtime gives it.
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), 0)) == S_FALSE && mock->calls[8] == calls8 + 1);
    EXPECT(hr_of(raw(device, 36, 0, 0, word(&kFenceIid), 0)) == S_FALSE);

    // An interface the real queue implements but the proxy does not model: created, refused, released.
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kExtraIid), word(&out))) == E_NOINTERFACE && !out);
    EXPECT(mock->last_child->refs == 0);
    void* queue = make_queue(device);
    EXPECT(queue);
    Mock* const queue_mock = mock->last_child;
    void* extra = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(queue, 0, word(&kExtraIid), word(&extra))) == E_NOINTERFACE && !extra);
    EXPECT(xs::command_object_proxy_ref_count(queue) == 1 && queue_mock->refs == 1);
    EXPECT(release(queue) == 0 && queue_mock->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: creation with no object (E_FAIL), a failing device, null IID and output, and an interface the "
                "proxy does not model leave no reference\n", g_label);
    return 0;
}

// ---- 4. OpenSharedHandle ----

int check_open_shared() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, false);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);

    for (const IID* iid : {&kFenceIid, &kUnknownIid}) {  // a fence, whatever it is asked for as, comes back wrapped
        void* opened = nullptr;
        EXPECT(hr_of(raw(device, 32, kFenceHandle, word(iid), word(&opened))) == S_OK);
        Mock* const fence_mock = mock->last_child;
        EXPECT(xs::is_fence_proxy(opened) && word(xs::fence_proxy_real_fence(opened)) == word(fence_mock));
        EXPECT(xs::command_object_proxy_ref_count(opened) == 1 && fence_mock->refs == 1);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs + 1);
        EXPECT(release(opened) == 0 && fence_mock->refs == 0);
    }
    void* out = reinterpret_cast<void*>(1);  // as Fence1, which this fence lacks: the real open fails, passed on
    EXPECT(hr_of(raw(device, 32, kFenceHandle, word(&kFence1Iid), word(&out))) == E_NOINTERFACE && !out);
    EXPECT(mock->last_child->refs == 0);

    for (const IID* iid : {&kResourceIid, &kUnknownIid}) {  // not a fence: the real object, unchanged
        void* opened = nullptr;
        EXPECT(hr_of(raw(device, 32, kResourceHandle, word(iid), word(&opened))) == S_OK);
        Mock* const resource_mock = mock->last_child;
        EXPECT(opened == resource_mock && !xs::is_fence_proxy(opened) && !xs::is_queue_proxy(opened));
        EXPECT(resource_mock->refs == 1 && release(opened) == 0);
    }
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);

    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 32, 0xbad, word(&kFenceIid), word(&out))) == E_INVALIDARG && !out);
    EXPECT(hr_of(raw(device, 32, kFenceHandle, word(&kFenceIid), 0)) == S_FALSE);  // the device's own answer
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 32, kFenceHandle, 0, word(&out))) == E_POINTER && !out);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: OpenSharedHandle wraps a fence (as ID3D12Fence and IUnknown), passes a resource, a failed open, "
                "an invalid handle and a null output through, with balanced references\n", g_label);
    return 0;
}

// ---- 5. Allocation failures in the proxy ----

int check_alloc_failures() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    void* queue = make_queue(device);
    void* fence = make_fence(device);
    EXPECT(queue && fence);
    const std::uint64_t objects[2] = {word(queue), word(fence)};

    const auto failing = [&](auto call) {  // one allocation refused, the result of the call
        const int before = g_failed_news;
        g_fail_news = 1;
        const HRESULT hr = call();
        g_fail_news = 0;
        return g_failed_news == before + 1 ? hr : S_OK;
    };
    void* out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 36, 0, 0, word(&kFenceIid), word(&out))); }) == E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);  // the real fence was made and released
    out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))); }) ==
           E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);
    out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 32, kFenceHandle, word(&kFenceIid), word(&out))); }) ==
           E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);
    // Up to 16 entries the unwrapped copy is inline: an armed failure is not even reached, and the call goes on.
    g_fail_news = 1;
    const HRESULT inline_result = hr_of(raw(device, 34, 2, word(objects)));
    const bool untouched = g_fail_news == 1;
    g_fail_news = 0;
    EXPECT(inline_result == S_OK && untouched && mock->array[34][0] == word(xs::queue_proxy_real_queue(queue)));

    // A longer array is allocated; refused, the call returns E_OUTOFMEMORY and reaches no device.
    constexpr unsigned kLong = 32;
    std::uint64_t many[kLong], many_fences[kLong], values[kLong];
    D3D12_RESIDENCY_PRIORITY priorities[kLong] = {};
    for (unsigned i = 0; i < kLong; ++i) {
        many[i] = (i % 2 != 0) ? word(fence) : word(queue);
        many_fences[i] = word(fence);
        values[i] = 1;
    }
    const unsigned calls34 = mock->calls[34], calls45 = mock->calls[45], calls46 = mock->calls[46];
    const unsigned calls50 = mock->calls[50];
    EXPECT(failing([&] { return hr_of(raw(device, 34, kLong, word(many))); }) == E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 45, word(many_fences), word(values), kLong, 0, 0)); }) ==
           E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 46, kLong, word(many), word(priorities))); }) == E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 50, 0, kLong, word(many), word(fence), 1)); }) == E_OUTOFMEMORY);
    EXPECT(mock->calls[34] == calls34 && mock->calls[45] == calls45 && mock->calls[46] == calls46 &&
           mock->calls[50] == calls50);  // nothing reached the device
    // And without the failure the long array arrives unwrapped.
    EXPECT(hr_of(raw(device, 34, kLong, word(many))) == S_OK && mock->calls[34] == calls34 + 1);
    EXPECT(mock->array[34][0] == word(xs::queue_proxy_real_queue(queue)) &&
           mock->array[34][1] == word(xs::fence_proxy_real_fence(fence)));

    // The device proxy itself: on failure the caller keeps its reference.
    Mock* other = new_mock(Kind::device);
    other->refs = 1;
    void* second = reinterpret_cast<void*>(1);
    EXPECT(failing([&] {
               return xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(other), __uuidof(ID3D12Device),
                                              &second);
           }) == E_OUTOFMEMORY);
    EXPECT(!second && other->refs == 1);

    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    Mock* const fence_mock = reinterpret_cast<Mock*>(xs::fence_proxy_real_fence(fence));
    EXPECT(release(queue) == 0 && release(fence) == 0 && queue_mock->refs == 0 && fence_mock->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: a refused allocation in queue, fence, shared-fence and device proxy creation and in the four "
                "unwrapped arrays returns E_OUTOFMEMORY, reaches no device and leaks nothing\n", g_label);
    return 0;
}

// ---- 6. Command allocators (device slot 9) ----

int check_allocators() {
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    // An allocator that answers Palmwave's tag (its lists resolve a proxy) comes back wrapped.
    {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, true, true);
        EXPECT(device);
        const unsigned long device_refs = xs::device_proxy_ref_count(device);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, word(&allocator_iid), word(&allocator))) == S_OK);
        Mock* const real = mock->last_child;
        EXPECT(xs::is_allocator_proxy(allocator) && word(xs::allocator_proxy_real_allocator(allocator)) == word(real));
        EXPECT(mock->args[9][0] == D3D12_COMMAND_LIST_TYPE_DIRECT && real->refs == 1);
        EXPECT(xs::allocator_proxy_slot_count(allocator) == 9 && xs::device_proxy_ref_count(device) == device_refs + 1);
        // Reset (slot 8) reaches the real allocator; every slot past it up to the table end is refused.
        EXPECT(hr_of(raw(allocator, 8)) == S_OK && real->calls[8] == 1);
        for (unsigned slot = 9; slot < xs::allocator_proxy_table_size(); ++slot)
            EXPECT(hr_of(raw(allocator, slot)) == E_NOTIMPL && real->calls[slot] == 0);
        // The tag and the real object stay hidden: QueryInterface answers only the modelled interfaces.
        void* found = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(allocator, 0, word(&xs::kPalmwaveCommandAllocatorTag), word(&found))) == E_NOINTERFACE &&
               !found);
        EXPECT(hr_of(raw(allocator, 0, word(&allocator_iid), word(&found))) == S_OK && found == allocator);
        EXPECT(release(found) == 1);
        // An allocator is a pageable: MakeResident gives the real runtime the real allocator.
        const std::uint64_t objects[1] = {word(allocator)};
        EXPECT(hr_of(raw(device, 34, 1, word(objects))) == S_OK && mock->array[34][0] == word(real));
        EXPECT(release(allocator) == 0 && real->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        // Null IID, null output, a failing or empty creation, an IID the allocator lacks.
        void* out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, 0, word(&out))) == E_POINTER && !out);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), 0)) == S_FALSE);  // the device's own answer
        mock->create_result = E_OUTOFMEMORY;
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out))) == E_OUTOFMEMORY && !out);
        mock->create_result = S_OK;
        mock->create_null = true;
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out))) == E_FAIL && !out);
        mock->create_null = false;
        const IID fence_iid = __uuidof(ID3D12Fence);
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&fence_iid), word(&out))) == E_NOINTERFACE && !out);
        EXPECT(mock->last_child->refs == 0);
        // A refused allocation for the proxy: E_OUTOFMEMORY, and the real allocator is released.
        g_fail_news = 1;
        out = reinterpret_cast<void*>(1);
        const HRESULT starved = hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out)));
        g_fail_news = 0;
        EXPECT(starved == E_OUTOFMEMORY && !out && mock->last_child->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    // An allocator without the tag (a PC runtime allocator, whose lists could not resolve a proxy) comes back as it
    // is, as the interface asked for.
    {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, true, false);
        EXPECT(device);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
        EXPECT(allocator == mock->last_child && !xs::is_allocator_proxy(allocator) && mock->last_child->refs == 1);
        Mock* const real = mock->last_child;
        EXPECT(release(allocator) == 0 && real->refs == 0);
        const IID fence_iid = __uuidof(ID3D12Fence);
        void* out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&fence_iid), word(&out))) == E_NOINTERFACE && !out);
        EXPECT(mock->last_child->refs == 0);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: allocators answering Palmwave's tag are wrapped (Reset forwarded, tail refused, residency "
                "unwrapped, references balanced); others come back unwrapped; failures leave no reference\n", g_label);
    return 0;
}

// ---- 7. Xbox device slots from the public HLE (48, 73, 78, 79, 80) ----
//
// A device that answers ID3D12Device11 (80 PC slots) is the Vulkan layer's shape: slots 78/79 are inside its table
// (CreateReservedResource2/CreateSampler2 there) and 80 is just past it. None of the five may reach the mock's own
// slot: the mock's call counter proves the route, and the returned token/queue/list proves the handler ran.
int check_xbox_slots() {
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true, true, true);  // full-chain lists: the console IID needs GCL6
    EXPECT(device && xs::device_proxy_slot_count(device) == 80);
    xs::reset_device_proxy_stats();

    // 78 SetFrameIntervalX(output, interval_us, latency, flags): S_OK, not forwarded to PC slot 78.
    EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK);
    EXPECT(mock->calls[78] == 0 && xs::device_slot_hits(78) == 1);
    // 79 ScheduleFrameEventX(event, offset_us, pEvent, flags): S_OK, not forwarded to PC slot 79.
    EXPECT(hr_of(raw(device, 79, 0, 0, 0x1234, 0)) == S_OK);
    EXPECT(mock->calls[79] == 0 && xs::device_slot_hits(79) == 1);

    // 80 WaitFrameEventX(event, timeout_ms, pEvent, flags, &token): a fresh non-zero token each time; a null token
    // pointer is E_POINTER; the call never blocks (this test would hang otherwise).
    std::uint64_t token = 0, token2 = 0;
    EXPECT(hr_of(raw(device, 80, 0, 0xffffffffu, 0, 0, word(&token))) == S_OK && token != 0);
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, word(&token2))) == S_OK && token2 != 0 && token2 != token);
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, 0)) == E_POINTER);
    EXPECT(mock->calls[80] == 0 && xs::device_slot_hits(80) == 3);

    // 48 CreateCommandQueueX(desc, riid, out): the queue proxy, created through the PC slot 8 path.
    void* queue = nullptr;
    EXPECT(hr_of(raw(device, 48, word(&kQueueDesc), word(&kQueueIid), word(&queue))) == S_OK);
    EXPECT(xs::is_queue_proxy(queue) && mock->calls[48] == 0 && mock->calls[8] == 1);
    EXPECT(mock->has_queue_desc &&
           std::memcmp(&mock->last_queue_desc, &kQueueDesc, sizeof(kQueueDesc)) == 0);

    // 73 CreateCommandListX(desc, allocator, initialState, riid, out): the type is desc's first word, the allocator
    // proxy is unwrapped, the node mask is 0 and the PC six-argument CreateCommandList (slot 12) is reached; the list
    // comes back behind the console list proxy (check_lists covers the proxy itself).
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
    EXPECT(xs::is_allocator_proxy(allocator));
    Mock* const real_allocator = reinterpret_cast<Mock*>(xs::allocator_proxy_real_allocator(allocator));
    struct XboxCommandListDesc {
        std::uint32_t type;
        std::uint32_t node_mask;
        std::uint64_t reserved[2];
    } desc{};
    desc.type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.node_mask = 7;  // ignored by the console call, as the HLE does
    const IID& list_iid = kConsoleListIid;  // the measured console IID the title passes
    void* list = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), word(&list))) == S_OK);
    EXPECT(xs::is_list_proxy(list) && reinterpret_cast<Mock*>(xs::list_proxy_real_list(list))->kind == Kind::list);
    EXPECT(mock->calls[73] == 0 && mock->calls[12] == 1 && xs::device_slot_hits(73) == 1);
    EXPECT(mock->args[12][0] == 0 && mock->args[12][1] == D3D12_COMMAND_LIST_TYPE_DIRECT);
    EXPECT(mock->args[12][2] == word(real_allocator) && mock->args[12][3] == 0);
    // The real list is made as ID3D12GraphicsCommandList; the console IID is answered by the console proxy only.
    EXPECT(IsEqualIID(*reinterpret_cast<const IID*>(mock->args[12][4]), __uuidof(ID3D12GraphicsCommandList)));

    // Release the list before the refusal calls below: the COM convention (and the handler) nulls the caller's
    // output on failure, so an invalid call would clear this pointer.
    EXPECT(release(list) == 0);

    // Missing description, allocator or output: E_INVALIDARG/E_POINTER before the real device is called.
    EXPECT(hr_of(raw(device, 73, 0, word(allocator), 0, word(&list_iid), word(&list))) == E_INVALIDARG);
    EXPECT(hr_of(raw(device, 73, word(&desc), 0, 0, word(&list_iid), word(&list))) == E_INVALIDARG);
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), 0)) == E_POINTER);
    // A copy list, the derived DMA type 6 and an unknown type (the console's separate DMA-list vtable is not modelled)
    // are refused before anything is created, not coerced to a direct list.
    for (const std::uint32_t type : {static_cast<std::uint32_t>(D3D12_COMMAND_LIST_TYPE_COPY), 6u, 99u}) {
        desc.type = type;
        list = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), word(&list))) == E_NOTIMPL);
        EXPECT(!list && mock->calls[12] == 1);
    }

    EXPECT(xs::device_slot_overridden(48) && xs::device_slot_overridden(73) && xs::device_slot_overridden(78) &&
           xs::device_slot_overridden(79) && xs::device_slot_overridden(80));

    EXPECT(release(allocator) == 0 && real_allocator->refs == 0);
    EXPECT(release(queue) == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: device slots 48/73/78/79/80 route to a queue, a list and frame-event tokens without reaching "
                "the mock's own PC slots\n", g_label);
    return 0;
#else
    std::printf("%s: ID3D12Device11 is not in this SDK; Xbox slot 80's beyond-table case is not built\n", g_label);
    return 0;
#endif
}

// ---- 7b. Console command lists (device slot 73) and the PC path (device slot 12) ----

int check_lists() {
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    const IID gcl_iid = __uuidof(ID3D12GraphicsCommandList);
    const std::uint32_t direct_desc[4] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true, true, true);
    EXPECT(device);
    xs::reset_device_proxy_stats();
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, word(&allocator_iid), word(&allocator))) == S_OK);
    Mock* const real_allocator = mock->last_child;
    EXPECT(xs::is_allocator_proxy(allocator));

    // Creation with the measured console IID: the allocator proxy unwrapped, the real list made as
    // ID3D12GraphicsCommandList, the console IID answered by the proxy (it maps to ID3D12GraphicsCommandList6).
    void* list = nullptr;
    EXPECT(hr_of(raw(device, 73, word(direct_desc), word(allocator), 0, word(&kConsoleListIid), word(&list))) == S_OK);
    Mock* const real = mock->last_child;
    EXPECT(real->kind == Kind::list && mock->args[12][2] == word(real_allocator));
    EXPECT(xs::is_list_proxy(list) && word(xs::list_proxy_real_list(list)) == word(real));
    EXPECT(xs::list_proxy_slot_count(list) == 82 && xs::list_proxy_table_size() == 160);
    EXPECT(xs::list_proxy_ref_count(list) == 1 && real->refs == 1);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs + 2);  // the allocator and the list

    // The object body: the real list at +0x08, the inline-record cursor (+0x18) equal to both limits (+0x28, +0x30)
    // and pointing inside the proxy's own body, the flag bytes, header table and pipeline record zero.
    const auto* object = reinterpret_cast<const std::uint8_t*>(list);
    std::uint64_t fields[4];
    std::memcpy(&fields[0], object + 0x08, 8);
    std::memcpy(&fields[1], object + 0x18, 8);
    std::memcpy(&fields[2], object + 0x28, 8);
    std::memcpy(&fields[3], object + 0x30, 8);
    EXPECT(fields[0] == word(real));
    EXPECT(fields[1] == fields[2] && fields[1] == fields[3]);
    EXPECT(fields[1] >= word(list) + 0x554 && fields[1] < word(list) + 0x18 + 0x600);  // past the pipeline record
    for (std::size_t offset = 0x140; offset < 0x144; ++offset) EXPECT(object[offset] == 0);
    for (std::size_t offset = 0x344; offset < 0x554; ++offset) EXPECT(object[offset] == 0);

    // QueryInterface: the console IID and the layout-identical base chain (IUnknown, ID3D12Object, ID3D12DeviceChild,
    // ID3D12CommandList) return the proxy. Every PC graphics-list IID is refused with the output cleared even though
    // the real list implements it: the console table is shifted from the PC one (PC lists come from device slot 12).
    // An unknown IID with the console list's first 32 bits is refused too.
    void* asked = nullptr;
    EXPECT(hr_of(raw(list, 0, word(&kConsoleListIid), word(&asked))) == S_OK && asked == list && release(asked) == 1);
    for (const IID& base : {__uuidof(IUnknown), __uuidof(ID3D12Object), __uuidof(ID3D12DeviceChild),
                            __uuidof(ID3D12CommandList)}) {
        asked = nullptr;
        EXPECT(hr_of(raw(list, 0, word(&base), word(&asked))) == S_OK && asked == list && release(asked) == 1);
    }
    const GUID unknown_console{0xc3827890, 0x4024, 0x474c, {0, 0, 0, 0, 0, 0, 0, 1}};
    const IID refused[] = {kQueueIid,
                           unknown_console,
                           __uuidof(ID3D12GraphicsCommandList),
                           __uuidof(ID3D12GraphicsCommandList1),
                           __uuidof(ID3D12GraphicsCommandList2),
                           __uuidof(ID3D12GraphicsCommandList3),
                           __uuidof(ID3D12GraphicsCommandList4),
                           __uuidof(ID3D12GraphicsCommandList5),
                           __uuidof(ID3D12GraphicsCommandList6),
                           __uuidof(ID3D12GraphicsCommandList7),
                           __uuidof(ID3D12GraphicsCommandList8)};
    for (const IID& iid : refused) {
        asked = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(list, 0, word(&iid), word(&asked))) == E_NOINTERFACE && !asked);
    }
    EXPECT(xs::list_proxy_ref_count(list) == 1 && real->refs == 1);  // no refusal leaked a reference
    // GetDevice answers with the Xbox-ABI device.
    const IID device_iid = __uuidof(ID3D12Device);
    void* found = nullptr;
    EXPECT(hr_of(raw(list, 7, word(&device_iid), word(&found))) == S_OK && found == device);
    release(found);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs + 2);

    // Forwarding by the console table: Dispatch keeps its slot (14), the +4 block moves (console 37 -> PC 33), the
    // heap array of SetDescriptorHeaps (console 32 -> PC 28) is unwrapped (any proxy kind unwraps the same way; the
    // allocator stands in for a heap here), Reset at console 139 and 10 reaches PC 10 with the real allocator, and the
    // enhanced Barrier (console 146) reaches GCL7 slot 80. Dispatch, SetComputeRoot32BitConstant, SetDescriptorHeaps
    // and Barrier are void: their return register carries no contract, so only forwarding and arguments are checked.
    // Reset is HRESULT-returning and keeps its S_OK check.
    raw(list, 14, 1, 2, 3);
    EXPECT(real->calls[14] == 1 && real->args[14][2] == 3);
    raw(list, 37, 4, 0x1234, 2);
    EXPECT(real->calls[33] == 1 && real->args[33][1] == 0x1234 && real->calls[37] == 0);
    const std::uint64_t heaps[2] = {word(allocator), word(real_allocator)};
    raw(list, 32, 2, word(heaps));
    EXPECT(real->calls[28] == 1 && real->calls[32] == 0);
    EXPECT(real->array[28][0] == word(real_allocator) && real->array[28][1] == word(real_allocator));
    EXPECT(hr_of(raw(list, 139, word(allocator), 0)) == S_OK && real->calls[10] == 1 &&
           real->args[10][0] == word(real_allocator));
    EXPECT(hr_of(raw(list, 10, word(allocator), 0)) == S_OK && real->calls[10] == 2 &&
           real->args[10][0] == word(real_allocator));
    const std::uint64_t barrier_group[4] = {};
    raw(list, 146, 1, word(barrier_group));
    EXPECT(real->calls[80] == 1 && real->args[80][0] == 1 && real->args[80][1] == word(barrier_group));
    EXPECT(xs::list_slot_hits(139) == 1 && xs::list_slot_hits(146) == 1);

    // Close and submission: the queue gets the real list, never the proxy.
    void* queue = make_queue(device);
    EXPECT(queue);
    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    EXPECT(hr_of(raw(list, 9)) == S_OK && real->calls[9] == 1);
    const std::uint64_t submitted[1] = {word(list)};
    raw(queue, 10, 1, word(submitted));
    EXPECT(queue_mock->calls[10] == 1 && queue_mock->array[10][0] == word(real));

    // Unsupported slots: a console-only extra (27), the disputed 59 and the unmeasured tail (100) return E_NOTIMPL
    // and reach nothing. The list keeps the first one; its Close still closes the real list but fails, and the queue
    // refuses the whole submission.
    const auto total_calls = [](const Mock* m) {
        unsigned total = 0;
        for (const unsigned calls : m->calls) total += calls;
        return total;
    };
    EXPECT(hr_of(raw(list, 139, word(allocator), 0)) == S_OK);
    const unsigned real_calls_before = total_calls(real);
    EXPECT(hr_of(raw(list, 27, 1, 2, 3, 4)) == E_NOTIMPL);
    EXPECT(hr_of(raw(list, 59, 1, 2, 3)) == E_NOTIMPL && hr_of(raw(list, 100)) == E_NOTIMPL);
    EXPECT(total_calls(real) == real_calls_before);
    EXPECT(xs::list_proxy_unsupported_slot(list) == 27);
    EXPECT(hr_of(raw(list, 9)) == E_NOTIMPL && real->calls[9] == 2);
    raw(queue, 10, 1, word(submitted));
    EXPECT(queue_mock->calls[10] == 1);
    // Reset starts a clean recording: Close succeeds and the list is submitted again.
    EXPECT(hr_of(raw(list, 139, word(allocator), 0)) == S_OK && xs::list_proxy_unsupported_slot(list) == 0);
    EXPECT(hr_of(raw(list, 9)) == S_OK);
    raw(queue, 10, 1, word(submitted));
    EXPECT(queue_mock->calls[10] == 2 && queue_mock->array[10][0] == word(real));

    // ExecuteBundle (console 31 -> PC 27): a clean bundle proxy unwraps; one with an unexecuted command makes the
    // calling list incomplete and is not forwarded. ExecuteBundle is void, so its return register is not checked:
    // forwarding, the unwrapped argument and the poison state are.
    void* bundle = nullptr;
    EXPECT(hr_of(raw(device, 73, word(direct_desc), word(allocator), 0, word(&kConsoleListIid), word(&bundle))) ==
           S_OK);
    Mock* const real_bundle = mock->last_child;
    EXPECT(xs::is_list_proxy(bundle) && word(xs::list_proxy_real_list(bundle)) == word(real_bundle));
    raw(list, 31, word(bundle));
    EXPECT(real->calls[27] == 1 && real->args[27][0] == word(real_bundle));
    EXPECT(xs::list_proxy_unsupported_slot(list) == 0);
    EXPECT(hr_of(raw(bundle, 28)) == E_NOTIMPL && xs::list_proxy_unsupported_slot(bundle) == 28);
    raw(list, 31, word(bundle));
    EXPECT(real->calls[27] == 1 && xs::list_proxy_unsupported_slot(list) == 31);
    EXPECT(hr_of(raw(list, 139, word(allocator), 0)) == S_OK);

    // AllocateUploadMemory (console 148): pointers into a mapped UPLOAD buffer, refusals that move nothing, and the
    // root-descriptor and IA/SO view setters receive the buffer's GPU address for an arena pointer.
    EXPECT(raw(list, 148, 0, 16) == 0 && mock->calls[27] == 0);
    const std::uint64_t p1 = raw(list, 148, 4096, 64);
    Mock* const chunk = mock->last_child;
    EXPECT(p1 && chunk->kind == Kind::resource && mock->calls[27] == 1);
    EXPECT(mock->args[27][0] == D3D12_HEAP_TYPE_UPLOAD && mock->args[27][1] >= 4096);
    EXPECT(p1 == word(chunk->mapped) && (p1 & 63) == 0 && chunk->calls[8] == 1);  // Map
    const std::uint64_t p2 = raw(list, 148, 16, 16);
    EXPECT(p2 == p1 + 4096);
    EXPECT(raw(list, 148, ~0ull, 16) == 0 && raw(list, 148, 16, 48) == 0 && raw(list, 148, 16, 1u << 20) == 0);
    const std::uint64_t p3 = raw(list, 148, 16, 16);
    EXPECT(p3 == p2 + 16 && mock->calls[27] == 1);  // refusals moved nothing; still one chunk
    std::memset(reinterpret_cast<void*>(p1), 0x5a, 4096);  // the title writes the mapped memory directly
    const std::uint64_t gpu = chunk->gpu_va;
    // The root-descriptor and IA/SO view setters are void: forwarding and the translated arguments are checked, not
    // their return register.
    raw(list, 43, 3, p2);
    EXPECT(real->calls[39] == 1 && real->args[39][0] == 3 && real->args[39][1] == gpu + 4096);
    raw(list, 44, 3, 0xdead0000);
    EXPECT(real->calls[40] == 1 && real->args[40][0] == 3 && real->args[40][1] == 0xdead0000);
    D3D12_INDEX_BUFFER_VIEW ib_view{};
    ib_view.BufferLocation = p1 + 32;
    raw(list, 47, word(&ib_view));
    EXPECT(real->calls[43] == 1 && real->array[43][0] == gpu + 32);
    EXPECT(ib_view.BufferLocation == p1 + 32);  // the title's struct is not written
    D3D12_VERTEX_BUFFER_VIEW vb_views[2]{};
    vb_views[0].BufferLocation = p1;
    vb_views[1].BufferLocation = 0xdead0000;
    raw(list, 48, 0, 2, word(vb_views));
    EXPECT(real->calls[44] == 1 && real->args[44][1] == 2);
    EXPECT(real->array[44][0] == gpu && real->array[44][1] == 0xdead0000 && vb_views[0].BufferLocation == p1);
    D3D12_STREAM_OUTPUT_BUFFER_VIEW so_view{};
    so_view.BufferLocation = p1;
    so_view.BufferFilledSizeLocation = p2;
    raw(list, 49, 0, 1, word(&so_view));
    EXPECT(real->calls[45] == 1 && real->args[45][1] == 1);
    EXPECT(real->array[45][0] == gpu && real->array[45][1] == gpu + 4096);
    // A device that cannot make the buffer hands out no memory: 0, not host memory posing as a GPU address.
    mock->create_result = E_OUTOFMEMORY;
    EXPECT(raw(list, 148, 16u << 20, 16) == 0 && mock->calls[27] == 2);
    mock->create_result = S_OK;
    // A request larger than the remaining chunk gets a chunk of its own.
    const std::uint64_t big = raw(list, 148, 16u << 20, 256);
    Mock* const big_chunk = mock->last_child;
    EXPECT(big && mock->calls[27] == 3 && big == word(big_chunk->mapped) && (big & 255) == 0);
    EXPECT(xs::list_proxy_unsupported_slot(list) == 0);  // none of this marked the list

    // Ownership: the lists hold their real lists and the device; the arena's buffers go with the device.
    EXPECT(release(bundle) == 0 && real_bundle->refs == 0);
    EXPECT(release(list) == 0 && real->refs == 0);
    EXPECT(release(queue) == 0 && release(allocator) == 0 && real_allocator->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(chunk->refs == 1 && big_chunk->refs == 1);
    EXPECT(release(device) == 0 && mock->refs == 0);
    EXPECT(chunk->refs == 0 && chunk->calls[9] == 1 && big_chunk->refs == 0 && big_chunk->calls[9] == 1);

    // Creation failures: an IID the proxy does not model (including PC graphics-list revision IIDs), and a refused
    // allocation for the proxy, all release the real list and leave the device's references as they were.
    Mock* mock2 = nullptr;
    void* device2 = make_device(mock2, 9, true, true, true);
    EXPECT(device2);
    void* allocator2 = nullptr;
    EXPECT(hr_of(raw(device2, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, word(&allocator_iid), word(&allocator2))) == S_OK);
    const unsigned long device2_refs = xs::device_proxy_ref_count(device2);
    void* out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device2, 73, word(direct_desc), word(allocator2), 0, word(&kQueueIid), word(&out))) ==
           E_NOINTERFACE);
    EXPECT(!out && mock2->last_child->kind == Kind::list && mock2->last_child->refs == 0);
    // PC revision IIDs through slot 73 are refused even when the real list implements them. The shared base GUID
    // has its own bounded console-creation path, checked separately below.
    for (const IID& pc_iid : {__uuidof(ID3D12GraphicsCommandList1), __uuidof(ID3D12GraphicsCommandList6)}) {
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device2, 73, word(direct_desc), word(allocator2), 0, word(&pc_iid), word(&out))) ==
               E_NOINTERFACE);
        EXPECT(!out && mock2->last_child->kind == Kind::list && mock2->last_child->refs == 0);
    }
    EXPECT(xs::device_proxy_ref_count(device2) == device2_refs);
    g_fail_news = 1;
    out = reinterpret_cast<void*>(1);
    const HRESULT starved =
        hr_of(raw(device2, 73, word(direct_desc), word(allocator2), 0, word(&kConsoleListIid), word(&out)));
    g_fail_news = 0;
    EXPECT(starved == E_OUTOFMEMORY && !out && mock2->last_child->kind == Kind::list &&
           mock2->last_child->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device2) == device2_refs);
    EXPECT(release(allocator2) == 0);

    // The PC path: device slot 12 is forwarded unchanged, so a caller compiled against the PC headers gets the real
    // PC-shaped list, which the queue passes through as it is.
    void* pc_list = nullptr;
    EXPECT(hr_of(raw(device2, 12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, word(&gcl_iid), word(&pc_list))) == S_OK);
    EXPECT(pc_list == mock2->last_child && !xs::is_list_proxy(pc_list) &&
           mock2->last_child->kind == Kind::list);
    void* queue2 = make_queue(device2);
    EXPECT(queue2);
    Mock* const queue2_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue2));
    const std::uint64_t pc_submitted[1] = {word(pc_list)};
    raw(queue2, 10, 1, word(pc_submitted));
    EXPECT(queue2_mock->calls[10] == 1 && queue2_mock->array[10][0] == word(pc_list));
    raw(pc_list, 28, 0, 0);  // PC SetDescriptorHeaps stays at its own slot.
    EXPECT(reinterpret_cast<Mock*>(pc_list)->calls[28] == 1 && reinterpret_cast<Mock*>(pc_list)->calls[32] == 0);
    EXPECT(release(pc_list) == 0 && release(queue2) == 0);
    EXPECT(release(device2) == 0 && mock2->refs == 0);
    std::printf("%s: slot 73 lists are console-shaped (body, cursor pinned, console/base IIDs only, PC graphics-list "
                "QIs refused on console-IID creation, GetDevice), forward by the console "
                "table with allocator/bundle/heap/queue unwrapping, fail Close and refuse submission after an "
                "unsupported slot until Reset, carve upload memory from a mapped buffer whose GPU address the setters "
                "receive; slot 12 stays PC-shaped\n", g_label);
    return 0;
}

// The base graphics-list GUID is shared by Xbox and PC. Factory slot 73 fixes the ABI for this object; QI must
// subsequently preserve that identity. A base-only backend proves this route does not require GCL6 capability.
int check_shared_base_list() {
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    const IID shared_iid = __uuidof(ID3D12GraphicsCommandList);
    const std::uint32_t desc[4] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true, true, false);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, word(&allocator_iid), word(&allocator))) == S_OK);
    Mock* const real_allocator = mock->last_child;
    void* list = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 73, word(desc), word(allocator), 0, word(&shared_iid), word(&list))) == S_OK);
    Mock* const real = mock->last_child;
    EXPECT(xs::is_list_proxy(list) && real->kind == Kind::command_list && real->refs == 1);
    EXPECT(xs::list_proxy_real_list(list) == reinterpret_cast<ID3D12GraphicsCommandList*>(real));
    EXPECT(xs::list_proxy_slot_count(list) == 60 && xs::list_proxy_ref_count(list) == 1);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs + 2);

    for (const IID& iid : {shared_iid, __uuidof(IUnknown), __uuidof(ID3D12Object),
                           __uuidof(ID3D12DeviceChild), __uuidof(ID3D12CommandList)}) {
        void* asked = nullptr;
        EXPECT(hr_of(raw(list, 0, word(&iid), word(&asked))) == S_OK && asked == list);
        EXPECT(xs::list_proxy_ref_count(list) == 2 && real->refs == 1);
        EXPECT(release(asked) == 1);
    }
    GUID unknown_shared = shared_iid;
    unknown_shared.Data4[7] ^= 1;  // comparing Data1 alone must not grant an unknown interface
    const IID refused[] = {unknown_shared, kQueueIid, kConsoleListIid,
                            __uuidof(ID3D12GraphicsCommandList1), __uuidof(ID3D12GraphicsCommandList2),
                            __uuidof(ID3D12GraphicsCommandList3), __uuidof(ID3D12GraphicsCommandList4),
                            __uuidof(ID3D12GraphicsCommandList5), __uuidof(ID3D12GraphicsCommandList6),
                            __uuidof(ID3D12GraphicsCommandList7), __uuidof(ID3D12GraphicsCommandList8)};
    for (const IID& iid : refused) {
        void* asked = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(list, 0, word(&iid), word(&asked))) == E_NOINTERFACE && !asked);
    }
    EXPECT(xs::list_proxy_ref_count(list) == 1 && real->refs == 1);
    void* unknown = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 73, word(desc), word(allocator), 0, word(&unknown_shared), word(&unknown))) == E_NOINTERFACE);
    EXPECT(!unknown && mock->last_child->refs == 0 && xs::device_proxy_ref_count(device) == device_refs + 2);

    raw(list, 32, 0, 0);  // console SetDescriptorHeaps is PC slot 28 even for the shared creation GUID
    EXPECT(real->calls[28] == 1 && real->calls[32] == 0 && real->args[28][0] == 0);
    EXPECT(hr_of(raw(list, 9)) == S_OK && real->calls[9] == 1);
    EXPECT(xs::list_proxy_unsupported_slot(list) == 0);
    EXPECT(release(list) == 0 && real->refs == 0);
    EXPECT(release(allocator) == 0 && real_allocator->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: shared base GUID at Xbox factory 73 preserves console slots, QI identity and references with a "
                "base-only backend; revision and unknown IIDs refused\n", g_label);
    return 0;
}

// ---- 7c. EXPERIMENTAL: console list slots 131/132 behind PALMWAVE_EXPERIMENTAL_XBOX_FLUSH (a hypothesis) ----
//
// The default stays strict (E_NOTIMPL, poisoned list, failed Close, refused submission). With the variable exactly
// "1" at list creation, only the four exact tuples record one real global barrier on the backend list's
// ID3D12GraphicsCommandList7. The experimental calls' return register is never checked (the console method's return
// type is unmeasured); the mock's calls, references and the list state are.

constexpr char kFlushEnv[] = "PALMWAVE_EXPERIMENTAL_XBOX_FLUSH";
constexpr std::uint64_t kFullRange = 0x0000ffffffffff00ull;
struct FlushCall {
    unsigned slot;
    std::uint64_t w[4];
};
constexpr FlushCall kFlushTuples[] = {
    {132, {0x700000, 0, 0, kFullRange}},
    {131, {0xc3c0, 0x80020000, 0, kFullRange}},
    {132, {0x100000, 0, 0, kFullRange}},
    {131, {0xc380, 0x20000, 0, kFullRange}},
};

std::uint64_t flush_call(void* list, const FlushCall& c) {
    return raw(list, c.slot, c.w[0], c.w[1], c.w[2], c.w[3]);
}

// ACCESS_COMMON and BARRIER_TYPE_GLOBAL are both 0, so the recorded fields start as values no barrier carries.
void arm_barrier_record(Mock* m) {
    m->barrier_groups = 0xbad;
    std::memset(&m->barrier_group, 0x5c, sizeof(m->barrier_group));
    std::memset(&m->barrier_global, 0x5c, sizeof(m->barrier_global));
}

bool recorded_full_global_barrier(const Mock* m) {
    return m->barrier_groups == 1 && m->barrier_group.Type == D3D12_BARRIER_TYPE_GLOBAL &&
           m->barrier_group.NumBarriers == 1 && m->barrier_global.SyncBefore == D3D12_BARRIER_SYNC_ALL &&
           m->barrier_global.SyncAfter == D3D12_BARRIER_SYNC_ALL &&
           m->barrier_global.AccessBefore == D3D12_BARRIER_ACCESS_COMMON &&
           m->barrier_global.AccessAfter == D3D12_BARRIER_ACCESS_COMMON;
}

// A console list made through device slot 73 with the variable set to `value` (null: unset) during creation only.
void* make_flush_list(void* device, void* allocator, const char* value) {
    const std::uint32_t direct_desc[4] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    SetEnvironmentVariableA(kFlushEnv, value);
    void* list = nullptr;
    const HRESULT hr =
        hr_of(raw(device, 73, word(direct_desc), word(allocator), 0, word(&kConsoleListIid), word(&list)));
    SetEnvironmentVariableA(kFlushEnv, nullptr);
    return SUCCEEDED(hr) && xs::is_list_proxy(list) ? list : nullptr;
}

// From a clean recording, `c` takes the existing unsupported path: E_NOTIMPL, no backend Barrier and no GCL7 query,
// the list marked with c.slot, Close failing although the real list closed, and the queue refusing the submission.
// Ends with a Reset, so the next case starts clean.
int expect_strict(void* list, Mock* real, void* allocator, void* queue, Mock* queue_mock, const FlushCall& c) {
    EXPECT(hr_of(raw(list, 139, word(allocator), 0)) == S_OK && xs::list_proxy_unsupported_slot(list) == 0);
    const unsigned barriers = real->calls[80], queries = real->gcl7_queries, closes = real->calls[9];
    const unsigned submissions = queue_mock->calls[10];
    const long refs = real->refs;
    EXPECT(hr_of(flush_call(list, c)) == E_NOTIMPL);
    EXPECT(real->calls[80] == barriers && real->gcl7_queries == queries && real->refs == refs);
    EXPECT(xs::list_proxy_unsupported_slot(list) == c.slot);
    EXPECT(hr_of(raw(list, 9)) == E_NOTIMPL && real->calls[9] == closes + 1);
    const std::uint64_t submitted[1] = {word(list)};
    raw(queue, 10, 1, word(submitted));
    EXPECT(queue_mock->calls[10] == submissions);
    EXPECT(hr_of(raw(list, 139, word(allocator), 0)) == S_OK && xs::list_proxy_unsupported_slot(list) == 0);
    return 0;
}

int check_experimental_flush() {
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    SetEnvironmentVariableA(kFlushEnv, nullptr);
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true, true, true);  // full-chain lists: GCL6 for the IID, GCL7 for Barrier
    EXPECT(device);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, word(&allocator_iid), word(&allocator))) == S_OK);
    void* queue = make_queue(device);
    EXPECT(queue);
    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));

    // 1. Flag off. Unset at creation: every exact tuple is strict, and setting the variable afterwards changes nothing
    // (it is read when the list is created).
    void* off = make_flush_list(device, allocator, nullptr);
    EXPECT(off);
    Mock* const off_real = mock->last_child;
    for (const FlushCall& c : kFlushTuples)
        if (expect_strict(off, off_real, allocator, queue, queue_mock, c)) return 1;
    SetEnvironmentVariableA(kFlushEnv, "1");
    const int late = expect_strict(off, off_real, allocator, queue, queue_mock, kFlushTuples[1]);
    SetEnvironmentVariableA(kFlushEnv, nullptr);
    if (late) return 1;
    EXPECT(off_real->gcl7_queries == 0 && off_real->calls[80] == 0);
    EXPECT(release(off) == 0 && off_real->refs == 0);
    // Any value other than exactly "1" is off.
    for (const char* value : {"", "0", "true", "yes", "1 ", " 1", "01", "11", "2"}) {
        void* list = make_flush_list(device, allocator, value);
        EXPECT(list);
        Mock* const real = mock->last_child;
        for (const FlushCall& c : kFlushTuples)
            if (expect_strict(list, real, allocator, queue, queue_mock, c)) return 1;
        EXPECT(real->gcl7_queries == 0 && release(list) == 0 && real->refs == 0);
    }

    // 2. Flag on, exact matches: each tuple queries GCL7 once, records one full global barrier on it, releases it,
    // and leaves the list clean, so Close succeeds and the queue submits the real list.
    void* list = make_flush_list(device, allocator, "1");
    EXPECT(list);
    Mock* const real = mock->last_child;
    const long real_refs = real->refs;
    EXPECT(real_refs == 1);
    EXPECT(hr_of(raw(list, 139, word(allocator), 0)) == S_OK);
    for (const FlushCall& c : kFlushTuples) {
        const unsigned barriers = real->calls[80], queries = real->gcl7_queries;
        arm_barrier_record(real);
        flush_call(list, c);  // return register not checked
        EXPECT(real->gcl7_queries == queries + 1 && real->calls[80] == barriers + 1);
        EXPECT(recorded_full_global_barrier(real));
        EXPECT(real->refs == real_refs && xs::list_proxy_unsupported_slot(list) == 0);
    }
    {
        const unsigned closes = real->calls[9], submissions = queue_mock->calls[10];
        EXPECT(hr_of(raw(list, 9)) == S_OK && real->calls[9] == closes + 1);
        const std::uint64_t submitted[1] = {word(list)};
        raw(queue, 10, 1, word(submitted));
        EXPECT(queue_mock->calls[10] == submissions + 1 && queue_mock->array[10][0] == word(real));
    }

    // 3. Every single-word change of every tuple (a low bit and a high-dword bit), each tuple on the other slot, the
    // statically found 131 form that has not been seen at run time, and neighbouring slots: all strict.
    for (const FlushCall& c : kFlushTuples) {
        for (int i = 0; i < 4; ++i) {
            for (const std::uint64_t flip : {1ull, 1ull << 40}) {
                FlushCall changed = c;
                changed.w[i] ^= flip;
                if (expect_strict(list, real, allocator, queue, queue_mock, changed)) return 1;
            }
        }
        FlushCall other_slot = c;
        other_slot.slot = c.slot == 131 ? 132 : 131;
        if (expect_strict(list, real, allocator, queue, queue_mock, other_slot)) return 1;
        for (const unsigned neighbour : {130u, 133u}) {
            FlushCall moved = c;
            moved.slot = neighbour;
            if (expect_strict(list, real, allocator, queue, queue_mock, moved)) return 1;
        }
    }
    if (expect_strict(list, real, allocator, queue, queue_mock, {131, {0xc280, 0x20000, 0, kFullRange}})) return 1;

    // 4. No ID3D12GraphicsCommandList7 on the backend list: queried, refused, nothing recorded, strict.
    real->refuse_gcl7 = true;
    for (const FlushCall& c : kFlushTuples) {
        EXPECT(hr_of(raw(list, 139, word(allocator), 0)) == S_OK);
        const unsigned barriers = real->calls[80], queries = real->gcl7_queries, closes = real->calls[9];
        const unsigned submissions = queue_mock->calls[10];
        EXPECT(hr_of(flush_call(list, c)) == E_NOTIMPL);
        EXPECT(real->gcl7_queries == queries + 1 && real->calls[80] == barriers && real->refs == real_refs);
        EXPECT(xs::list_proxy_unsupported_slot(list) == c.slot);
        EXPECT(hr_of(raw(list, 9)) == E_NOTIMPL && real->calls[9] == closes + 1);
        const std::uint64_t submitted[1] = {word(list)};
        raw(queue, 10, 1, word(submitted));
        EXPECT(queue_mock->calls[10] == submissions);
    }
    real->refuse_gcl7 = false;

    // 5. QueryInterface hands back a different object: the Barrier goes to that pointer (not the proxy's base list)
    // and its reference is released.
    Mock* const alias = new_mock(Kind::list);
    alias->refs = 1;  // held by `real`
    real->gcl7_alias = alias;
    EXPECT(hr_of(raw(list, 139, word(allocator), 0)) == S_OK);
    for (const FlushCall& c : kFlushTuples) {
        const unsigned real_barriers = real->calls[80], alias_barriers = alias->calls[80];
        arm_barrier_record(alias);
        flush_call(list, c);
        EXPECT(alias->calls[80] == alias_barriers + 1 && real->calls[80] == real_barriers);
        EXPECT(recorded_full_global_barrier(alias));
        EXPECT(alias->refs == 1 && real->refs == real_refs);
        EXPECT(xs::list_proxy_unsupported_slot(list) == 0);
    }
    real->gcl7_alias = nullptr;
    EXPECT(hr_of(raw(list, 9)) == S_OK);

    // 6. A list already poisoned stays poisoned: the barrier is still recorded, but the first unsupported slot, the
    // failed Close and the refused submission remain until Reset.
    EXPECT(hr_of(raw(list, 139, word(allocator), 0)) == S_OK);
    EXPECT(hr_of(raw(list, 27, 1, 2, 3, 4)) == E_NOTIMPL && xs::list_proxy_unsupported_slot(list) == 27);
    for (const FlushCall& c : kFlushTuples) {
        const unsigned barriers = real->calls[80];
        arm_barrier_record(real);
        flush_call(list, c);
        EXPECT(real->calls[80] == barriers + 1 && recorded_full_global_barrier(real));
        EXPECT(xs::list_proxy_unsupported_slot(list) == 27);
    }
    {
        const unsigned submissions = queue_mock->calls[10];
        EXPECT(hr_of(raw(list, 9)) == E_NOTIMPL);
        const std::uint64_t submitted[1] = {word(list)};
        raw(queue, 10, 1, word(submitted));
        EXPECT(queue_mock->calls[10] == submissions);
    }

    // 7. The backend's own recording failure (a void Barrier it could not record marks its list) still surfaces at
    // Close: the proxy returns the backend's HRESULT, not success.
    EXPECT(hr_of(raw(list, 139, word(allocator), 0)) == S_OK);
    flush_call(list, kFlushTuples[1]);
    EXPECT(xs::list_proxy_unsupported_slot(list) == 0);
    real->close_result = E_INVALIDARG;
    {
        const unsigned closes = real->calls[9];
        EXPECT(hr_of(raw(list, 9)) == E_INVALIDARG && real->calls[9] == closes + 1);
    }
    real->close_result = S_OK;
    EXPECT(hr_of(raw(list, 139, word(allocator), 0)) == S_OK);

    EXPECT(release(list) == 0 && real->refs == 0 && alias->refs == 1);
    EXPECT(release(queue) == 0 && release(allocator) == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    SetEnvironmentVariableA(kFlushEnv, nullptr);
    std::printf("%s: EXPERIMENTAL slots 131/132: strict unless PALMWAVE_EXPERIMENTAL_XBOX_FLUSH is exactly 1 at list "
                "creation; then only the four exact tuples record one global SYNC_ALL/ACCESS_COMMON barrier on the "
                "GCL7 pointer QueryInterface returned (released); changed words, other slots, no GCL7 stay strict; "
                "an earlier poison and a backend Close failure still surface\n", g_label);
    return 0;
}

// ---- 8. Environment overrides for the Xbox device slots (one process each, read once) ----

int run_env_slots(bool valid, bool on_slot70 = false) {
    const char* frame_interval = on_slot70 ? "70" : valid ? "96" : "2";
    const char* schedule_event = valid ? "97" : "2";
    const char* wait_event = valid ? "98" : "2";
    const char* queue_x = valid ? "99" : "2";
    const char* list_x = valid ? "100" : "2";
    EXPECT(SetEnvironmentVariableA("PALMWAVE_XS_FRAME_INTERVAL_SLOT", frame_interval) != FALSE);
    EXPECT(SetEnvironmentVariableA("PALMWAVE_XS_SCHEDULE_EVENT_SLOT", schedule_event) != FALSE);
    EXPECT(SetEnvironmentVariableA("PALMWAVE_XS_WAIT_EVENT_SLOT", wait_event) != FALSE);
    EXPECT(SetEnvironmentVariableA("PALMWAVE_XS_QUEUE_X_SLOT", queue_x) != FALSE);
    EXPECT(SetEnvironmentVariableA("PALMWAVE_XS_LIST_X_SLOT", list_x) != FALSE);

    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true, true, true);  // full-chain lists: the console IID needs GCL6
    EXPECT(device);
    xs::reset_device_proxy_stats();
    xs::reset_srv_at_va_stats();
    std::uint64_t token = 0;
    if (on_slot70) {
        // SetFrameIntervalX moved onto 70, which also has a built-in handler: the configured route wins, so (output,
        // interval, latency, flags) is a frame-interval call, never a view whose destination is the latency word; the
        // view handler is not entered at all (no outcome recorded) and neither PC slot 70 nor 18 is reached.
        EXPECT(hr_of(raw(device, 70, 0, 16667, 1, 0)) == S_OK && mock->calls[70] == 0 && mock->calls[18] == 0);
        EXPECT(xs::last_srv_at_va_outcome() == xs::SrvAtVaOutcome::none && xs::srv_at_va_refused_calls() == 0 &&
               xs::srv_at_va_created_calls() == 0 && xs::device_slot_hits(70) == 1);
        // The old default is a PC slot again and is forwarded.
        EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK && mock->calls[78] == 1);
        EXPECT(release(device) == 0 && mock->refs == 0);
        std::printf("%s: a route moved onto device slot 70 takes precedence over the built-in view\n", g_label);
        return 0;
    }
    if (valid) {
        // Every route moved: the new slot is handled, the default slot is forwarded to the mock.
        EXPECT(hr_of(raw(device, 96, 0, 16667, 1, 0)) == S_OK && mock->calls[96] == 0);
        EXPECT(hr_of(raw(device, 97, 0, 0, 0, 0)) == S_OK && mock->calls[97] == 0);
        EXPECT(hr_of(raw(device, 98, 0, 0, 0, 0, word(&token))) == S_OK && token && mock->calls[98] == 0);
        void* queue = nullptr;
        EXPECT(hr_of(raw(device, 99, word(&kQueueDesc), word(&kQueueIid), word(&queue))) == S_OK);
        EXPECT(xs::is_queue_proxy(queue) && mock->calls[99] == 0);
        const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
        struct {
            std::uint32_t type, node_mask;
        } desc{static_cast<std::uint32_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), 0};
        void* list = nullptr;
        EXPECT(hr_of(raw(device, 100, word(&desc), word(allocator), 0, word(&kConsoleListIid), word(&list))) ==
               S_OK);
        EXPECT(xs::is_list_proxy(list));
        EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK && mock->calls[78] == 1);
        EXPECT(release(list) == 0);
        EXPECT(release(allocator) == 0);
        EXPECT(release(queue) == 0);
    } else {
        // Values outside 3..127 are ignored: the five defaults still answer, and a PC slot that is not a default is
        // forwarded unchanged.
        EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK && mock->calls[78] == 0);
        EXPECT(hr_of(raw(device, 79, 0, 0, 0, 0)) == S_OK && mock->calls[79] == 0);
        EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, word(&token))) == S_OK && token && mock->calls[80] == 0);
        EXPECT(hr_of(raw(device, 71)) == S_OK && mock->calls[71] == 1);  // not an Xbox slot: forwarded
    }
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: environment slot overrides %s\n", g_label,
                valid ? "move all five routes" : "outside 3..127 are ignored");
    return 0;
}

// Xbox queue field +8 has no established public engine/affinity contract. Only the observed
// values 0..3 have a bounded mapping to ordinary host queues; PC slot 8 remains strict.
int check_xbox_queue_descriptions() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 0, true);
    EXPECT(device);
    mock->validate_queue_desc = true;
    void* queues[4]{};
    Mock* real_queues[4]{};
    for (UINT value = 0; value < 4; ++value) {
        const D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_COMPUTE,
            value == 3 ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
            static_cast<D3D12_COMMAND_QUEUE_FLAGS>(value), value == 0 ? 1u : 0u};
        const auto original = desc;
        EXPECT(hr_of(raw(device, 48, word(&desc), word(&kQueueIid), word(&queues[value]))) == S_OK);
        EXPECT(xs::is_queue_proxy(queues[value]) && mock->calls[8] == value + 1 && mock->calls[48] == 0);
        EXPECT(mock->has_queue_desc && mock->last_queue_desc.Flags == D3D12_COMMAND_QUEUE_FLAG_NONE);
        EXPECT(mock->last_queue_desc.Type == desc.Type && mock->last_queue_desc.Priority == desc.Priority &&
               mock->last_queue_desc.NodeMask == desc.NodeMask);
        EXPECT(std::memcmp(&desc, &original, sizeof(desc)) == 0);
        real_queues[value] = mock->last_child;
        EXPECT(xs::queue_proxy_real_queue(queues[value]) == reinterpret_cast<ID3D12CommandQueue*>(real_queues[value]));
        for (UINT prior = 0; prior < value; ++prior)
            EXPECT(queues[prior] != queues[value] && real_queues[prior] != real_queues[value]);
    }
    for (UINT value = 0; value < 4; ++value) {
        EXPECT(release(queues[value]) == 0 && real_queues[value]->refs == 0);
    }

    // Unobserved bits must fail before any backend call, with a cleared output and no source mutation.
    const unsigned before = mock->calls[8];
    for (UINT value : {4u, 0x80000002u}) {
        const D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_COMPUTE, 0,
            static_cast<D3D12_COMMAND_QUEUE_FLAGS>(value), 0};
        const auto original = desc;
        void* out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 48, word(&desc), word(&kQueueIid), word(&out))) == E_INVALIDARG && !out);
        EXPECT(mock->calls[8] == before && std::memcmp(&desc, &original, sizeof(desc)) == 0);
    }
    void* out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 48, 0, word(&kQueueIid), word(&out))) == E_INVALIDARG && !out);
    EXPECT(mock->calls[8] == before);

    // The same bit 2 at the PC entry point is forwarded unchanged and rejected by the strict backend.
    D3D12_COMMAND_QUEUE_DESC pc{D3D12_COMMAND_LIST_TYPE_COMPUTE, 0,
        static_cast<D3D12_COMMAND_QUEUE_FLAGS>(2), 0};
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 8, word(&pc), word(&kQueueIid), word(&out))) == E_INVALIDARG && !out);
    EXPECT(mock->calls[8] == before + 1 && mock->last_queue_desc.Flags == pc.Flags);
    pc.Flags = D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT;
    EXPECT(hr_of(raw(device, 8, word(&pc), word(&kQueueIid), word(&out))) == S_OK);
    EXPECT(mock->last_queue_desc.Flags == D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT && xs::is_queue_proxy(out));
    EXPECT(release(out) == 0);

    // Capability queries and host creation failures retain their real answers through the Xbox adapter.
    pc.Flags = static_cast<D3D12_COMMAND_QUEUE_FLAGS>(3);
    EXPECT(hr_of(raw(device, 48, word(&pc), word(&kQueueIid), 0)) == S_FALSE);
    mock->create_result = E_OUTOFMEMORY;
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 48, word(&pc), word(&kQueueIid), word(&out))) == E_OUTOFMEMORY && !out);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: bounded Xbox queue values create distinct backend queues; PC flags and failures preserved\n", g_label);
    return 0;
}

// ---- 8b. Device slot 133's opt-in experiment (one process per environment, read once) ----
//
// CTest sets PALMWAVE_EXPERIMENTAL_XBOX_HOST_IMPORT for the process: "1" for `on`; unset, "0" or another value for `off`.
// The test only reads it. `on`: the exact tuple (flags 0x20004, state word 0xFFFFFFFF, no clear value, with garbage in
// the padding DWORD at +4) reaches the handler as a new description with flags 0x4 and state 0, with the real device, the
// title's VA and riid; the title's bytes are unchanged. Both modes: every near miss is refused by the proxy (output null,
// handler not called), and the strict shapes are forwarded byte for byte as before.

struct Seen133 {
    int calls = 0;
    void* real = nullptr;
    void* va = nullptr;
    D3D12_RESOURCE_DESC desc{};
    GUID iid{};
    std::uint32_t state = 0;
    HRESULT result = S_OK;
};
Seen133 g_seen133;

HRESULT seen133_handler(ID3D12Device* real_device, void* va, const D3D12_RESOURCE_DESC& desc, std::uint32_t state,
                        REFIID iid, void** out) {
    ++g_seen133.calls;
    g_seen133.real = real_device;
    g_seen133.va = va;
    g_seen133.desc = desc;
    g_seen133.iid = iid;
    g_seen133.state = state;
    if (FAILED(g_seen133.result)) {
        *out = reinterpret_cast<void*>(0xbad);  // a sloppy handler: the proxy must still return null
        return g_seen133.result;
    }
    *out = reinterpret_cast<void*>(0x1330);
    return S_OK;
}

int run_host_import_experiment(bool expect_on) {
    g_label = expect_on ? "xs_proxy_mock host import experiment on" : "xs_proxy_mock host import experiment off";
    EXPECT(rt::experimental_host_import::enabled() == expect_on);  // the CTest environment matches the mode
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    xs::set_placed_at_va_handler(&seen133_handler);
    const GUID resource_iid = __uuidof(ID3D12Resource);
    void* const va = reinterpret_cast<void*>(0x7f0000040000ull);
    const auto call = [&](const D3D12_RESOURCE_DESC* desc, std::uint64_t state, const void* clear, void** placed) {
        return hr_of(raw(device, 133, word(va), word(desc), state, word(clear), word(&resource_iid), word(placed)));
    };

    // The exact tuple, with garbage in the padding DWORD at +4 (written through bytes: it is not a field).
    D3D12_RESOURCE_DESC tuple{};
    tuple.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    tuple.Alignment = 0;
    tuple.Width = 0x30000;
    tuple.Height = 1;
    tuple.DepthOrArraySize = 1;
    tuple.MipLevels = 1;
    tuple.Format = DXGI_FORMAT_UNKNOWN;
    tuple.SampleDesc.Count = 1;
    tuple.SampleDesc.Quality = 0;
    tuple.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x1);
    tuple.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x00020004);
    static_assert(offsetof(D3D12_RESOURCE_DESC, Alignment) == 8, "the padding DWORD is at +4");
    const std::uint32_t padding = 0xA5C3E1F7u;
    std::memcpy(reinterpret_cast<unsigned char*>(&tuple) + 4, &padding, sizeof(padding));
    unsigned char original[sizeof(tuple)];
    std::memcpy(original, &tuple, sizeof(tuple));
    constexpr std::uint64_t kStateWord = 0xFFFFFFFFull;

    {
        void* placed = reinterpret_cast<void*>(1);
        const int before = g_seen133.calls;
        const HRESULT hr = call(&tuple, kStateWord, nullptr, &placed);
        EXPECT(std::memcmp(original, &tuple, sizeof(tuple)) == 0);  // the title's description, byte for byte
        if (expect_on) {
            EXPECT(hr == S_OK && placed == reinterpret_cast<void*>(0x1330) && g_seen133.calls == before + 1);
            const D3D12_RESOURCE_DESC& d = g_seen133.desc;
            EXPECT(d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && d.Alignment == 0 && d.Width == tuple.Width &&
                   d.Height == 1 && d.DepthOrArraySize == 1 && d.MipLevels == 1 && d.Format == DXGI_FORMAT_UNKNOWN &&
                   d.SampleDesc.Count == 1 && d.SampleDesc.Quality == 0 && d.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
            EXPECT(static_cast<std::uint32_t>(d.Flags) == 0x4u && g_seen133.state == 0u);
            EXPECT(g_seen133.real == xs::device_proxy_real_device(device) && g_seen133.va == va);
            EXPECT(std::memcmp(&g_seen133.iid, &resource_iid, sizeof(GUID)) == 0);
            // The handler's real failure comes back as it is, with the output null.
            for (const HRESULT failure : {E_NOTIMPL, E_INVALIDARG, E_OUTOFMEMORY}) {
                g_seen133.result = failure;
                void* failed = reinterpret_cast<void*>(1);
                EXPECT(call(&tuple, kStateWord, nullptr, &failed) == failure && !failed);
            }
            g_seen133.result = S_OK;
        } else {
            EXPECT(hr == E_NOTIMPL && !placed && g_seen133.calls == before);
        }
    }

    // Near misses: refused by the proxy in both modes (flags 0x20004 are not a strict shape), handler not called.
    const auto refused = [&](const D3D12_RESOURCE_DESC& desc, std::uint64_t state, const void* clear, HRESULT want) {
        void* placed = reinterpret_cast<void*>(1);
        const int before = g_seen133.calls;
        return call(&desc, state, clear, &placed) == want && !placed && g_seen133.calls == before;
    };
    for (const std::uint32_t flags : {0x20000u, 0x2000Cu, 0x20204u, 0x20005u, 0x60004u, 0x80020004u}) {
        D3D12_RESOURCE_DESC d = tuple;
        d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(flags);
        EXPECT(refused(d, kStateWord, nullptr, E_NOTIMPL));
    }
    for (const std::uint64_t state : {std::uint64_t{0}, std::uint64_t{0xFFFFFFFEull}, std::uint64_t{0x7FFFFFFFull},
                                      std::uint64_t{0x4}}) {
        EXPECT(refused(tuple, state, nullptr, E_NOTIMPL));
    }
    {
        const D3D12_CLEAR_VALUE clear_value{};
        EXPECT(refused(tuple, kStateWord, &clear_value, E_NOTIMPL));  // the strict flag check comes first
    }
    {
        D3D12_RESOURCE_DESC d = tuple;
        d.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        EXPECT(refused(d, kStateWord, nullptr, E_NOTIMPL));
        d = tuple;
        d.Width = 0;
        EXPECT(refused(d, kStateWord, nullptr, E_NOTIMPL));
        d = tuple;
        d.Height = 2;
        EXPECT(refused(d, kStateWord, nullptr, E_NOTIMPL));
        d = tuple;
        d.DepthOrArraySize = 2;
        EXPECT(refused(d, kStateWord, nullptr, E_NOTIMPL));
        d = tuple;
        d.MipLevels = 0;
        EXPECT(refused(d, kStateWord, nullptr, E_NOTIMPL));
        d = tuple;
        d.Format = DXGI_FORMAT_R8_UNORM;
        EXPECT(refused(d, kStateWord, nullptr, E_NOTIMPL));
        d = tuple;
        d.SampleDesc.Count = 2;
        EXPECT(refused(d, kStateWord, nullptr, E_NOTIMPL));
        d = tuple;
        d.SampleDesc.Quality = 1;
        EXPECT(refused(d, kStateWord, nullptr, E_NOTIMPL));
        for (const std::uint32_t layout : {0x0u, 0x2u, 0x101u, 0xFFFFFFFFu}) {
            d = tuple;
            d.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(layout);
            EXPECT(refused(d, kStateWord, nullptr, E_NOTIMPL));
        }
        d = tuple;
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        EXPECT(refused(d, kStateWord, nullptr, E_NOTIMPL));
    }
    {
        // Null pointers stay E_POINTER before anything else.
        const int before = g_seen133.calls;
        void* placed = reinterpret_cast<void*>(1);
        EXPECT(call(nullptr, kStateWord, nullptr, &placed) == E_POINTER && !placed);
        EXPECT(hr_of(raw(device, 133, word(va), word(&tuple), kStateWord, 0, 0, word(&placed))) == E_POINTER && !placed);
        EXPECT(g_seen133.calls == before);
    }

    // The strict shapes are forwarded unchanged, byte for byte, in both modes.
    for (const std::uint32_t flags : {0x0u, 0x4u, 0x8u, 0xCu}) {
        for (const std::uint64_t state : {std::uint64_t{0}, kStateWord}) {
            D3D12_RESOURCE_DESC d = tuple;
            d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(flags);
            void* placed = nullptr;
            const int before = g_seen133.calls;
            EXPECT(call(&d, state, nullptr, &placed) == S_OK && placed == reinterpret_cast<void*>(0x1330));
            EXPECT(g_seen133.calls == before + 1 && std::memcmp(&g_seen133.desc, &d, sizeof(d)) == 0 &&
                   g_seen133.state == static_cast<std::uint32_t>(state));
        }
    }

    xs::set_placed_at_va_handler(nullptr);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: the exact experimental tuple %s; every near miss is refused before the handler; strict shapes are "
                "forwarded unchanged\n",
                g_label, expect_on ? "reaches the handler as flags 0x4 / state 0" : "is refused (strict)");
    return 0;
}

// ---- 9. Device slot 26, GetCustomHeapProperties, the console's way and the PC's ----

int check_custom_heap_properties() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 0, true);
    EXPECT(device);
    void* const* vtable = *reinterpret_cast<void* const* const*>(device);

    // The retail executable's call: the buffer for the struct in RCX, the device in RDX, node mask and heap type in
    // R8 and R9 (their upper halves are not the callee's to read). The buffer sits inside a pattern that must stay.
    struct Padded {
        std::uint8_t before[16];
        D3D12_HEAP_PROPERTIES value;
        std::uint8_t after[64];
    } padded;
    std::memset(&padded, 0xA5, sizeof(padded));
    using ConsoleFn = std::uint64_t(WINAPI*)(void*, void*, std::uint64_t, std::uint64_t);
    const std::uint64_t returned = reinterpret_cast<ConsoleFn>(vtable[26])(
        &padded.value, device, 0xdeadbeef00000001ull, 0xdeadbeef00000002ull);
    EXPECT(returned == word(&padded.value));
    // The PC method got (buffer, node mask, heap type) once, on the device behind the proxy.
    EXPECT(mock->calls[26] == 1);
    EXPECT(static_cast<std::uint32_t>(mock->args[26][1]) == 1 && static_cast<std::uint32_t>(mock->args[26][2]) == 2);
    EXPECT(padded.value.Type == D3D12_HEAP_TYPE_UPLOAD && padded.value.CreationNodeMask == 1 &&
           padded.value.VisibleNodeMask == kHeapMarker);
    for (const std::uint8_t byte : padded.before) EXPECT(byte == 0xA5);
    for (const std::uint8_t byte : padded.after) EXPECT(byte == 0xA5);
    // The proxy is what it was: main's forwarder took the buffer for the proxy, and a thunk that took the proxy
    // for the buffer would write the struct over it.
    EXPECT(xs::is_device_proxy(device) && word(xs::device_proxy_real_device(device)) == word(mock) &&
           xs::device_proxy_slot_count(device) == 44 && xs::device_proxy_ref_count(device) == 1);

    // A PC C++ caller: the proxy in RCX and its buffer in RDX. That call is forwarded as it is.
    const D3D12_HEAP_PROPERTIES pc = reinterpret_cast<ID3D12Device*>(device)->GetCustomHeapProperties(
        1, D3D12_HEAP_TYPE_READBACK);
    EXPECT(mock->calls[26] == 2);
    EXPECT(xs::device_slot_overridden(26));
    EXPECT(pc.Type == D3D12_HEAP_TYPE_READBACK && pc.CreationNodeMask == 1 && pc.VisibleNodeMask == kHeapMarker);

    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: device slot 26 with the buffer in RCX and the device in RDX reaches the PC method and writes "
                "only the struct; the PC form is forwarded unchanged\n", g_label);
    return 0;
}

// ---- 9a. Device slot 25, GetResourceAllocationInfo, the console's way and the PC's ----
//
// The console call: RCX = buffer for the 16-byte struct, RDX = device, R8 = node mask, R9 = count, the description
// array as the fifth word on the stack. The descriptions carry made-up flag, layout and padding values: the proxy hands
// the same pointer and bytes to the device and does not judge them.

int check_resource_allocation_info() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 0, true);
    EXPECT(device);
    void* const* vtable = *reinterpret_cast<void* const* const*>(device);
    const long host_refs = mock->refs;
    const unsigned long proxy_refs = xs::device_proxy_ref_count(device);

    D3D12_RESOURCE_DESC descs[2];
    std::memset(descs, 0x5C, sizeof(descs));  // the padding after Dimension keeps this pattern
    descs[0].Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    descs[0].Alignment = 0x10000;
    descs[0].Width = 0x1234;
    descs[0].Height = 0x567;
    descs[0].DepthOrArraySize = 6;
    descs[0].MipLevels = 9;
    descs[0].Format = DXGI_FORMAT_BC7_UNORM;
    descs[0].SampleDesc = {1, 0};
    descs[0].Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);
    descs[0].Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x80020000u);
    descs[1].Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    descs[1].Alignment = 0;
    descs[1].Width = 0xABCDE;
    descs[1].Height = 1;
    descs[1].DepthOrArraySize = 1;
    descs[1].MipLevels = 1;
    descs[1].Format = DXGI_FORMAT_UNKNOWN;
    descs[1].SampleDesc = {1, 0};
    descs[1].Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x1);
    descs[1].Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x20004u);
    std::uint8_t original[sizeof(descs)];
    std::memcpy(original, descs, sizeof(descs));

    struct Padded {
        std::uint8_t before[16];
        D3D12_RESOURCE_ALLOCATION_INFO value;
        std::uint8_t after[64];
    };
    using ConsoleFn = std::uint64_t(WINAPI*)(void*, void*, std::uint64_t, std::uint64_t, const D3D12_RESOURCE_DESC*);
    const auto console = reinterpret_cast<ConsoleFn>(vtable[25]);
    EXPECT(xs::device_slot_overridden(25));

    // Two answers: ordinary values in both fields, and UINT64_MAX (a rejected description), each passed back as is.
    const D3D12_RESOURCE_ALLOCATION_INFO answers[] = {{0x0000000123456000ull, 0x0000000000010000ull},
                                                      {UINT64_MAX, 0x0000000000000200ull}};
    for (const D3D12_RESOURCE_ALLOCATION_INFO& answer : answers) {
        g_alloc_info = {};
        g_alloc_info.answer = answer;
        Padded padded;
        std::memset(&padded, 0xA5, sizeof(padded));
        const auto hits = xs::device_slot_hits(25);
        const std::uint64_t returned =
            console(&padded.value, device, 0xdeadbeef00000001ull, 0xfeedface00000002ull, descs);
        EXPECT(returned == word(&padded.value));
        EXPECT(g_alloc_info.calls == 1 && mock->calls[25] == static_cast<unsigned>(&answer - answers) + 1);
        EXPECT(g_alloc_info.self == mock);
        EXPECT(static_cast<std::uint32_t>(g_alloc_info.mask) == 1 && static_cast<std::uint32_t>(g_alloc_info.count) == 2);
        EXPECT(g_alloc_info.descs == descs);  // the title's array itself, not a copy
        EXPECT(std::memcmp(g_alloc_info.seen, original, sizeof(original)) == 0);
        EXPECT(std::memcmp(descs, original, sizeof(original)) == 0);
        EXPECT(padded.value.SizeInBytes == answer.SizeInBytes && padded.value.Alignment == answer.Alignment);
        for (const std::uint8_t byte : padded.before) EXPECT(byte == 0xA5);
        for (const std::uint8_t byte : padded.after) EXPECT(byte == 0xA5);
        EXPECT(xs::device_slot_hits(25) == hits + 1);
        EXPECT(xs::is_device_proxy(device) && word(xs::device_proxy_real_device(device)) == word(mock) &&
               xs::device_proxy_slot_count(device) == 44 && xs::device_proxy_ref_count(device) == proxy_refs &&
               mock->refs == host_refs);
    }

    // A PC C++ caller: the proxy in RCX and its buffer in RDX; the same host method, the same arguments and answer.
    for (const D3D12_RESOURCE_ALLOCATION_INFO& answer : answers) {
        g_alloc_info = {};
        g_alloc_info.answer = answer;
        const D3D12_RESOURCE_ALLOCATION_INFO pc =
            reinterpret_cast<ID3D12Device*>(device)->GetResourceAllocationInfo(1, 2, descs);
        EXPECT(g_alloc_info.calls == 1 && g_alloc_info.self == mock);
        EXPECT(g_alloc_info.out != device && g_alloc_info.out != nullptr);
        EXPECT(static_cast<std::uint32_t>(g_alloc_info.mask) == 1 && static_cast<std::uint32_t>(g_alloc_info.count) == 2);
        EXPECT(g_alloc_info.descs == descs && std::memcmp(g_alloc_info.seen, original, sizeof(original)) == 0);
        EXPECT(std::memcmp(descs, original, sizeof(original)) == 0);
        EXPECT(pc.SizeInBytes == answer.SizeInBytes && pc.Alignment == answer.Alignment);
        EXPECT(xs::device_proxy_ref_count(device) == proxy_refs && mock->refs == host_refs);
    }
    EXPECT(mock->calls[25] == 4 && mock->calls[26] == 0 && mock->calls[43] == 0);

    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: device slot 25 with the buffer in RCX, the device in RDX and the descriptions on the stack reaches "
                "the PC method once with the title's array unchanged and returns its answer (UINT64_MAX included); the "
                "PC form is forwarded unchanged\n", g_label);
    return 0;
}

// ---- 9b. Title-backed descriptor heaps and CPU descriptor handles (upstream PRs #7021 and #7880, adapted) ----
//
// A synthetic allocator stands in for the bridge to __XMemAllocationHooks__ (d3d12_xs.cpp; its registry side is in
// device_proxy_test and loader_test). Its answer is set per call; every allocation and release is recorded, so each
// failure path can show that the backing is released exactly once, by the pair that allocated it, after the real heap.

struct BackingState {
    xs::DescriptorBackingStatus status = xs::DescriptorBackingStatus::allocated;
    std::uint64_t next = 0;  // the address the next allocation returns
    int allocations = 0;
    std::size_t last_size = 0;
    std::uint64_t last_attributes = 0;
    int releases = 0;
    void* last_released = nullptr;
    std::uint64_t last_release_attributes = 0;
    std::uint64_t last_release_token = 0;
    void* last_release_hook = nullptr;
    long real_refs_at_release = -1;  // the released heap's real mock references when its backing was freed
    Mock* watch = nullptr;           // the real heap whose references are sampled at release
} g_backing;

constexpr std::uint64_t kBackingTokenBase = 0x7000;
void* const kReleaseHookTag = reinterpret_cast<void*>(0x5150);

xs::DescriptorBackingStatus mock_backing_allocate(std::size_t size, std::uint64_t attributes,
                                                  xs::DescriptorHeapBacking* out) {
    ++g_backing.allocations;
    g_backing.last_size = size;
    g_backing.last_attributes = attributes;
    if (g_backing.status != xs::DescriptorBackingStatus::allocated) return g_backing.status;
    out->address = reinterpret_cast<void*>(g_backing.next);
    out->token = kBackingTokenBase + static_cast<std::uint64_t>(g_backing.allocations);
    out->release_hook = kReleaseHookTag;
    return xs::DescriptorBackingStatus::allocated;
}

void mock_backing_release(const xs::DescriptorHeapBacking& backing, std::uint64_t attributes) {
    ++g_backing.releases;
    g_backing.last_released = backing.address;
    g_backing.last_release_attributes = attributes;
    g_backing.last_release_token = backing.token;
    g_backing.last_release_hook = backing.release_hook;
    g_backing.real_refs_at_release = g_backing.watch ? g_backing.watch->refs : -1;
}

const xs::DescriptorHeapAllocator kMockBackingAllocator{&mock_backing_allocate, &mock_backing_release};

const IID kHeapIid = __uuidof(ID3D12DescriptorHeap);

// Device slot 14 called the console way; the new heap proxy (or null) and the result.
HRESULT make_heap(void* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, void** heap, const IID& iid = kHeapIid) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = count;
    desc.Flags = type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                                                                : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    *heap = reinterpret_cast<void*>(1);
    return hr_of(raw(device, 14, word(&desc), word(&iid), word(heap)));
}

Mock* real_heap_of(void* heap) { return reinterpret_cast<Mock*>(xs::heap_proxy_real_heap(heap)); }

xs::DescriptorHandleTranslation translate(void* device, std::uint64_t ptr, std::uint64_t count,
                                          D3D12_DESCRIPTOR_HEAP_TYPE type, std::uint64_t* out = nullptr) {
    D3D12_CPU_DESCRIPTOR_HANDLE translated{};
    const auto result = xs::translate_cpu_descriptor_range(device, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(ptr)},
                                                           count, type, &translated);
    if (out) *out = translated.ptr;
    return result;
}

int check_descriptor_heaps() {
    using T = xs::DescriptorHandleTranslation;
    constexpr auto kCbv = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    g_backing = {};
    xs::set_descriptor_heap_allocator(nullptr);
    xs::set_default_descriptor_heap_allocator(nullptr);
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    const std::size_t tracked_before = xs::tracked_descriptor_heap_count();
    EXPECT(xs::title_descriptor_range_count() == 0);
    EXPECT(xs::device_slot_overridden(17) && xs::device_slot_overridden(23) && xs::device_slot_overridden(24));

    // No allocator: a plain heap, slot 9 the real start, and every handle passes through unchanged (the previous
    // behaviour) while no title-backed heap exists, an unknown pointer included.
    void* plain = nullptr;
    EXPECT(make_heap(device, kCbv, 4, &plain) == S_OK && xs::is_heap_proxy(plain));
    Mock* const plain_real = real_heap_of(plain);
    EXPECT(raw(plain, 9) == plain_real->heap_cpu && raw(plain, 10) == plain_real->heap_gpu);
    EXPECT(xs::tracked_descriptor_heap_count() == tracked_before + 1 && xs::title_descriptor_range_count() == 0);
    raw(device, 17, 0x11, 0x4242000);
    EXPECT(mock->calls[17] == 1 && mock->args[17][0] == 0x11 && mock->args[17][1] == 0x4242000);
    EXPECT(translate(device, 0x4242000, 1, kCbv) == T::unchanged);

    // The capability check (no output) asks the real device and allocates nothing.
    xs::set_descriptor_heap_allocator(&kMockBackingAllocator);
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{kCbv, 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        EXPECT(hr_of(raw(device, 14, word(&desc), word(&kHeapIid), 0)) == S_FALSE && g_backing.allocations == 0);
    }

    // A title-backed heap: one allocation of NumDescriptors * increment with the D3D attribute word; slot 9 is the
    // title's address and slot 10 still the real GPU start.
    constexpr std::uint64_t kCustom = 0x0000610000000000ull;
    constexpr std::uint64_t kInc = 32;
    g_backing.next = kCustom;
    void* heap = nullptr;
    EXPECT(make_heap(device, kCbv, 8, &heap) == S_OK && xs::is_heap_proxy(heap));
    Mock* const heap_real = real_heap_of(heap);
    EXPECT(g_backing.allocations == 1 && g_backing.last_size == 8 * kInc);
    EXPECT(g_backing.last_attributes == xs::kXmemDescriptorHeapAttributes && xs::kXmemDescriptorHeapAttributes == 0x00800004);
    EXPECT(raw(heap, 9) == kCustom && raw(heap, 10) == heap_real->heap_gpu);
    EXPECT(xs::title_descriptor_range_count() == 1 && xs::device_proxy_ref_count(device) == device_refs + 2);
    const std::uint64_t real0 = heap_real->heap_cpu;

    // Range endpoints and overflow, through the public translator.
    std::uint64_t out = 0;
    EXPECT(translate(device, kCustom, 1, kCbv, &out) == T::translated && out == real0);
    EXPECT(translate(device, kCustom, 8, kCbv, &out) == T::translated && out == real0);
    EXPECT(translate(device, kCustom + 7 * kInc, 1, kCbv, &out) == T::translated && out == real0 + 7 * kInc);
    EXPECT(translate(device, kCustom, 0, kCbv, &out) == T::translated);  // 0 counts as one descriptor
    EXPECT(translate(device, kCustom, 9, kCbv, &out) == T::invalid && out == kCustom);
    EXPECT(translate(device, kCustom + 7 * kInc, 2, kCbv) == T::invalid);
    EXPECT(translate(device, kCustom + kInc, ~0ull, kCbv) == T::invalid);
    EXPECT(translate(device, kCustom + 8 * kInc, 1, kCbv) == T::unchanged);  // outside registered custom ranges
    EXPECT(translate(device, kCustom - kInc, 1, kCbv) == T::unchanged);
    EXPECT(translate(device, kCustom + kInc / 2, 1, kCbv) == T::invalid);  // not on a descriptor boundary
    EXPECT(translate(device, kCustom, 1, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) == T::invalid);  // wrong heap type
    EXPECT(translate(device, 0, 1, kCbv, &out) == T::unchanged && out == 0);
    // A normal PC heap's handle stays valid and unchanged; a real start of the title heap too (never handed out).
    EXPECT(translate(device, plain_real->heap_cpu + 3 * kInc, 1, kCbv, &out) == T::unchanged &&
           out == plain_real->heap_cpu + 3 * kInc);
    EXPECT(translate(device, real0, 1, kCbv) == T::unchanged);
    EXPECT(translate(device, 0x4242000, 1, kCbv) == T::unchanged);  // untracked native handles remain valid
    // A heap created directly on the backend is absent from the proxy registry. Its native handles must remain
    // usable while a custom heap is live, including a mixed native/custom descriptor copy.
    {
        void* native_heap = nullptr;
        const auto tracked = xs::tracked_descriptor_heap_count();
        EXPECT(make_heap(mock, kCbv, 4, &native_heap) == S_OK && !xs::is_heap_proxy(native_heap));
        const auto native_start = static_cast<Mock*>(native_heap)->heap_cpu;
        EXPECT(xs::tracked_descriptor_heap_count() == tracked);
        EXPECT(translate(device, native_start, 1, kCbv, &out) == T::unchanged && out == native_start);
        const unsigned before = mock->calls[17];
        raw(device, 17, 0x11, native_start);
        EXPECT(mock->calls[17] == before + 1 && mock->args[17][1] == native_start);
        const unsigned copies = mock->calls[24];
        raw(device, 24, 1, native_start, kCustom, 0);
        EXPECT(mock->calls[24] == copies + 1 && mock->args[24][1] == native_start && mock->args[24][2] == real0);
        EXPECT(release(native_heap) == 0);
        mock->calls[17] = before;
        mock->calls[24] = copies;
    }
    // The handle belongs to this device only.
    Mock* other_mock = nullptr;
    void* other = make_device(other_mock, 9, true);
    EXPECT(other && translate(other, kCustom, 1, kCbv) == T::invalid);

    // Device slots 17..24 forward translated handles.
    raw(device, 17, 0x11, kCustom + 3 * kInc);
    EXPECT(mock->calls[17] == 2 && mock->args[17][0] == 0x11 && mock->args[17][1] == real0 + 3 * kInc);
    raw(device, 18, 0x22, 0x33, kCustom + 3 * kInc);
    EXPECT(mock->calls[18] == 1 && mock->args[18][0] == 0x22 && mock->args[18][2] == real0 + 3 * kInc);
    raw(device, 19, 0x22, 0x44, 0x33, kCustom + 7 * kInc);
    EXPECT(mock->calls[19] == 1 && mock->args[19][1] == 0x44 && mock->args[19][3] == real0 + 7 * kInc);
    g_backing.next = 0x0000620000000000ull;
    void* rtv_heap = nullptr;
    EXPECT(make_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4, &rtv_heap) == S_OK && g_backing.last_size == 4 * 64);
    raw(device, 20, 0x22, 0x33, 0x0000620000000000ull + 2 * 64);
    EXPECT(mock->calls[20] == 1 && mock->args[20][2] == real_heap_of(rtv_heap)->heap_cpu + 2 * 64);
    g_backing.next = 0x0000630000000000ull;
    void* dsv_heap = nullptr;
    EXPECT(make_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, &dsv_heap) == S_OK && g_backing.last_size == 2 * 16);
    raw(device, 21, 0x22, 0x33, 0x0000630000000000ull + 16);
    EXPECT(mock->calls[21] == 1 && mock->args[21][2] == real_heap_of(dsv_heap)->heap_cpu + 16);
    g_backing.next = 0x0000640000000000ull;
    void* sampler_heap = nullptr;
    EXPECT(make_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2, &sampler_heap) == S_OK);
    raw(device, 22, 0x55, 0x0000640000000000ull + 32);
    EXPECT(mock->calls[22] == 1 && mock->args[22][1] == real_heap_of(sampler_heap)->heap_cpu + 32);
    {
        // CopyDescriptors (type word 0 = CBV_SRV_UAV): each range over its size, in copies; sizes pass unchanged.
        const D3D12_CPU_DESCRIPTOR_HANDLE dests[2] = {{static_cast<SIZE_T>(kCustom + 3 * kInc)},
                                                      {static_cast<SIZE_T>(plain_real->heap_cpu)}};
        const UINT dest_sizes[2] = {2, 1};
        const D3D12_CPU_DESCRIPTOR_HANDLE srcs[1] = {{static_cast<SIZE_T>(kCustom + 5 * kInc)}};
        const UINT src_sizes[1] = {3};
        raw(device, 23, 2, word(dests), word(dest_sizes), 1, word(srcs), word(src_sizes));
        EXPECT(mock->calls[23] == 1 && mock->array[23][0] == real0 + 3 * kInc &&
               mock->array[23][1] == plain_real->heap_cpu && mock->copy_src[0] == real0 + 5 * kInc);
        EXPECT(mock->args[23][2] == word(dest_sizes) && dests[0].ptr == kCustom + 3 * kInc);  // caller not written
        // Null size arrays mean ranges of one.
        raw(device, 23, 1, word(dests), 0, 1, word(srcs), 0);
        EXPECT(mock->calls[23] == 2 && mock->array[23][0] == real0 + 3 * kInc);
        // A source range running past the heap's end: nothing is forwarded.
        const UINT too_long[1] = {4};
        const std::uint64_t refused = xs::refused_descriptor_handle_calls();
        raw(device, 23, 2, word(dests), word(dest_sizes), 1, word(srcs), word(too_long));
        EXPECT(mock->calls[23] == 2 && xs::refused_descriptor_handle_calls() == refused + 1);
        // More than 16 entries take the allocated copy; its allocation failing refuses the call.
        D3D12_CPU_DESCRIPTOR_HANDLE many[20];
        for (auto& h : many) h.ptr = static_cast<SIZE_T>(kCustom);
        raw(device, 23, 20, word(many), 0, 1, word(srcs), 0);
        EXPECT(mock->calls[23] == 3 && mock->array[23][3] == real0);
        g_fail_news = 1;
        raw(device, 23, 20, word(many), 0, 1, word(srcs), 0);
        g_fail_news = 0;
        EXPECT(mock->calls[23] == 3 && xs::refused_descriptor_handle_calls() == refused + 2);
    }
    raw(device, 24, 2, kCustom + kInc, kCustom + 5 * kInc, 0);
    EXPECT(mock->calls[24] == 1 && mock->args[24][0] == 2 && mock->args[24][1] == real0 + kInc &&
           mock->args[24][2] == real0 + 5 * kInc);

    // Invalid handles are never forwarded: counted and refused.
    {
        const std::uint64_t refused = xs::refused_descriptor_handle_calls();
        const unsigned before17 = mock->calls[17], before20 = mock->calls[20], before24 = mock->calls[24];
        raw(device, 17, 0x11, kCustom + 1);                            // misaligned custom handle
        raw(device, 17, 0x11, kCustom + 2);                            // another misaligned custom handle
        raw(device, 20, 0x22, 0x33, kCustom);                          // a CBV heap's handle for an RTV
        raw(device, 24, 3, kCustom + 6 * kInc, kCustom, 0);            // destination span past the end
        raw(device, 24, 1, kCustom, kCustom + kInc / 2, 0);            // misaligned source
        EXPECT(mock->calls[17] == before17 && mock->calls[20] == before20 && mock->calls[24] == before24);
        EXPECT(xs::refused_descriptor_handle_calls() == refused + 5);
        // The plain heap's genuine handle still goes through.
        raw(device, 17, 0x11, plain_real->heap_cpu + kInc);
        EXPECT(mock->calls[17] == before17 + 1 && mock->args[17][1] == plain_real->heap_cpu + kInc);
    }

    // QueryInterface lifetime: a second reference keeps the heap and its range; the last Release frees the backing
    // once, after the real heap, through the allocating pair.
    {
        void* same = nullptr;
        EXPECT(hr_of(raw(heap, 0, word(&kHeapIid), word(&same))) == S_OK && same == heap);
        const int releases = g_backing.releases;
        EXPECT(release(same) == 1 && g_backing.releases == releases);
        EXPECT(translate(device, kCustom, 1, kCbv) == T::translated);
        g_backing.watch = heap_real;
        const std::uint64_t token = kBackingTokenBase + 1;  // the heap's allocation was the first
        EXPECT(release(heap) == 0 && g_backing.releases == releases + 1);
        EXPECT(g_backing.last_released == reinterpret_cast<void*>(kCustom) && g_backing.last_release_token == token &&
               g_backing.last_release_hook == kReleaseHookTag &&
               g_backing.last_release_attributes == xs::kXmemDescriptorHeapAttributes);
        EXPECT(g_backing.real_refs_at_release == 0 && heap_real->refs == 0);
        g_backing.watch = nullptr;
        EXPECT(xs::title_descriptor_range_count() == 3);  // RTV, DSV and sampler heaps remain
        EXPECT(translate(device, kCustom, 1, kCbv) == T::unchanged);  // no longer classified as custom
    }

    // Failures: each releases the real heap and any backing exactly once, hands out nothing, and leaves the device's
    // references and the range table as they were.
    const auto expect_refused = [&](HRESULT want, int want_releases, const IID& iid = kHeapIid) {
        const unsigned long refs = xs::device_proxy_ref_count(device);
        const std::size_t ranges = xs::title_descriptor_range_count();
        const std::size_t tracked = xs::tracked_descriptor_heap_count();
        const int releases = g_backing.releases;
        void* refused = nullptr;
        const HRESULT hr = make_heap(device, kCbv, 4, &refused, iid);
        return hr == want && refused == nullptr && mock->last_child->kind == Kind::heap &&
               mock->last_child->refs == 0 && g_backing.releases == releases + want_releases &&
               xs::device_proxy_ref_count(device) == refs && xs::title_descriptor_range_count() == ranges &&
               xs::tracked_descriptor_heap_count() == tracked;
    };
    // Never invoke an allocator that cannot release its result.
    {
        const xs::DescriptorHeapAllocator unpaired{&mock_backing_allocate, nullptr};
        const int allocations = g_backing.allocations;
        xs::set_descriptor_heap_allocator(&unpaired);
        EXPECT(expect_refused(E_FAIL, 0) && g_backing.allocations == allocations);
        xs::set_descriptor_heap_allocator(&kMockBackingAllocator);
    }
    g_backing.status = xs::DescriptorBackingStatus::out_of_memory;
    EXPECT(expect_refused(E_OUTOFMEMORY, 0));  // the hook returned no memory: the heap fails, nothing to free
    g_backing.status = xs::DescriptorBackingStatus::invalid_hooks;
    EXPECT(expect_refused(E_FAIL, 0));
    g_backing.status = xs::DescriptorBackingStatus::allocated;
    g_backing.next = 0;
    EXPECT(expect_refused(E_FAIL, 0));  // "allocated" with a null address
    g_backing.next = ~0ull - 16;
    EXPECT(expect_refused(E_FAIL, 1) && g_backing.last_released == reinterpret_cast<void*>(~0ull - 16));  // wraps
    g_backing.next = 0x0000620000000000ull + 64;  // inside the live RTV heap's title range
    EXPECT(expect_refused(E_FAIL, 1));
    g_backing.next = plain_real->heap_cpu;  // aliases a live real range
    EXPECT(expect_refused(E_FAIL, 1));
    g_backing.next = 0x0000650000000000ull;
    EXPECT(expect_refused(E_NOINTERFACE, 1, kExtraIid));  // QueryInterface refused after registration
    EXPECT(g_backing.last_released == reinterpret_cast<void*>(0x0000650000000000ull));
    g_fail_news = 1;  // the heap proxy's own allocation
    EXPECT(expect_refused(E_OUTOFMEMORY, 1));
    g_fail_news = 0;
    EXPECT(translate(device, 0x0000650000000000ull, 1, kCbv) == T::unchanged);  // no range was left behind

    // A heap of zero descriptors asks the hooks for nothing.
    {
        const int allocations = g_backing.allocations;
        void* empty = nullptr;
        EXPECT(make_heap(device, kCbv, 0, &empty) == S_OK && g_backing.allocations == allocations);
        EXPECT(raw(empty, 9) == real_heap_of(empty)->heap_cpu && release(empty) == 0);
    }
    // The hooks answering "not hooked": a plain heap even while title heaps exist; its handles pass unchanged.
    {
        g_backing.status = xs::DescriptorBackingStatus::not_hooked;
        void* unhooked = nullptr;
        EXPECT(make_heap(device, kCbv, 2, &unhooked) == S_OK);
        Mock* const unhooked_real = real_heap_of(unhooked);
        EXPECT(raw(unhooked, 9) == unhooked_real->heap_cpu);
        EXPECT(translate(device, unhooked_real->heap_cpu + kInc, 1, kCbv) == T::unchanged);
        EXPECT(release(unhooked) == 0 && unhooked_real->refs == 0);
        g_backing.status = xs::DescriptorBackingStatus::allocated;
    }
    // An explicit allocator wins over the default; clearing it restores the default.
    {
        constexpr xs::DescriptorHeapAllocator kNever{
            [](std::size_t, std::uint64_t, xs::DescriptorHeapBacking*) {
                return xs::DescriptorBackingStatus::not_hooked;
            },
            [](const xs::DescriptorHeapBacking&, std::uint64_t) {}};
        xs::set_default_descriptor_heap_allocator(&kMockBackingAllocator);
        xs::set_descriptor_heap_allocator(&kNever);
        const int allocations = g_backing.allocations;
        void* explicit_heap = nullptr;
        EXPECT(make_heap(device, kCbv, 2, &explicit_heap) == S_OK && g_backing.allocations == allocations);
        EXPECT(release(explicit_heap) == 0);
        xs::set_descriptor_heap_allocator(nullptr);
        g_backing.next = 0x0000660000000000ull;
        void* default_heap = nullptr;
        EXPECT(make_heap(device, kCbv, 2, &default_heap) == S_OK && g_backing.allocations == allocations + 1);
        EXPECT(raw(default_heap, 9) == 0x0000660000000000ull && release(default_heap) == 0);
        xs::set_default_descriptor_heap_allocator(nullptr);
    }

    // Teardown: every backing freed once; the table back to where it started.
    const int releases = g_backing.releases;
    EXPECT(release(rtv_heap) == 0 && release(dsv_heap) == 0 && release(sampler_heap) == 0);
    EXPECT(g_backing.releases == releases + 3 && xs::title_descriptor_range_count() == 0);
    EXPECT(release(plain) == 0 && plain_real->refs == 0);
    EXPECT(xs::tracked_descriptor_heap_count() == tracked_before);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    EXPECT(release(other) == 0 && other_mock->refs == 0);
    xs::set_descriptor_heap_allocator(nullptr);
    std::printf("%s: slot 14 title backing (size, attributes, slot 9/10, every failure releases once), slots 17-24 "
                "translate checked spans and refuse invalid handles, plain heap handles pass unchanged\n", g_label);
    return 0;
}

// Console list slots 50..54 with title-backed RTV/DSV/UAV heaps: translated copies, single-range spans, the typed
// slot 51 (FLOAT Depth in XMM3), the genuine GPU handle of the UAV clears, and invalid handles marking the list.
int check_list_descriptor_handles() {
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    const std::uint32_t direct_desc[4] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    g_backing = {};
    xs::set_descriptor_heap_allocator(&kMockBackingAllocator);
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true, true, true);
    EXPECT(device);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, word(&allocator_iid), word(&allocator))) == S_OK);
    void* list = nullptr;
    EXPECT(hr_of(raw(device, 73, word(direct_desc), word(allocator), 0, word(&kConsoleListIid), word(&list))) == S_OK);
    Mock* const real = mock->last_child;
    EXPECT(xs::is_list_proxy(list) && real->kind == Kind::list);

    constexpr std::uint64_t kRtv = 0x0000710000000000ull, kDsv = 0x0000720000000000ull, kUav = 0x0000730000000000ull;
    void* rtv_heap = nullptr;
    void* dsv_heap = nullptr;
    void* uav_heap = nullptr;
    void* plain_rtv = nullptr;
    g_backing.next = kRtv;
    EXPECT(make_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4, &rtv_heap) == S_OK);
    g_backing.next = kDsv;
    EXPECT(make_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, &dsv_heap) == S_OK);
    g_backing.next = kUav;
    EXPECT(make_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, &uav_heap) == S_OK);
    g_backing.status = xs::DescriptorBackingStatus::not_hooked;
    EXPECT(make_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, &plain_rtv) == S_OK);
    g_backing.status = xs::DescriptorBackingStatus::allocated;
    const std::uint64_t rtv_real = real_heap_of(rtv_heap)->heap_cpu, dsv_real = real_heap_of(dsv_heap)->heap_cpu;
    const std::uint64_t uav_real = real_heap_of(uav_heap)->heap_cpu, plain_real = real_heap_of(plain_rtv)->heap_cpu;

    // Console 50 -> PC 46, an array: each entry translated into a copy (a plain heap's entry unchanged), the DSV too.
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = {{static_cast<SIZE_T>(kRtv + 64)}, {static_cast<SIZE_T>(plain_real)},
                                           {static_cast<SIZE_T>(kRtv + 3 * 64)}};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{static_cast<SIZE_T>(kDsv + 16)};
    raw(list, 50, 3, word(rtvs), 0, word(&dsv));
    EXPECT(real->calls[46] == 1 && real->args[46][0] == 3 && real->args[46][2] == 0);
    EXPECT(real->array[46][0] == rtv_real + 64 && real->array[46][1] == plain_real &&
           real->array[46][2] == rtv_real + 3 * 64 && real->om_dsv == dsv_real + 16);
    EXPECT(real->args[46][1] != word(rtvs) && real->args[46][3] != word(&dsv));  // copies, not the caller's arrays
    EXPECT(rtvs[0].ptr == kRtv + 64 && dsv.ptr == kDsv + 16);
    // A single range: only [0] is read, and its whole span of `count` descriptors is checked.
    D3D12_CPU_DESCRIPTOR_HANDLE range_start{static_cast<SIZE_T>(kRtv)};
    raw(list, 50, 4, word(&range_start), 1, 0);
    EXPECT(real->calls[46] == 2 && real->array[46][0] == rtv_real && real->args[46][2] == 1 &&
           real->args[46][3] == 0 && real->om_dsv == 0);
    // No render targets and no DSV: forwarded as it is.
    raw(list, 50, 0, 0, 0, 0);
    EXPECT(real->calls[46] == 3 && real->args[46][1] == 0 && real->args[46][3] == 0);

    // Console 52 -> PC 48 ClearRenderTargetView: the handle translated, the color pointer unchanged.
    const FLOAT color[4] = {1.0f, 0.0f, 128.0f / 255.0f, 1.0f};
    raw(list, 52, kRtv + 2 * 64, word(color), 0, 0);
    EXPECT(real->calls[48] == 1 && real->args[48][0] == rtv_real + 2 * 64 && real->args[48][1] == word(color));
    // Console 53/54 -> PC 49/50: the GPU handle is the genuine one and passes unchanged; the CPU handle is translated.
    const UINT uint_values[4] = {1, 2, 3, 4};
    const FLOAT float_values[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    constexpr std::uint64_t kGpuHandle = 0x0000580000abc000ull;
    raw(list, 53, kGpuHandle, kUav + 32, 0x1234, word(uint_values), 0, 0);
    EXPECT(real->calls[49] == 1 && real->args[49][0] == kGpuHandle && real->args[49][1] == uav_real + 32 &&
           real->args[49][2] == 0x1234 && real->args[49][3] == word(uint_values));
    raw(list, 54, kGpuHandle, kUav + 3 * 32, 0x1234, word(float_values), 0, 0);
    EXPECT(real->calls[50] == 1 && real->args[50][0] == kGpuHandle && real->args[50][1] == uav_real + 3 * 32);

    // Console 51 -> PC 47 through the typed thunk: Depth arrives in XMM3 and is forwarded there, bit for bit.
    using ClearDsvFn = void(WINAPI*)(void*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT,
                                     const D3D12_RECT*);
    const auto clear_dsv = reinterpret_cast<ClearDsvFn>((*reinterpret_cast<void* const* const*>(list))[51]);
    const D3D12_RECT rect{0, 0, 2, 2};
    clear_dsv(list, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(kDsv + 16)},
              D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.25f, 7, 1, &rect);
    EXPECT(real->calls[47] == 1 && real->args[47][0] == dsv_real + 16 &&
           real->args[47][1] == (D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL));
    EXPECT(real->args[47][2] == 0x3e800000u && real->args[47][3] == 7 && real->args[47][4] == 1 &&
           real->array[47][0] == word(&rect));  // 0.25f
    clear_dsv(list, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(kDsv)}, D3D12_CLEAR_FLAG_DEPTH, 0.75f, 0, 0,
              nullptr);
    EXPECT(real->calls[47] == 2 && real->args[47][0] == dsv_real && real->args[47][2] == 0x3f400000u);  // 0.75f
    EXPECT(xs::list_proxy_unsupported_slot(list) == 0 && hr_of(raw(list, 9)) == S_OK);

    // Invalid handles mark the list instead of reaching the real one; Close fails until Reset.
    const auto expect_marked = [&](unsigned slot, auto&& call, unsigned pc) {
        if (hr_of(raw(list, 139, word(allocator), 0)) != S_OK) return false;
        const unsigned before = real->calls[pc];
        call();
        return real->calls[pc] == before && xs::list_proxy_unsupported_slot(list) == slot &&
               hr_of(raw(list, 9)) == E_NOTIMPL;
    };
    D3D12_CPU_DESCRIPTOR_HANDLE past_end{static_cast<SIZE_T>(kRtv + 3 * 64)};
    EXPECT(expect_marked(50, [&] { raw(list, 50, 2, word(&past_end), 1, 0); }, 46));  // single range past the end
    D3D12_CPU_DESCRIPTOR_HANDLE nine[9] = {};
    EXPECT(expect_marked(50, [&] { raw(list, 50, 9, word(nine), 0, 0); }, 46));  // more than 8 targets
    D3D12_CPU_DESCRIPTOR_HANDLE wrong_type{static_cast<SIZE_T>(kUav)};
    EXPECT(expect_marked(50, [&] { raw(list, 50, 1, word(rtvs), 0, word(&wrong_type)); }, 46));  // a UAV as DSV
    EXPECT(expect_marked(52, [&] { raw(list, 52, kRtv + 1, word(color), 0, 0); }, 48));  // misaligned custom pointer
    EXPECT(expect_marked(53, [&] { raw(list, 53, kGpuHandle, kUav + 16, 0x1234, word(uint_values), 0, 0); }, 49));
    EXPECT(expect_marked(54, [&] { raw(list, 54, kGpuHandle, kRtv, 0x1234, word(float_values), 0, 0); }, 50));
    EXPECT(expect_marked(51, [&] {
        clear_dsv(list, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(kDsv + 8)}, D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0,
                  nullptr);
    }, 47));
    EXPECT(hr_of(raw(list, 139, word(allocator), 0)) == S_OK && hr_of(raw(list, 9)) == S_OK);

    EXPECT(release(list) == 0 && real->refs == 0 && release(allocator) == 0);
    const int releases = g_backing.releases;
    EXPECT(release(rtv_heap) == 0 && release(dsv_heap) == 0 && release(uav_heap) == 0 && release(plain_rtv) == 0);
    EXPECT(g_backing.releases == releases + 3 && xs::title_descriptor_range_count() == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    xs::set_descriptor_heap_allocator(nullptr);
    std::printf("%s: console list slots 50-54 translate title-backed CPU handles in copies, keep the GPU handle and "
                "FLOAT Depth (XMM3), and mark the list for an invalid handle\n", g_label);
    return 0;
}

// ---- Device slot 70: a raw buffer SRV named by GPU VA, never the PC CreatePlacedResource1 ----
//
// The raw console call void(this, resource desc*, 64-byte view*, CPU handle) through the proxy's vtable. The mock
// device stands in for the backend: its slot 18 (CreateShaderResourceView) records the resource, the description and
// the handle it receives; its slot 70 (the PC CreatePlacedResource1) must never be reached. The buffers are mock
// resources found by a resolver that, like the Vulkan layer's, only answers buffers of the device it is asked about
// that are still alive, and hands one reference to the caller.

struct FakeBuffer {
    Mock* resource;      // refs 0 = released (what a failed try_add_ref sees)
    const Mock* owner;   // the PC device it belongs to
    std::uint64_t start;
    std::uint64_t size;
    bool deny;           // DENY_SHADER_RESOURCE
};
enum class ResolverMode { normal, fail, short_range, ref_with_not_found };
struct ResolverState {
    FakeBuffer buffers[4];
    unsigned count;
    ResolverMode mode;
    unsigned calls;
    const void* last_real;
    std::uint64_t last_va;
    std::uint64_t last_bytes;
} g_resolver;

xs::BufferAtVaStatus fake_buffer_resolver(ID3D12Device* real, std::uint64_t va, std::uint64_t bytes,
                                          xs::BufferAtVa* out) {
    ++g_resolver.calls;
    g_resolver.last_real = real;
    g_resolver.last_va = va;
    g_resolver.last_bytes = bytes;
    *out = {};
    if (g_resolver.mode == ResolverMode::fail) return xs::BufferAtVaStatus::failed;
    for (unsigned i = 0; i < g_resolver.count; ++i) {
        const FakeBuffer& b = g_resolver.buffers[i];
        if (b.owner != reinterpret_cast<const Mock*>(real) || !b.resource || b.resource->refs <= 0) continue;
        if (va < b.start || va - b.start >= b.size || bytes > b.size - (va - b.start)) continue;
        if (b.deny) return xs::BufferAtVaStatus::not_shader_readable;
        ++b.resource->refs;  // the caller's reference
        out->resource = reinterpret_cast<ID3D12Resource*>(b.resource);
        out->start = b.start;
        out->size = b.size;
        // A broken resolver: an answer that does not hold the request, or a reference with a non-found status.
        if (g_resolver.mode == ResolverMode::short_range) out->size = (va - b.start) + bytes - 4;
        if (g_resolver.mode == ResolverMode::ref_with_not_found) return xs::BufferAtVaStatus::not_found;
        return xs::BufferAtVaStatus::found;
    }
    return xs::BufferAtVaStatus::not_found;
}

// The two descriptions as raw bytes, the way the title lays them out, with garbage in every padding field and in the
// view's bytes 0x30..0x3F (no established meaning for a buffer view): the proxy must not read or reject them.
struct Slot70Args {
    alignas(8) std::uint8_t resource[sizeof(D3D12_RESOURCE_DESC)];
    alignas(8) std::uint8_t view[64];
};

void put32(std::uint8_t* at, std::size_t offset, std::uint32_t value) { std::memcpy(at + offset, &value, 4); }
void put64(std::uint8_t* at, std::size_t offset, std::uint64_t value) { std::memcpy(at + offset, &value, 8); }

// The measured shape (docs/d3d12_vulkan.md, "Device slot 70"), for any width.
Slot70Args slot70_args(std::uint64_t va, std::uint64_t width) {
    Slot70Args a{};
    D3D12_RESOURCE_DESC r{};
    r.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    r.Alignment = 0;
    r.Width = width;
    r.Height = 1;
    r.DepthOrArraySize = 1;
    r.MipLevels = 1;
    r.Format = DXGI_FORMAT_UNKNOWN;
    r.SampleDesc = {1, 0};
    r.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    r.Flags = D3D12_RESOURCE_FLAG_NONE;
    std::memcpy(a.resource, &r, sizeof(r));
    put32(a.resource, 4, 0xDEADBEEFu);  // padding after Dimension: uninitialized in the title
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = DXGI_FORMAT_R32_TYPELESS;
    v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Buffer.FirstElement = 0;
    v.Buffer.NumElements = static_cast<UINT>(width / 4);
    v.Buffer.StructureByteStride = 0;
    v.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    std::memcpy(a.view, &v, sizeof(v));
    put32(a.view, 0x0C, 0xCAFEF00Du);  // padding before the union
    put32(a.view, 0x24, 0x5EC0DE11u);  // padding after Buffer.Flags: uninitialized in the title
    put64(a.view, 0x28, va);
    std::memset(a.view + 0x30, 0xA5, 0x10);
    return a;
}

std::uint64_t call70(void* device, const void* resource, const void* view, std::uint64_t destination) {
    return raw(device, 70, word(resource), word(view), destination);
}

int check_srv_at_va() {
    using O = xs::SrvAtVaOutcome;
    constexpr auto kCbv = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    constexpr std::uint64_t kInc = 32;                         // the mock's CBV_SRV_UAV increment
    constexpr std::uint64_t kCustom = 0x0000630000000000ull;   // the title backing of the heap below
    constexpr std::uint64_t kMeasuredWidth = 182452224;        // 174 MiB, the measured buffer
    constexpr std::uint64_t kStart = 0x0000020000000000ull;    // a title VA, 64 KiB aligned
    g_backing = {};
    g_resolver = {};
    xs::set_default_descriptor_heap_allocator(nullptr);
    xs::set_descriptor_heap_allocator(nullptr);

    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);  // level 9: the PC slot 70 (CreatePlacedResource1) exists
    EXPECT(device && xs::device_slot_overridden(70));
    Mock* other_mock = nullptr;
    void* other = make_device(other_mock, 9, true);
    EXPECT(other);

    // Heaps: a plain one, a title-backed one, a sampler heap; another device's heap; one released before use.
    void* plain = nullptr;
    EXPECT(make_heap(device, kCbv, 4, &plain) == S_OK);
    void* sampler = nullptr;
    EXPECT(make_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 4, &sampler) == S_OK);
    void* foreign_heap = nullptr;
    EXPECT(make_heap(other, kCbv, 4, &foreign_heap) == S_OK);
    void* gone = nullptr;
    EXPECT(make_heap(device, kCbv, 4, &gone) == S_OK);
    const std::uint64_t gone_cpu = real_heap_of(gone)->heap_cpu;
    EXPECT(release(gone) == 0);
    xs::set_descriptor_heap_allocator(&kMockBackingAllocator);
    g_backing.next = kCustom;
    void* heap = nullptr;
    EXPECT(make_heap(device, kCbv, 8, &heap) == S_OK && raw(heap, 9) == kCustom);
    xs::set_descriptor_heap_allocator(nullptr);
    const std::uint64_t real0 = real_heap_of(heap)->heap_cpu;
    const std::uint64_t plain0 = real_heap_of(plain)->heap_cpu;

    // The live buffers: the measured 174 MiB import, a small one with DENY_SHADER_RESOURCE, one of the other device.
    Mock* buffer = new_mock(Kind::resource);
    buffer->refs = 1;
    Mock* denied = new_mock(Kind::resource);
    denied->refs = 1;
    Mock* foreign = new_mock(Kind::resource);
    foreign->refs = 1;
    constexpr std::uint64_t kDeniedStart = 0x0000030000000000ull, kForeignStart = 0x0000040000000000ull;
    g_resolver.buffers[0] = {buffer, mock, kStart, kMeasuredWidth, false};
    g_resolver.buffers[1] = {denied, mock, kDeniedStart, 0x10000, true};
    g_resolver.buffers[2] = {foreign, other_mock, kForeignStart, 0x10000, false};
    g_resolver.count = 3;
    EXPECT(xs::set_device_buffer_at_va_resolver(device, &fake_buffer_resolver));
    EXPECT(xs::set_device_buffer_at_va_resolver(other, &fake_buffer_resolver));
    EXPECT(!xs::set_device_buffer_at_va_resolver(mock, &fake_buffer_resolver));  // not a proxy: nothing set
    xs::reset_device_proxy_stats();
    xs::reset_srv_at_va_stats();

    // The measured call: the whole 174 MiB buffer by its base VA into a title-backed slot. The PC slot 18 receives the
    // live buffer, the translated handle and a raw view of exactly the bytes asked for; the reference is balanced; the
    // descriptions are not written; PC slot 70 is never reached.
    {
        const Slot70Args args = slot70_args(kStart, kMeasuredWidth);
        Slot70Args call = args;
        EXPECT(call70(device, call.resource, call.view, kCustom + 2 * kInc) == 0);
        EXPECT(std::memcmp(&call, &args, sizeof(args)) == 0);
        EXPECT(mock->calls[70] == 0 && xs::device_slot_hits(70) == 1);
        EXPECT(mock->calls[18] == 1 && mock->args[18][0] == word(buffer) && mock->args[18][2] == real0 + 2 * kInc);
        const D3D12_SHADER_RESOURCE_VIEW_DESC& srv = mock->srv;
        EXPECT(srv.Format == DXGI_FORMAT_R32_TYPELESS && srv.ViewDimension == D3D12_SRV_DIMENSION_BUFFER &&
               srv.Shader4ComponentMapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
        EXPECT(srv.Buffer.FirstElement == 0 && srv.Buffer.NumElements == kMeasuredWidth / 4 &&
               srv.Buffer.StructureByteStride == 0 && srv.Buffer.Flags == D3D12_BUFFER_SRV_FLAG_RAW);
        EXPECT(buffer->refs == 1);  // the resolver's reference was released after the call
        EXPECT(g_resolver.calls == 1 && g_resolver.last_real == mock && g_resolver.last_va == kStart &&
               g_resolver.last_bytes == kMeasuredWidth);
        EXPECT(xs::last_srv_at_va_outcome() == O::created && xs::srv_at_va_created_calls() == 1 &&
               xs::srv_at_va_refused_calls() == 0);
    }
    // Size-independent: a 512-byte view 256 bytes into the buffer, into a plain heap's real slot (unchanged handle).
    {
        const Slot70Args args = slot70_args(kStart + 0x100, 0x200);
        call70(device, args.resource, args.view, plain0 + 3 * kInc);
        EXPECT(mock->calls[18] == 2 && mock->args[18][0] == word(buffer) && mock->args[18][2] == plain0 + 3 * kInc);
        EXPECT(mock->srv.Buffer.FirstElement == 0x40 && mock->srv.Buffer.NumElements == 0x80 &&
               mock->srv.Buffer.Flags == D3D12_BUFFER_SRV_FLAG_RAW && buffer->refs == 1);
        // The last 16 bytes of the buffer exactly.
        const Slot70Args tail = slot70_args(kStart + kMeasuredWidth - 16, 16);
        call70(device, tail.resource, tail.view, kCustom + 7 * kInc);
        EXPECT(mock->calls[18] == 3 && mock->srv.Buffer.FirstElement == (kMeasuredWidth - 16) / 4 &&
               mock->srv.Buffer.NumElements == 4 && mock->args[18][2] == real0 + 7 * kInc && buffer->refs == 1);
    }
    // The PC slot 18 itself is unchanged: forwarded with the caller's own description and the translated handle.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC pc{};
        pc.Format = DXGI_FORMAT_R32_FLOAT;
        pc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        pc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        pc.Buffer.FirstElement = 5;
        pc.Buffer.NumElements = 7;
        raw(device, 18, word(buffer), word(&pc), kCustom + kInc);
        EXPECT(mock->calls[18] == 4 && mock->args[18][0] == word(buffer) && mock->args[18][1] == word(&pc) &&
               mock->args[18][2] == real0 + kInc && mock->srv.Format == DXGI_FORMAT_R32_FLOAT &&
               mock->srv.Buffer.FirstElement == 5 && mock->srv.Buffer.NumElements == 7);
        EXPECT(mock->calls[70] == 0 && xs::srv_at_va_created_calls() == 3);
    }

    // Every refusal: no descriptor (slot 18 not called), PC slot 70 not called, the descriptions unchanged, no
    // reference left behind, the refusal counted with its reason.
    const std::uint64_t good_dest = kCustom + kInc;
    const auto refused = [&](const Slot70Args& args, std::uint64_t destination, O want) {
        Slot70Args call = args;
        const unsigned srv_calls = mock->calls[18] + other_mock->calls[18];
        const long refs[] = {buffer->refs, denied->refs, foreign->refs};
        const std::uint64_t before = xs::srv_at_va_refused_calls();
        const std::uint64_t created = xs::srv_at_va_created_calls();
        call70(device, call.resource, call.view, destination);
        const bool ok = mock->calls[18] + other_mock->calls[18] == srv_calls && mock->calls[70] == 0 &&
                        other_mock->calls[70] == 0 && buffer->refs == refs[0] && denied->refs == refs[1] &&
                        foreign->refs == refs[2] && std::memcmp(&call, &args, sizeof(args)) == 0 &&
                        xs::last_srv_at_va_outcome() == want && xs::srv_at_va_refused_calls() == before + 1 &&
                        xs::srv_at_va_created_calls() == created;
        if (!ok)
            std::fprintf(stderr, "%s: slot 70 refusal: outcome %d (want %d), slot 18 calls %u -> %u\n", g_label,
                         static_cast<int>(xs::last_srv_at_va_outcome()), static_cast<int>(want), srv_calls,
                         mock->calls[18] + other_mock->calls[18]);
        return ok;
    };
    // The base every refusal below changes in one way only; it succeeds as it is.
    const Slot70Args good = slot70_args(kStart, 0x1000);
    call70(device, good.resource, good.view, good_dest);
    EXPECT(mock->calls[18] == 5 && mock->args[18][2] == real0 + kInc && buffer->refs == 1 &&
           xs::last_srv_at_va_outcome() == O::created);

    // Every field of the shape (padding is never examined; see slot70_args).
    {
        const auto mutate = [&](bool in_view, std::size_t offset, std::size_t bytes, std::uint64_t value) {
            Slot70Args a = good;
            std::uint8_t* at = (in_view ? a.view : a.resource) + offset;
            std::memcpy(at, &value, bytes);
            return a;
        };
        struct Field {
            bool view;
            std::size_t offset, bytes;
            std::uint64_t value;
        };
        constexpr std::size_t kBuf = offsetof(D3D12_SHADER_RESOURCE_VIEW_DESC, Buffer);
        const Field fields[] = {
            {false, offsetof(D3D12_RESOURCE_DESC, Dimension), 4, D3D12_RESOURCE_DIMENSION_TEXTURE2D},
            {false, offsetof(D3D12_RESOURCE_DESC, Alignment), 8, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT},
            {false, offsetof(D3D12_RESOURCE_DESC, Height), 4, 2},
            {false, offsetof(D3D12_RESOURCE_DESC, DepthOrArraySize), 2, 2},
            {false, offsetof(D3D12_RESOURCE_DESC, MipLevels), 2, 0},
            {false, offsetof(D3D12_RESOURCE_DESC, Format), 4, DXGI_FORMAT_R32_TYPELESS},
            {false, offsetof(D3D12_RESOURCE_DESC, SampleDesc), 4, 2},
            {false, offsetof(D3D12_RESOURCE_DESC, SampleDesc) + 4, 4, 1},
            {false, offsetof(D3D12_RESOURCE_DESC, Layout), 4, D3D12_TEXTURE_LAYOUT_UNKNOWN},
            {false, offsetof(D3D12_RESOURCE_DESC, Flags), 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},
            {false, offsetof(D3D12_RESOURCE_DESC, Flags), 4, 0x20004},
            {true, offsetof(D3D12_SHADER_RESOURCE_VIEW_DESC, Format), 4, DXGI_FORMAT_R32_UINT},
            {true, offsetof(D3D12_SHADER_RESOURCE_VIEW_DESC, ViewDimension), 4, D3D12_SRV_DIMENSION_TEXTURE2D},
            {true, offsetof(D3D12_SHADER_RESOURCE_VIEW_DESC, Shader4ComponentMapping), 4,
             D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(2, 1, 0, 3)},
            {true, kBuf + offsetof(D3D12_BUFFER_SRV, FirstElement), 8, 1},          // no established offset meaning
            {true, kBuf + offsetof(D3D12_BUFFER_SRV, NumElements), 4, 0x1000 / 4 - 1},  // not Width / 4
            {true, kBuf + offsetof(D3D12_BUFFER_SRV, NumElements), 4, 0x1000 / 4 + 1},
            {true, kBuf + offsetof(D3D12_BUFFER_SRV, StructureByteStride), 4, 4},
            {true, kBuf + offsetof(D3D12_BUFFER_SRV, Flags), 4, D3D12_BUFFER_SRV_FLAG_NONE},
            {true, kBuf + offsetof(D3D12_BUFFER_SRV, Flags), 4, 3},
        };
        for (const Field& f : fields) EXPECT(refused(mutate(f.view, f.offset, f.bytes, f.value), good_dest, O::unsupported_shape));
    }
    // Widths and VAs: zero, unaligned, too wide for a raw V#, overflowing, unaligned VA, null VA.
    EXPECT(refused(slot70_args(kStart, 0), good_dest, O::invalid_range));
    EXPECT(refused(slot70_args(kStart, 6), good_dest, O::invalid_range));
    EXPECT(refused(slot70_args(kStart, 0x100000000ull), good_dest, O::invalid_range));
    EXPECT(refused(slot70_args(0xFFFFFFFFFFFFFF00ull, 0x200), good_dest, O::invalid_range));
    EXPECT(refused(slot70_args(kStart + 4, 0x100), good_dest, O::invalid_range));
    EXPECT(refused(slot70_args(kStart + 8, 0x100), good_dest, O::invalid_range));
    EXPECT(refused(slot70_args(0, 0x100), good_dest, O::invalid_range));

    // Destinations: null, off a descriptor boundary, past the title range, a native handle the proxy never saw, a
    // sampler heap's slot, another device's heap, a released heap's slot.
    EXPECT(refused(good, 0, O::invalid_destination));
    EXPECT(refused(good, kCustom + kInc / 2, O::invalid_destination));
    EXPECT(refused(good, kCustom + 8 * kInc, O::invalid_destination));
    EXPECT(refused(good, 0x4242000, O::invalid_destination));
    EXPECT(refused(good, real_heap_of(sampler)->heap_cpu, O::invalid_destination));
    EXPECT(refused(good, real_heap_of(foreign_heap)->heap_cpu, O::invalid_destination));
    EXPECT(refused(good, gone_cpu, O::invalid_destination));

    // VAs: none of this device's buffers, a range that runs past the buffer (never cut to fit), a buffer of another
    // device (whose own proxy does find it), a released buffer, a buffer that denies shader-resource access.
    EXPECT(refused(slot70_args(kStart + kMeasuredWidth, 0x100), good_dest, O::unknown_va));
    EXPECT(refused(slot70_args(kStart + kMeasuredWidth - 0x100, 0x200), good_dest, O::unknown_va));
    EXPECT(refused(slot70_args(kStart - 0x100, 0x200), good_dest, O::unknown_va));
    EXPECT(refused(slot70_args(kForeignStart, 0x100), good_dest, O::unknown_va));
    {
        void* foreign_dest = reinterpret_cast<void*>(real_heap_of(foreign_heap)->heap_cpu);
        const Slot70Args theirs = slot70_args(kForeignStart, 0x100);
        call70(other, theirs.resource, theirs.view, reinterpret_cast<std::uint64_t>(foreign_dest));
        EXPECT(other_mock->calls[18] == 1 && other_mock->args[18][0] == word(foreign) && foreign->refs == 1 &&
               mock->calls[70] == 0 && other_mock->calls[70] == 0);
    }
    buffer->refs = 0;  // as a buffer whose last Release is in progress: the resolver cannot take a reference
    EXPECT(refused(good, good_dest, O::unknown_va));
    buffer->refs = 1;
    EXPECT(refused(slot70_args(kDeniedStart, 0x100), good_dest, O::not_shader_readable));

    // A broken or absent resolver.
    g_resolver.mode = ResolverMode::fail;
    EXPECT(refused(good, good_dest, O::resolver_failed));
    g_resolver.mode = ResolverMode::short_range;
    EXPECT(refused(good, good_dest, O::resolver_failed));          // its reference is released all the same
    g_resolver.mode = ResolverMode::ref_with_not_found;
    EXPECT(refused(good, good_dest, O::unknown_va));
    g_resolver.mode = ResolverMode::normal;
    EXPECT(xs::set_device_buffer_at_va_resolver(device, nullptr));
    EXPECT(refused(good, good_dest, O::no_resolver));
    EXPECT(xs::set_device_buffer_at_va_resolver(device, &fake_buffer_resolver));

    // Descriptions that cannot be read: null, a no-access page, a guard page, and both kinds running off the end of a
    // committed page into a reserved one. Nothing is copied from them and nothing is written.
    {
        const auto unreadable = [&](std::uint64_t resource_word, std::uint64_t view_word) {
            const unsigned srv_calls = mock->calls[18];
            const std::uint64_t before = xs::srv_at_va_refused_calls();
            raw(device, 70, resource_word, view_word, good_dest);
            return mock->calls[18] == srv_calls && mock->calls[70] == 0 &&
                   xs::last_srv_at_va_outcome() == O::unreadable && xs::srv_at_va_refused_calls() == before + 1 &&
                   buffer->refs == 1;
        };
        Slot70Args args = good;
        EXPECT(unreadable(0, word(args.view)));
        EXPECT(unreadable(word(args.resource), 0));
        auto* const pages = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x3000, MEM_RESERVE, PAGE_NOACCESS));
        EXPECT(pages != nullptr);
        EXPECT(VirtualAlloc(pages, 0x1000, MEM_COMMIT, PAGE_READWRITE) == pages);
        EXPECT(VirtualAlloc(pages + 0x2000, 0x1000, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD) == pages + 0x2000);
        // Straddling into the reserved page at +0x1000: the view needs 0x30 bytes, the resource 0x38.
        std::uint8_t* const view_end = pages + 0x1000 - 0x20;
        std::memcpy(view_end, args.view, 0x20);
        std::uint8_t* const resource_end = pages + 0x1000 - 0x30;
        EXPECT(unreadable(word(args.resource), word(view_end)));
        std::memcpy(resource_end, args.resource, 0x30);
        EXPECT(unreadable(word(resource_end), word(args.view)));
        EXPECT(unreadable(word(pages + 0x1000), word(args.view)));  // reserved, not committed
        EXPECT(unreadable(word(args.resource), word(pages + 0x2000)));  // guard page: not touched
        DWORD old = 0;
        EXPECT(VirtualProtect(pages, 0x1000, PAGE_NOACCESS, &old) != FALSE);
        EXPECT(unreadable(word(pages), word(args.view)));
        // The guard page is still armed: the proxy never touched it.
        MEMORY_BASIC_INFORMATION info{};
        EXPECT(VirtualQuery(pages + 0x2000, &info, sizeof(info)) == sizeof(info) && (info.Protect & PAGE_GUARD));
        EXPECT(VirtualFree(pages, 0, MEM_RELEASE) != FALSE);
    }

    // A base PC device (44 slots, no PC slot 70): still the console view, never a refusal past the PC table.
    {
        Mock* base = nullptr;
        void* base_device = make_device(base, 0, false);
        EXPECT(base_device);
        call70(base_device, good.resource, good.view, good_dest);  // no resolver, no heap of its own
        EXPECT(base->calls[70] == 0 && base->calls[18] == 0 && xs::last_srv_at_va_outcome() == O::invalid_destination);
        EXPECT(release(base_device) == 0 && base->refs == 0);
    }

    EXPECT(xs::srv_at_va_created_calls() == 5 && mock->calls[70] == 0);
    EXPECT(release(heap) == 0 && release(plain) == 0 && release(sampler) == 0 && release(foreign_heap) == 0);
    EXPECT(buffer->refs == 1 && denied->refs == 1 && foreign->refs == 1);
    EXPECT(release(device) == 0 && mock->refs == 0);
    EXPECT(release(other) == 0 && other_mock->refs == 0);
    g_resolver = {};
    std::printf("%s: device slot 70 writes a raw view of the live buffer at the GPU VA through PC slot 18 with the "
                "translated handle (measured 174 MiB shape and smaller ones), never PC slot 70; every shape, range, "
                "destination, VA, resolver and unreadable-description refusal writes nothing and leaves no reference\n",
                g_label);
    return 0;
}

// ---- 11. Device4/Device8 resource routes: console slots 92-95, 108-110 and 112 (--resource-routes) ----
//
// The slot-to-method mapping is public reference only (OpenScarlett and two upstream PRs), not a measured retail call.
// The mock device answers ID3D12Device4/ID3D12Device8 with separate RouteAlias objects. Each route must use the
// returned pointer and release it (reference counts balanced), and call the PC slot of the method exactly once with the
// caller's pointers and bytes (padding, DESC1 mip region, unknown flag bits) and values. A word after the method's
// own carries a canary that must never arrive, and every output sits between canaries. A device without the interface
// must get the explicit refusal for each shape. No GPU, no D3D12 runtime.

static_assert(sizeof(D3D12_RESOURCE_DESC) == 56 && sizeof(D3D12_RESOURCE_DESC1) == 64,
              "the description strides the routes forward unchanged");
constexpr std::uint64_t kTailCanary = 0xC0DEC0DE00000000ull;  // + word index, past the method's own words
constexpr std::uint64_t kFootprintTotal = 0x0000000BADC0FFEEull;

RouteAlias g_aliases[8];
unsigned g_aliases_used = 0;

template <unsigned Slot>
std::uint64_t WINAPI alias_slot(RouteAlias* self, std::uint64_t, std::uint64_t a2, std::uint64_t, std::uint64_t) {
    (void)a2;  // QueryInterface's output; the other slots ignore their words
    if constexpr (Slot == 1) {
        return static_cast<std::uint64_t>(++self->refs);
    } else if constexpr (Slot == 2) {
        return static_cast<std::uint64_t>(--self->refs);
    } else {
        ++self->calls[Slot];  // a stray call: a route reached a PC slot that is not its method
        ++self->total_calls;
        if constexpr (Slot == 0) {
            if (a2) *reinterpret_cast<void**>(a2) = nullptr;
            return static_cast<std::uint32_t>(E_NOINTERFACE);
        }
        return static_cast<std::uint32_t>(E_FAIL);
    }
}

void alias_record(RouteAlias* self, unsigned pc_slot, const std::uint64_t* words, unsigned count, std::uint64_t descs,
                  std::size_t stride, std::uint32_t desc_count) {
    ++self->calls[pc_slot];
    ++self->total_calls;
    std::memset(self->words, 0, sizeof(self->words));
    std::memcpy(self->words, words, count * sizeof(std::uint64_t));
    std::memset(self->desc_bytes, 0, sizeof(self->desc_bytes));
    for (std::uint32_t i = 0; descs && i < desc_count && i < 2; ++i)
        std::memcpy(self->desc_bytes[i], reinterpret_cast<const std::uint8_t*>(descs) + i * stride, stride);
}

// S_FALSE without an output (the PC capability check); `result` otherwise, with a new object on success.
std::uint64_t alias_create(RouteAlias* self, std::uint64_t out_word) {
    auto** out = reinterpret_cast<void**>(out_word);
    if (!out) return static_cast<std::uint32_t>(S_FALSE);
    *out = nullptr;
    if (FAILED(self->result)) return static_cast<std::uint32_t>(self->result);
    Mock* child = new_mock(Kind::resource);
    child->refs = 1;
    self->made = child;
    *out = child;
    return static_cast<std::uint32_t>(S_OK);
}

// PC slot 53, ID3D12Device4::CreateCommittedResource1(heap properties, heap flags, desc, state, clear, session, riid, out)
std::uint64_t WINAPI alias_committed1(RouteAlias* self, std::uint64_t heap, std::uint64_t flags, std::uint64_t desc,
                                      std::uint64_t state, std::uint64_t clear, std::uint64_t session,
                                      std::uint64_t riid, std::uint64_t out) {
    const std::uint64_t w[] = {heap, flags, desc, state, clear, session, riid, out};
    alias_record(self, 53, w, 8, desc, sizeof(D3D12_RESOURCE_DESC), 1);
    return alias_create(self, out);
}

// PC slot 54, ID3D12Device4::CreateHeap1(heap desc, session, riid, out)
std::uint64_t WINAPI alias_heap1(RouteAlias* self, std::uint64_t desc, std::uint64_t session, std::uint64_t riid,
                                 std::uint64_t out) {
    const std::uint64_t w[] = {desc, session, riid, out};
    alias_record(self, 54, w, 4, desc, sizeof(D3D12_HEAP_DESC), 1);
    return alias_create(self, out);
}

// PC slot 55, ID3D12Device4::CreateReservedResource1(desc, state, clear, session, riid, out)
std::uint64_t WINAPI alias_reserved1(RouteAlias* self, std::uint64_t desc, std::uint64_t state, std::uint64_t clear,
                                     std::uint64_t session, std::uint64_t riid, std::uint64_t out) {
    const std::uint64_t w[] = {desc, state, clear, session, riid, out};
    alias_record(self, 55, w, 6, desc, sizeof(D3D12_RESOURCE_DESC), 1);
    return alias_create(self, out);
}

// PC slot 69, ID3D12Device8::CreateCommittedResource2(heap properties, heap flags, DESC1, state, clear, session, riid,
// out)
std::uint64_t WINAPI alias_committed2(RouteAlias* self, std::uint64_t heap, std::uint64_t flags, std::uint64_t desc,
                                      std::uint64_t state, std::uint64_t clear, std::uint64_t session,
                                      std::uint64_t riid, std::uint64_t out) {
    const std::uint64_t w[] = {heap, flags, desc, state, clear, session, riid, out};
    alias_record(self, 69, w, 8, desc, sizeof(D3D12_RESOURCE_DESC1), 1);
    return alias_create(self, out);
}

// PC slot 70, ID3D12Device8::CreatePlacedResource1(heap, offset, DESC1, state, clear, riid, out)
std::uint64_t WINAPI alias_placed1(RouteAlias* self, std::uint64_t heap, std::uint64_t offset, std::uint64_t desc,
                                   std::uint64_t state, std::uint64_t clear, std::uint64_t riid, std::uint64_t out) {
    const std::uint64_t w[] = {heap, offset, desc, state, clear, riid, out};
    alias_record(self, 70, w, 7, desc, sizeof(D3D12_RESOURCE_DESC1), 1);
    return alias_create(self, out);
}

// PC slots 56 and 68 as the MSVC C++ methods are called: `this`, the hidden return buffer, mask, count, the
// descriptions and the per-resource array (both on the stack). The buffer's address comes back in RAX.
template <unsigned PcSlot, class Desc>
std::uint64_t WINAPI alias_allocation(RouteAlias* self, std::uint64_t ret, std::uint64_t mask, std::uint64_t count,
                                      std::uint64_t descs, std::uint64_t infos) {
    const std::uint64_t w[] = {ret, mask, count, descs, infos};
    const auto n = static_cast<std::uint32_t>(count);
    alias_record(self, PcSlot, w, 5, descs, sizeof(Desc), n);
    auto* each = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO1*>(infos);
    for (std::uint32_t i = 0; each && i < n && i < 2; ++i) each[i] = self->per[i];
    if (ret) std::memcpy(reinterpret_cast<void*>(ret), &self->answer, sizeof(self->answer));
    return ret;
}

D3D12_PLACED_SUBRESOURCE_FOOTPRINT expected_footprint(std::uint64_t base, std::uint32_t i) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT f{};
    f.Offset = base + i * 0x10000ull;
    f.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 64u >> i, 40u >> i, 1, 512};
    return f;
}

bool same_footprint(const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& a, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& b) {
    return a.Offset == b.Offset && a.Footprint.Format == b.Footprint.Format && a.Footprint.Width == b.Footprint.Width &&
           a.Footprint.Height == b.Footprint.Height && a.Footprint.Depth == b.Footprint.Depth &&
           a.Footprint.RowPitch == b.Footprint.RowPitch;
}

// PC slot 72, ID3D12Device8::GetCopyableFootprints1(DESC1, first, count, base offset, layouts, rows, row sizes, total)
std::uint64_t WINAPI alias_footprints1(RouteAlias* self, std::uint64_t desc, std::uint64_t first, std::uint64_t count,
                                       std::uint64_t base, std::uint64_t layouts, std::uint64_t rows,
                                       std::uint64_t row_sizes, std::uint64_t total) {
    const std::uint64_t w[] = {desc, first, count, base, layouts, rows, row_sizes, total};
    alias_record(self, 72, w, 8, desc, sizeof(D3D12_RESOURCE_DESC1), 1);
    const auto n = static_cast<std::uint32_t>(count);
    for (std::uint32_t i = 0; i < n && i < 2; ++i) {
        if (layouts) reinterpret_cast<D3D12_PLACED_SUBRESOURCE_FOOTPRINT*>(layouts)[i] = expected_footprint(base, i);
        if (rows) reinterpret_cast<UINT*>(rows)[i] = 40u + i;
        if (row_sizes) reinterpret_cast<UINT64*>(row_sizes)[i] = 0x300ull + i;
    }
    if (total) *reinterpret_cast<UINT64*>(total) = kFootprintTotal;
    return 0;
}

template <std::size_t... I>
void fill_alias_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&alias_slot<static_cast<unsigned>(I)>)), ...);
}

void** alias_vtable() {
    static void* table[kMockSlots];
    static const bool filled = [] {
        fill_alias_vtable(table, std::make_index_sequence<kMockSlots>{});
        table[53] = reinterpret_cast<void*>(&alias_committed1);
        table[54] = reinterpret_cast<void*>(&alias_heap1);
        table[55] = reinterpret_cast<void*>(&alias_reserved1);
        table[56] = reinterpret_cast<void*>(&alias_allocation<56, D3D12_RESOURCE_DESC>);
        table[68] = reinterpret_cast<void*>(&alias_allocation<68, D3D12_RESOURCE_DESC1>);
        table[69] = reinterpret_cast<void*>(&alias_committed2);
        table[70] = reinterpret_cast<void*>(&alias_placed1);
        table[72] = reinterpret_cast<void*>(&alias_footprints1);
        return true;
    }();
    (void)filled;
    return table;
}

RouteAlias* new_alias() {
    if (g_aliases_used == std::size(g_aliases)) {
        std::fprintf(stderr, "%s: alias pool exhausted\n", g_label);
        std::exit(1);
    }
    RouteAlias* a = &g_aliases[g_aliases_used++];
    std::memset(a, 0, sizeof(*a));
    a->vtable = alias_vtable();
    a->refs = 1;
    return a;
}

// A device proxy over a mock at `level` whose Device4/Device8 queries (when that level answers them) return aliases.
void* make_route_device(int level, Mock*& mock, RouteAlias*& device4, RouteAlias*& device8) {
    mock = new_mock(Kind::device);
    mock->device_level = level;
    mock->refs = 1;
    device4 = level >= 4 ? new_alias() : nullptr;
    device8 = level >= 8 ? new_alias() : nullptr;
    mock->device4_alias = device4;
    mock->device8_alias = device8;
    void* device = nullptr;
    if (FAILED(xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(mock), __uuidof(ID3D12Device), &device)))
        return nullptr;
    return device;
}

// A raw call through the proxy's table: `rcx` first (the proxy, or the console's return buffer), then 14 words.
std::uint64_t call_slot(const void* device, unsigned slot, const void* rcx, const std::uint64_t (&w)[14]) {
    void* const* vtable = *reinterpret_cast<void* const* const*>(device);
    return reinterpret_cast<SlotFn>(vtable[slot])(const_cast<void*>(rcx), w[0], w[1], w[2], w[3], w[4], w[5], w[6],
                                                  w[7], w[8], w[9], w[10], w[11], w[12], w[13]);
}

// The method's words first, canaries after them.
void set_words(std::uint64_t (&w)[14], const std::uint64_t* args, unsigned count) {
    for (unsigned i = 0; i < 14; ++i) w[i] = i < count ? args[i] : kTailCanary + i;
}

bool canaries_unseen(const RouteAlias* alias) {
    for (const std::uint64_t value : alias->words)
        if ((value & 0xFFFFFFFF00000000ull) == kTailCanary) return false;
    return true;
}

struct OutSlot {
    std::uint64_t before;
    void* value;
    std::uint64_t after;
};
constexpr std::uint64_t kOutBefore = 0x0123456789ABCDEFull, kOutAfter = 0xFEDCBA9876543210ull;

struct CreateRoute {
    unsigned slot, pc_slot, words;  // console slot, PC slot on the alias, argument words after `this`
    unsigned riid_index, out_index;
    std::uint32_t narrow;           // bit i: word i is 32-bit (poisoned above); only its low half must arrive
    const void* desc;               // the description whose bytes must arrive and stay unchanged (or null)
    std::size_t desc_bytes;
};

bool same_words(const RouteAlias* alias, const std::uint64_t (&w)[14], const CreateRoute& r) {
    for (unsigned i = 0; i < r.words; ++i) {
        const std::uint64_t mask = (r.narrow >> i) & 1 ? 0xFFFFFFFFull : ~0ull;
        if ((alias->words[i] & mask) != (w[i] & mask)) return false;
    }
    return canaries_unseen(alias);
}

// One HRESULT route on a device that has the interface: S_OK with an object, the device's E_INVALIDARG and
// E_OUTOFMEMORY with a null output, a null output (S_FALSE from the device), and a null IID (E_POINTER, no call).
int check_create_route(void* device, Mock* mock, RouteAlias* alias, RouteAlias* other, const CreateRoute& r,
                       const std::uint64_t* args) {
    std::uint8_t desc_before[sizeof(D3D12_RESOURCE_DESC1)] = {};
    if (r.desc) std::memcpy(desc_before, r.desc, r.desc_bytes);
    const long alias_refs = alias->refs, host_refs = mock->refs;
    const long other_refs = other ? other->refs : 0;
    const unsigned other_calls = other ? other->total_calls : 0;
    const unsigned long proxy_refs = xs::device_proxy_ref_count(device);
    EXPECT(xs::device_slot_overridden(r.slot));
    for (const HRESULT expected : {S_OK, E_INVALIDARG, E_OUTOFMEMORY}) {
        alias->result = expected;
        alias->made = nullptr;
        OutSlot out{kOutBefore, reinterpret_cast<void*>(1), kOutAfter};
        std::uint64_t w[14];
        set_words(w, args, r.words);
        w[r.out_index] = word(&out.value);
        const unsigned calls = alias->calls[r.pc_slot], total = alias->total_calls;
        const std::uint64_t hits = xs::device_slot_hits(r.slot);
        EXPECT(hr_of(call_slot(device, r.slot, device, w)) == expected);
        EXPECT(alias->calls[r.pc_slot] == calls + 1 && alias->total_calls == total + 1);
        EXPECT(same_words(alias, w, r));
        if (r.desc)
            EXPECT(std::memcmp(alias->desc_bytes[0], desc_before, r.desc_bytes) == 0 &&
                   std::memcmp(r.desc, desc_before, r.desc_bytes) == 0);
        EXPECT(out.before == kOutBefore && out.after == kOutAfter);
        if (expected == S_OK) {
            EXPECT(alias->made && out.value == alias->made);
            EXPECT(release(out.value) == 0);
        } else {
            EXPECT(out.value == nullptr);
        }
        EXPECT(xs::device_slot_hits(r.slot) == hits + 1);
        EXPECT(alias->refs == alias_refs && mock->refs == host_refs && xs::device_proxy_ref_count(device) == proxy_refs);
    }
    alias->result = S_OK;
    {  // The PC capability check: a null output reaches the device, whose S_FALSE comes back.
        std::uint64_t w[14];
        set_words(w, args, r.words);
        w[r.out_index] = 0;
        const unsigned calls = alias->calls[r.pc_slot];
        EXPECT(hr_of(call_slot(device, r.slot, device, w)) == S_FALSE);
        EXPECT(alias->calls[r.pc_slot] == calls + 1 && same_words(alias, w, r) && alias->words[r.out_index] == 0);
    }
    {  // A null IID cannot become a REFIID: E_POINTER, a null output and no query or call.
        OutSlot out{kOutBefore, reinterpret_cast<void*>(1), kOutAfter};
        std::uint64_t w[14];
        set_words(w, args, r.words);
        w[r.out_index] = word(&out.value);
        w[r.riid_index] = 0;
        const unsigned total = alias->total_calls;
        const std::uint64_t hits = xs::device_slot_hits(r.slot);
        EXPECT(hr_of(call_slot(device, r.slot, device, w)) == E_POINTER);
        EXPECT(out.value == nullptr && out.before == kOutBefore && out.after == kOutAfter);
        EXPECT(alias->total_calls == total && xs::device_slot_hits(r.slot) == hits + 1);
    }
    // Never the other interface object, the base device's PC slot or the console slot index on the device.
    EXPECT(!other || (other->total_calls == other_calls && other->refs == other_refs));
    EXPECT(mock->calls[r.pc_slot] == 0 && mock->calls[r.slot] == 0);
    EXPECT(alias->refs == alias_refs && mock->refs == host_refs);
    return 0;
}

// An HRESULT route on a device without its interface: E_NOTIMPL, a null output, nothing reaches any object.
int expect_create_unsupported(void* device, Mock* mock, RouteAlias* present, const CreateRoute& r,
                              const std::uint64_t* args) {
    const unsigned present_calls = present ? present->total_calls : 0;
    const long present_refs = present ? present->refs : 0;
    const long host_refs = mock->refs;
    for (const bool with_out : {true, false}) {
        OutSlot out{kOutBefore, reinterpret_cast<void*>(1), kOutAfter};
        std::uint64_t w[14];
        set_words(w, args, r.words);
        w[r.out_index] = with_out ? word(&out.value) : 0;
        const std::uint64_t hits = xs::device_slot_hits(r.slot);
        EXPECT(hr_of(call_slot(device, r.slot, device, w)) == E_NOTIMPL);
        EXPECT(out.before == kOutBefore && out.after == kOutAfter);
        EXPECT(with_out ? out.value == nullptr : out.value == reinterpret_cast<void*>(1));
        EXPECT(xs::device_slot_hits(r.slot) == hits + 1);
    }
    EXPECT(!present || (present->total_calls == present_calls && present->refs == present_refs));
    EXPECT(mock->calls[r.pc_slot] == 0 && mock->calls[r.slot] == 0 && mock->refs == host_refs);
    return 0;
}

struct AllocationBuffer {
    std::uint8_t before[16];
    D3D12_RESOURCE_ALLOCATION_INFO value;
    std::uint8_t after[64];
};

bool bytes_are(const void* p, std::size_t n, std::uint8_t value) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i)
        if (b[i] != value) return false;
    return true;
}

// Slot 95 or 108 in the console form (`console`: RCX buffer, RDX device) or the PC form (RCX device, RDX buffer),
// with two descriptions, mask/count poisoned in their upper halves, and a three-entry per-resource array whose last
// entry must stay untouched. `buffer` may be null. Returns RAX.
std::uint64_t call_allocation(void* device, unsigned slot, bool console, AllocationBuffer* buffer, const void* descs,
                              D3D12_RESOURCE_ALLOCATION_INFO1* infos) {
    void* const value = buffer ? &buffer->value : nullptr;
    const std::uint64_t args[] = {console ? word(device) : word(value), 0xDEADBEEF00000003ull, 0xFEEDFACE00000002ull,
                                  word(descs), word(infos)};
    std::uint64_t w[14];
    set_words(w, args, 5);
    return call_slot(device, slot, console ? value : device, w);
}

int check_allocation_route(void* device, Mock* mock, RouteAlias* alias, RouteAlias* other, unsigned slot,
                           unsigned pc_slot, const void* descs, std::size_t stride) {
    std::uint8_t descs_before[2 * sizeof(D3D12_RESOURCE_DESC1)];
    std::memcpy(descs_before, descs, 2 * stride);
    const long alias_refs = alias->refs, host_refs = mock->refs;
    const long other_refs = other ? other->refs : 0;
    const unsigned other_calls = other ? other->total_calls : 0;
    EXPECT(xs::device_slot_overridden(slot));
    alias->per[0] = {0x0000000000000000ull, 0x0000000000010000ull, 0x0000000000056000ull};
    alias->per[1] = {0x0000000000060000ull, 0x0000000000000200ull, 0x0000000000001234ull};
    const D3D12_RESOURCE_ALLOCATION_INFO answers[] = {{0x0000000123456000ull, 0x0000000000010000ull},
                                                      {UINT64_MAX, 0x0000000000000200ull}};
    for (const bool console : {true, false}) {
        for (const D3D12_RESOURCE_ALLOCATION_INFO& answer : answers) {
            alias->answer = answer;
            AllocationBuffer buffer;
            std::memset(&buffer, 0xA5, sizeof(buffer));
            D3D12_RESOURCE_ALLOCATION_INFO1 infos[3];
            std::memset(infos, 0x77, sizeof(infos));
            const unsigned calls = alias->calls[pc_slot], total = alias->total_calls;
            const std::uint64_t hits = xs::device_slot_hits(slot);
            EXPECT(call_allocation(device, slot, console, &buffer, descs, infos) == word(&buffer.value));
            EXPECT(alias->calls[pc_slot] == calls + 1 && alias->total_calls == total + 1);
            EXPECT(alias->words[0] != 0 && static_cast<std::uint32_t>(alias->words[1]) == 3 &&
                   static_cast<std::uint32_t>(alias->words[2]) == 2 && alias->words[3] == word(descs) &&
                   alias->words[4] == word(infos) && canaries_unseen(alias));
            for (unsigned i = 0; i < 2; ++i)
                EXPECT(std::memcmp(alias->desc_bytes[i], descs_before + i * stride, stride) == 0);
            EXPECT(std::memcmp(descs, descs_before, 2 * stride) == 0);
            // Exactly the 16 bytes, the device's answer (UINT64_MAX included) unchanged.
            EXPECT(buffer.value.SizeInBytes == answer.SizeInBytes && buffer.value.Alignment == answer.Alignment);
            EXPECT(bytes_are(buffer.before, sizeof(buffer.before), 0xA5) &&
                   bytes_are(buffer.after, sizeof(buffer.after), 0xA5));
            EXPECT(std::memcmp(&infos[0], &alias->per[0], sizeof(infos[0])) == 0 &&
                   std::memcmp(&infos[1], &alias->per[1], sizeof(infos[1])) == 0 &&
                   bytes_are(&infos[2], sizeof(infos[2]), 0x77));
            EXPECT(xs::device_slot_hits(slot) == hits + 1);
            EXPECT(alias->refs == alias_refs && mock->refs == host_refs && xs::is_device_proxy(device));
        }
        // A null return buffer: refused, nothing written, no device call, RAX null.
        D3D12_RESOURCE_ALLOCATION_INFO1 infos[3];
        std::memset(infos, 0x77, sizeof(infos));
        const unsigned total = alias->total_calls;
        const std::uint64_t hits = xs::device_slot_hits(slot);
        EXPECT(call_allocation(device, slot, console, nullptr, descs, infos) == 0);
        EXPECT(alias->total_calls == total && bytes_are(infos, sizeof(infos), 0x77));
        EXPECT(xs::device_slot_hits(slot) == hits + 1);
    }
    EXPECT(!other || (other->total_calls == other_calls && other->refs == other_refs));
    EXPECT(mock->calls[pc_slot] == 0 && mock->calls[slot] == 0);
    EXPECT(alias->refs == alias_refs && mock->refs == host_refs);
    return 0;
}

// Slot 95 or 108 on a device without the interface: {UINT64_MAX, UINT64_MAX} in both forms, the per-resource array
// unwritten, RAX the buffer.
int expect_allocation_unsupported(void* device, Mock* mock, RouteAlias* present, unsigned slot, const void* descs) {
    const unsigned present_calls = present ? present->total_calls : 0;
    for (const bool console : {true, false}) {
        AllocationBuffer buffer;
        std::memset(&buffer, 0xA5, sizeof(buffer));
        D3D12_RESOURCE_ALLOCATION_INFO1 infos[3];
        std::memset(infos, 0x77, sizeof(infos));
        const std::uint64_t hits = xs::device_slot_hits(slot);
        EXPECT(call_allocation(device, slot, console, &buffer, descs, infos) == word(&buffer.value));
        EXPECT(buffer.value.SizeInBytes == UINT64_MAX && buffer.value.Alignment == UINT64_MAX);
        EXPECT(bytes_are(buffer.before, sizeof(buffer.before), 0xA5) &&
               bytes_are(buffer.after, sizeof(buffer.after), 0xA5));
        EXPECT(bytes_are(infos, sizeof(infos), 0x77));
        EXPECT(xs::device_slot_hits(slot) == hits + 1);
    }
    EXPECT(!present || present->total_calls == present_calls);
    EXPECT(mock->calls[slot] == 0);
    return 0;
}

struct FootprintOutputs {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[3];
    UINT rows[3];
    UINT64 row_sizes[3];
    UINT64 total;
    UINT64 after;
};

void call_footprints(void* device, const D3D12_RESOURCE_DESC1* desc, std::uint64_t base, FootprintOutputs& o) {
    const std::uint64_t args[] = {word(desc), 0xAAAA000000000001ull, 0xBBBB000000000002ull, base,
                                  word(o.layouts), word(o.rows), word(o.row_sizes), word(&o.total)};
    std::uint64_t w[14];
    set_words(w, args, 8);
    (void)call_slot(device, 112, device, w);
}

int check_footprints_route(void* device, Mock* mock, RouteAlias* alias, RouteAlias* other,
                           const D3D12_RESOURCE_DESC1* desc) {
    EXPECT(xs::device_slot_overridden(112));
    std::uint8_t desc_before[sizeof(D3D12_RESOURCE_DESC1)];
    std::memcpy(desc_before, desc, sizeof(desc_before));
    const long alias_refs = alias->refs;
    const unsigned other_calls = other ? other->total_calls : 0;
    constexpr std::uint64_t kBase = 0x123456789ABCDEF0ull;  // a 64-bit offset: its upper half must arrive
    FootprintOutputs o;
    std::memset(&o, 0x66, sizeof(o));
    const unsigned calls = alias->calls[72], total = alias->total_calls;
    const std::uint64_t hits = xs::device_slot_hits(112);
    call_footprints(device, desc, kBase, o);
    EXPECT(alias->calls[72] == calls + 1 && alias->total_calls == total + 1);
    EXPECT(alias->words[0] == word(desc) && static_cast<std::uint32_t>(alias->words[1]) == 1 &&
           static_cast<std::uint32_t>(alias->words[2]) == 2 && alias->words[3] == kBase &&
           alias->words[4] == word(o.layouts) && alias->words[5] == word(o.rows) &&
           alias->words[6] == word(o.row_sizes) && alias->words[7] == word(&o.total) && canaries_unseen(alias));
    EXPECT(std::memcmp(alias->desc_bytes[0], desc_before, sizeof(desc_before)) == 0 &&
           std::memcmp(desc, desc_before, sizeof(desc_before)) == 0);
    for (std::uint32_t i = 0; i < 2; ++i)
        EXPECT(same_footprint(o.layouts[i], expected_footprint(kBase, i)) && o.rows[i] == 40u + i &&
               o.row_sizes[i] == 0x300ull + i);
    EXPECT(bytes_are(&o.layouts[2], sizeof(o.layouts[2]), 0x66) && bytes_are(&o.rows[2], sizeof(o.rows[2]), 0x66) &&
           bytes_are(&o.row_sizes[2], sizeof(o.row_sizes[2]), 0x66));
    EXPECT(o.total == kFootprintTotal && bytes_are(&o.after, sizeof(o.after), 0x66));
    EXPECT(xs::device_slot_hits(112) == hits + 1);
    EXPECT(alias->refs == alias_refs && (!other || other->total_calls == other_calls));
    EXPECT(mock->calls[72] == 0 && mock->calls[112] == 0);
    return 0;
}

// Slot 112 without ID3D12Device8: the PC failure footprint for the `count` entries, never a usable one.
int expect_footprints_unsupported(void* device, Mock* mock, RouteAlias* present, const D3D12_RESOURCE_DESC1* desc) {
    const unsigned present_calls = present ? present->total_calls : 0;
    FootprintOutputs o;
    std::memset(&o, 0x66, sizeof(o));
    const std::uint64_t hits = xs::device_slot_hits(112);
    call_footprints(device, desc, 0x123456789ABCDEF0ull, o);
    for (unsigned i = 0; i < 2; ++i)
        EXPECT(bytes_are(&o.layouts[i], sizeof(o.layouts[i]), 0xFF) && o.rows[i] == ~0u && o.row_sizes[i] == ~0ull);
    EXPECT(bytes_are(&o.layouts[2], sizeof(o.layouts[2]), 0x66) && bytes_are(&o.rows[2], sizeof(o.rows[2]), 0x66) &&
           bytes_are(&o.row_sizes[2], sizeof(o.row_sizes[2]), 0x66));
    EXPECT(o.total == UINT64_MAX && bytes_are(&o.after, sizeof(o.after), 0x66));
    EXPECT(xs::device_slot_hits(112) == hits + 1);
    EXPECT(!present || present->total_calls == present_calls);
    EXPECT(mock->calls[72] == 0 && mock->calls[112] == 0);
    return 0;
}

int check_resource_routes() {
    // Two descriptions of each kind: patterned padding, console-looking layout words and unknown flag bits, and a
    // nonzero sampler-feedback mip region in every DESC1. The proxy must hand all of it over untouched.
    D3D12_RESOURCE_DESC descs[2];
    std::memset(descs, 0x5C, sizeof(descs));
    descs[0].Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    descs[0].Alignment = 0x10000;
    descs[0].Width = 0x1234;
    descs[0].Height = 0x567;
    descs[0].DepthOrArraySize = 6;
    descs[0].MipLevels = 9;
    descs[0].Format = DXGI_FORMAT_BC7_UNORM;
    descs[0].SampleDesc = {1, 0};
    descs[0].Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);
    descs[0].Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x80020000u);
    descs[1].Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    descs[1].Alignment = 0;
    descs[1].Width = 0xABCDE;
    descs[1].Height = 1;
    descs[1].DepthOrArraySize = 1;
    descs[1].MipLevels = 1;
    descs[1].Format = DXGI_FORMAT_UNKNOWN;
    descs[1].SampleDesc = {1, 0};
    descs[1].Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x1);
    descs[1].Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x20004u);
    D3D12_RESOURCE_DESC1 descs1[2];
    std::memset(descs1, 0x3C, sizeof(descs1));  // padding at +4; DESC1 uses the original DESC tail padding
    descs1[0].Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    descs1[0].Alignment = 0x400000;
    descs1[0].Width = 0x800;
    descs1[0].Height = 0x400;
    descs1[0].DepthOrArraySize = 1;
    descs1[0].MipLevels = 12;
    descs1[0].Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    descs1[0].SampleDesc = {1, 0};
    descs1[0].Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);
    descs1[0].Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x00820004u);
    descs1[0].SamplerFeedbackMipRegion = {4, 8, 1};
    descs1[1] = descs1[0];
    descs1[1].Width = 0x200;
    descs1[1].Format = DXGI_FORMAT_BC1_UNORM;
    descs1[1].SamplerFeedbackMipRegion = {2, 2, 1};

    const D3D12_HEAP_PROPERTIES heap_properties{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                                D3D12_MEMORY_POOL_UNKNOWN, 0x3, 0x5};
    D3D12_HEAP_DESC heap_desc;
    std::memset(&heap_desc, 0x4E, sizeof(heap_desc));
    heap_desc.SizeInBytes = 0x400000;
    heap_desc.Alignment = 0x10000;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    // Opaque objects nobody may read: the session and the heap only have to arrive as the same pointers.
    std::uint64_t session_token = 0x5E55105E55105E55ull, heap_token = 0x4EA94EA94EA94EA9ull;
    const IID resource_iid = __uuidof(ID3D12Resource), heap_iid = __uuidof(ID3D12Heap);

    // Word values: 32-bit flags/states poisoned above, a 64-bit heap offset with its upper half set.
    const std::uint64_t args92[] = {word(&heap_properties), 0xDEAD000000000105ull, word(&descs[0]),
                                    0xBEEF0000FFFFFFFFull, word(&clear), word(&session_token), word(&resource_iid), 0};
    const std::uint64_t args93[] = {word(&heap_desc), word(&session_token), word(&heap_iid), 0};
    const std::uint64_t args94[] = {word(&descs[0]), 0xBEEF000000000400ull, word(&clear), word(&session_token),
                                    word(&resource_iid), 0};
    const std::uint64_t args109[] = {word(&heap_properties), 0xDEAD000000000244ull, word(&descs1[0]),
                                     0xBEEF0000FFFFFFFFull, word(&clear), word(&session_token), word(&resource_iid), 0};
    const std::uint64_t args110[] = {word(&heap_token), 0x0000000700010000ull, word(&descs1[0]), 0xBEEF000000000080ull,
                                     word(&clear), word(&resource_iid), 0};
    const CreateRoute r92{92, 53, 8, 6, 7, (1u << 1) | (1u << 3), &descs[0], sizeof(D3D12_RESOURCE_DESC)};
    const CreateRoute r93{93, 54, 4, 2, 3, 0, &heap_desc, sizeof(D3D12_HEAP_DESC)};
    const CreateRoute r94{94, 55, 6, 4, 5, 1u << 1, &descs[0], sizeof(D3D12_RESOURCE_DESC)};
    const CreateRoute r109{109, 69, 8, 6, 7, (1u << 1) | (1u << 3), &descs1[0], sizeof(D3D12_RESOURCE_DESC1)};
    const CreateRoute r110{110, 70, 7, 5, 6, 1u << 3, &descs1[0], sizeof(D3D12_RESOURCE_DESC1)};

    // Device8 (and so Device4): every route reaches its PC method on the right interface object.
    {
        Mock* mock = nullptr;
        RouteAlias *device4 = nullptr, *device8 = nullptr;
        void* device = make_route_device(8, mock, device4, device8);
        EXPECT(device && device4 && device8 && device4 != device8);
        EXPECT(word(device4) != word(mock) && word(device8) != word(mock));
        EXPECT(device4->refs == 1 && device8->refs == 1);  // the detection queries gave their references back
        xs::reset_device_proxy_stats();
        rt::d3d12_trace::set_enabled(true);
        if (check_create_route(device, mock, device4, device8, r92, args92) ||
            check_create_route(device, mock, device4, device8, r93, args93) ||
            check_create_route(device, mock, device4, device8, r94, args94) ||
            check_allocation_route(device, mock, device4, device8, 95, 56, descs, sizeof(D3D12_RESOURCE_DESC)) ||
            check_allocation_route(device, mock, device8, device4, 108, 68, descs1, sizeof(D3D12_RESOURCE_DESC1)) ||
            check_create_route(device, mock, device8, device4, r109, args109) ||
            check_create_route(device, mock, device8, device4, r110, args110) ||
            check_footprints_route(device, mock, device8, device4, &descs1[0]))
            return 1;
        rt::d3d12_trace::set_enabled(false);
        // The stray counters: only the routes' PC slots were ever entered on either object.
        for (unsigned slot = 0; slot < kMockSlots; ++slot) {
            const bool device4_slot = slot >= 53 && slot <= 56;
            const bool device8_slot = slot == 68 || slot == 69 || slot == 70 || slot == 72;
            EXPECT(device4_slot || device4->calls[slot] == 0);
            EXPECT(device8_slot || device8->calls[slot] == 0);
        }
        EXPECT(release(device) == 0 && mock->refs == 0 && device4->refs == 1 && device8->refs == 1);
    }

    // Device7 (Device4, no Device8): 92-95 still route; 108-112 are refused explicitly.
    {
        Mock* mock = nullptr;
        RouteAlias *device4 = nullptr, *device8 = nullptr;
        void* device = make_route_device(7, mock, device4, device8);
        EXPECT(device && device4 && !device8);
        xs::reset_device_proxy_stats();
        if (check_create_route(device, mock, device4, nullptr, r92, args92) ||
            expect_allocation_unsupported(device, mock, device4, 108, descs1) ||
            expect_create_unsupported(device, mock, device4, r109, args109) ||
            expect_create_unsupported(device, mock, device4, r110, args110) ||
            expect_footprints_unsupported(device, mock, device4, &descs1[0]))
            return 1;
        EXPECT(device4->calls[68] == 0 && device4->calls[69] == 0 && device4->calls[70] == 0 &&
               device4->calls[72] == 0);
        EXPECT(release(device) == 0 && mock->refs == 0 && device4->refs == 1);
    }

    // Device3 (no Device4): 92-95 are refused explicitly too.
    {
        Mock* mock = nullptr;
        RouteAlias *device4 = nullptr, *device8 = nullptr;
        void* device = make_route_device(3, mock, device4, device8);
        EXPECT(device && !device4 && !device8);
        xs::reset_device_proxy_stats();
        if (expect_create_unsupported(device, mock, nullptr, r92, args92) ||
            expect_create_unsupported(device, mock, nullptr, r93, args93) ||
            expect_create_unsupported(device, mock, nullptr, r94, args94) ||
            expect_allocation_unsupported(device, mock, nullptr, 95, descs))
            return 1;
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: resource routes 92-95/108-110/112 reach ID3D12Device4/8 once each through the queried interface "
                "with the caller's pointers and bytes, balance every reference, and refuse explicitly without the "
                "interface (public reference mapping, no retail call observed)\n", g_label);
    return 0;
}

// ---- 10. The refused tail and the guard region ----

// ---- Device slot 127: ID3D12Device10::CreateCommittedResource3 called the console's way ----
//
// Ten words after `this`, the UINT words with poisoned upper halves, the description with a poisoned padding DWORD.
// The PC device's ID3D12Device10 is a separate alias object; a call on the base pointer would answer E_UNEXPECTED.
int check_committed_resource3() {
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    constexpr std::uint64_t kPoison = 0xA5A5A5A500000000ull;
    constexpr std::uint32_t kMeasuredFlags = 0x00300005;  // RT | UA and two bits outside the PC set
    constexpr std::uint32_t kMeasuredLayout = 3;          // the measured initial layout word
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC1 desc;
    std::memset(&desc, 0xCD, sizeof(desc));  // the padding DWORD at +4 keeps this poison
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = 3840;
    desc.Height = 2160;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = static_cast<DXGI_FORMAT>(28);
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(kMeasuredFlags);
    desc.SamplerFeedbackMipRegion = {0, 0, 0};
    D3D12_CLEAR_VALUE clear{};
    clear.Format = static_cast<DXGI_FORMAT>(28);
    const D3D12_HEAP_PROPERTIES heap0 = heap;
    D3D12_RESOURCE_DESC1 desc0;
    std::memcpy(&desc0, &desc, sizeof(desc));
    const D3D12_CLEAR_VALUE clear0 = clear;
    const auto unchanged = [&] {
        return std::memcmp(&heap, &heap0, sizeof(heap)) == 0 && std::memcmp(&desc, &desc0, sizeof(desc)) == 0 &&
               std::memcmp(&clear, &clear0, sizeof(clear)) == 0;
    };

    Mock* mock = nullptr;
    void* device = make_device(mock, 10, true);
    EXPECT(device && xs::device_proxy_slot_count(device) == 79);
    Mock* alias = new_mock(Kind::device);
    alias->refs = 1;
    mock->device10_alias = alias;
    mock->create_result = E_UNEXPECTED;  // the trap: only the alias may create
    xs::reset_device_proxy_stats();
    g_committed3 = {};
    const Committed3Seen& s = g_committed3;
    const auto proxy_refs = xs::device_proxy_ref_count(device);
    const long base_refs = mock->refs;
    void* resource = reinterpret_cast<void*>(1);
    constexpr auto kDisplay = static_cast<std::uint32_t>(D3D12_HEAP_FLAG_ALLOW_DISPLAY);
    std::uint64_t w[10] = {word(&heap),  kPoison | kDisplay, word(&desc), kPoison | kMeasuredLayout,
                           word(&clear), 0, kPoison, 0, word(&kResourceIid), word(&resource)};
    const auto balanced = [&] {
        return alias->refs == 1 && mock->refs == base_refs && xs::device_proxy_ref_count(device) == proxy_refs;
    };

    // The measured request: every word reaches the alias's CreateCommittedResource3 as passed, flags included, and the
    // backend's refusal of the unknown bits comes back with a null output.
    EXPECT(hr_of(raw10(device, 127, w)) == E_INVALIDARG && resource == nullptr);
    EXPECT(s.calls == 1 && s.device == alias && mock->calls[76] == 0 && alias->calls[76] == 1);
    EXPECT(s.heap == &heap && s.heap_flags == kDisplay && s.desc == &desc);
    EXPECT(std::memcmp(s.desc_bytes, &desc0, sizeof(desc0)) == 0);  // padding poison included
    D3D12_RESOURCE_DESC1 seen;
    std::memcpy(&seen, s.desc_bytes, sizeof(seen));
    EXPECT(static_cast<std::uint32_t>(seen.Flags) == kMeasuredFlags && seen.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN);
    EXPECT(s.layout == kMeasuredLayout && s.clear == &clear && !s.session && s.castable_count == 0 && !s.castable);
    EXPECT(s.riid == &kResourceIid && s.out == &resource);
    EXPECT(unchanged() && balanced());
    EXPECT(xs::device_slot_hits(127) == 1 && xs::device_slot_overridden(127));

    // A PC-shaped request (flags RT | UA) with a session and a castable list: a real mock resource, one reference.
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x5);
    std::memcpy(&desc0, &desc, sizeof(desc));
    static const int session_sentinel = 0;  // never dereferenced
    const DXGI_FORMAT castable[2] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    w[5] = word(&session_sentinel);
    w[6] = kPoison | 2;
    w[7] = word(castable);
    resource = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw10(device, 127, w)) == S_OK && resource);
    EXPECT(s.calls == 2 && s.device == alias && s.session == &session_sentinel && s.castable_count == 2 &&
           s.castable == castable && s.out == &resource && s.layout == kMeasuredLayout);
    EXPECT(reinterpret_cast<Mock*>(resource)->kind == Kind::resource && alias->last_child == resource);
    EXPECT(unchanged() && balanced());
    EXPECT(release(resource) == 0);

    // The backend's failure comes back as it is, with a null output.
    alias->create_result = E_OUTOFMEMORY;
    resource = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw10(device, 127, w)) == E_OUTOFMEMORY && !resource && s.calls == 3 && balanced());
    alias->create_result = S_OK;

    // The optional output: a capability check is forwarded with a null output and its S_FALSE kept.
    w[9] = 0;
    EXPECT(hr_of(raw10(device, 127, w)) == S_FALSE && s.calls == 4 && s.out == nullptr && balanced());
    w[9] = word(&resource);

    // Null heap, description or IID: E_POINTER, no QueryInterface, no backend call; an all-null call is safe too.
    const unsigned calls = s.calls, queries = mock->device10_queries;
    for (const int i : {0, 2, 8}) {
        std::uint64_t bad[10];
        std::memcpy(bad, w, sizeof(bad));
        bad[i] = 0;
        resource = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw10(device, 127, bad)) == E_POINTER && !resource);
    }
    const std::uint64_t none[10] = {};
    EXPECT(hr_of(raw10(device, 127, none)) == E_POINTER);
    EXPECT(s.calls == calls && mock->device10_queries == queries && balanced());

    // The slot count says ID3D12Device10, QueryInterface refuses it: E_NOTIMPL, null output, no other method called.
    mock->refuse_device10 = true;
    resource = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw10(device, 127, w)) == E_NOTIMPL && !resource && s.calls == calls);
    EXPECT(mock->device10_queries == queries + 1 && xs::device_proxy_slot_count(device) == 79 && balanced());
    mock->refuse_device10 = false;
    EXPECT(release(device) == 0);

    // No alias: QueryInterface answers the device itself, and its reference is given back.
    Mock* plain = nullptr;
    void* plain_device = make_device(plain, 10, true);
    EXPECT(plain_device);
    const long plain_refs = plain->refs;
    resource = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw10(plain_device, 127, w)) == S_OK && resource && s.device == plain && plain->refs == plain_refs);
    EXPECT(release(resource) == 0);
    EXPECT(release(plain_device) == 0);

    // A PC device that stops at ID3D12Device9: E_NOTIMPL with a null output, PC slot 76 never reached.
    Mock* old_mock = nullptr;
    void* old_device = make_device(old_mock, 9, true);
    EXPECT(old_device && xs::device_proxy_slot_count(old_device) == 76);
    const unsigned old_queries = old_mock->device10_queries, before = s.calls;
    resource = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw10(old_device, 127, w)) == E_NOTIMPL && !resource);
    EXPECT(s.calls == before && old_mock->calls[76] == 0 && old_mock->device10_queries == old_queries + 1);
    EXPECT(release(old_device) == 0);
    std::printf("%s: device slot 127 forwards every word to ID3D12Device10::CreateCommittedResource3 unchanged; "
                "unknown flags reach the backend, which refuses them\n", g_label);
#else
    std::printf("%s: device slot 127 not checked: the SDK has no ID3D12Device10\n", g_label);
#endif
    return 0;
}

// A configured route moved onto 127 (inside 3..127) keeps its priority over the built-in adapter.
int run_env_slot127() {
    EXPECT(SetEnvironmentVariableA("PALMWAVE_XS_FRAME_INTERVAL_SLOT", "127") != FALSE);
    Mock* mock = nullptr;
    void* device = make_device(mock, 10, true);
    EXPECT(device && xs::device_proxy_slot_count(device) == 79);
    xs::reset_device_proxy_stats();
    g_committed3 = {};
    const unsigned queries = mock->device10_queries;
    EXPECT(hr_of(raw(device, 127, 0, 16667, 1, 0)) == S_OK);
    EXPECT(g_committed3.calls == 0 && mock->calls[76] == 0 && mock->device10_queries == queries &&
           xs::device_slot_hits(127) == 1);
    EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK && mock->calls[78] == 1);  // the old default: forwarded
    EXPECT(release(device) == 0);
    std::printf("%s: a route moved onto device slot 127 takes precedence over CreateCommittedResource3\n", g_label);
    return 0;
}

int check_tail(bool want_guarded) {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    void* queue = make_queue(device);
    void* fence = make_fence(device);
    EXPECT(queue && fence);
    EXPECT(xs::command_object_tables_guarded() == want_guarded);
    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    // Signal (14) still reaches the real queue, with the real fence.
    EXPECT(hr_of(raw(queue, 14, word(fence), 5)) == S_OK && queue_mock->calls[14] == 1 &&
           queue_mock->args[14][0] == word(xs::fence_proxy_real_fence(fence)) && queue_mock->args[14][1] == 5);
    for (unsigned slot = 19; slot < xs::queue_proxy_table_size(); ++slot)
        EXPECT(hr_of(raw(queue, slot)) == E_NOTIMPL && queue_mock->calls[slot] == 0);
    for (unsigned slot = 12; slot < xs::fence_proxy_table_size(); ++slot) EXPECT(hr_of(raw(fence, slot)) == E_NOTIMPL);
    EXPECT(xs::queue_table_overruns() == 0 && xs::fence_table_overruns() == 0);
    EXPECT(release(queue) == 0 && release(fence) == 0 && release(device) == 0 && mock->refs == 0);
    return 0;
}

// ---- Guard-region setup failures: this executable's import table, patched before the first proxy table ----

std::atomic<int> g_injected{0};
bool g_armed = false;
void* g_real = nullptr;

LPVOID WINAPI fail_virtual_alloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
    if (g_armed && (type & MEM_RESERVE) && size == 64 * 1024) {
        ++g_injected;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    return reinterpret_cast<decltype(&VirtualAlloc)>(g_real)(address, size, type, protect);
}

BOOL WINAPI fail_virtual_protect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old) {
    if (g_armed && protect == PAGE_READONLY) {
        ++g_injected;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return reinterpret_cast<decltype(&VirtualProtect)>(g_real)(address, size, protect, old);
}

PVOID WINAPI fail_add_veh(ULONG, PVECTORED_EXCEPTION_HANDLER) {
    ++g_injected;
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
}

// Points this executable's import of `function` (by name) at `replacement`; the proxy code is linked into it.
bool patch_import(const char* function, void* replacement) {
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) return false;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress); desc->Name; ++desc) {
        const DWORD lookup = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + lookup);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), function) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            g_real = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);  // through the replacement, not armed yet
            return true;
        }
    }
    return false;
}

int run_fault_mode(const char* mode) {
    struct Mode { const char* flag; const char* function; void* replacement; bool guarded; } const modes[] = {
        {"--fail-virtualalloc", "VirtualAlloc", reinterpret_cast<void*>(&fail_virtual_alloc), false},
        {"--fail-virtualprotect", "VirtualProtect", reinterpret_cast<void*>(&fail_virtual_protect), true},
        {"--fail-veh", "AddVectoredExceptionHandler", reinterpret_cast<void*>(&fail_add_veh), false},
    };
    for (const Mode& m : modes) {
        if (std::strcmp(mode, m.flag) != 0) continue;
        g_label = m.flag;
        EXPECT(patch_import(m.function, m.replacement));
        g_armed = true;
        const int failed = check_tail(m.guarded);
        // Disarmed once the tables exist: with a static CRT the CRT's own calls go through this import table too
        // (its exit path asks VirtualProtect for read-only pages), and only the proxy's calls are meant here.
        g_armed = false;
        if (failed) return 1;
        EXPECT(g_injected.load() > 0);  // the proxy's own call was the one refused
        std::printf("%s: %s refused %d time(s): the tables %s guarded, the proxies forward and refuse their tail as "
                    "before\n", m.flag, m.function, g_injected.load(), m.guarded ? "stay" : "are not");
        return 0;
    }
    return 2;
}

}  // namespace

// A failure ends the process with a message on stderr, never in a dialog a test run would wait on: abort() (an
// exception that escapes, for one), the debug CRT's reports, and Windows' fault and critical-error boxes.
static void no_dialogs() {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG  // the report functions exist only in the debug CRT (empty macros otherwise)
    for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

int main(int argc, char** argv) {
    no_dialogs();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && std::strcmp(argv[1], "--slot133") == 0) return check_placed_resource_refusal();
    if (argc == 2 && std::strcmp(argv[1], "--slot70") == 0) {
        // The same checks with the D3D12 trace on (as palmwave --trace-d3d12): every refusal is a traced line.
        rt::d3d12_trace::set_enabled(true);
        return check_srv_at_va();
    }
    if (argc == 2 && std::strcmp(argv[1], "--slot127") == 0) {
        rt::d3d12_trace::set_enabled(true);  // every outcome is a traced line, as under palmwave --trace-d3d12
        return check_committed_resource3();
    }
    if (argc == 2 && std::strcmp(argv[1], "--slot25") == 0) return check_resource_allocation_info();
    if (argc == 2 && std::strcmp(argv[1], "--resource-routes") == 0) return check_resource_routes();
    if (argc == 3 && std::strcmp(argv[1], "--host-import-experiment") == 0) {
        if (std::strcmp(argv[2], "on") == 0) return run_host_import_experiment(true);
        if (std::strcmp(argv[2], "off") == 0) return run_host_import_experiment(false);
        return 2;
    }
    if (argc == 2) {
        if (std::strcmp(argv[1], "--env-slots") == 0) return run_env_slots(true);
        if (std::strcmp(argv[1], "--env-slots-invalid") == 0) return run_env_slots(false);
        if (std::strcmp(argv[1], "--env-slots-70") == 0) return run_env_slots(true, true);
        if (std::strcmp(argv[1], "--env-slots-127") == 0) return run_env_slot127();
        return run_fault_mode(argv[1]);
    }
    if (argc != 1) return 2;
    if (check_placed_resource_refusal() || check_tail(true) || check_levels() || check_fence1() || check_create_failures() || check_open_shared() ||
        check_alloc_failures() || check_allocators() || check_xbox_slots() || check_xbox_queue_descriptions() ||
        check_lists() || check_shared_base_list() || check_experimental_flush() ||
        check_custom_heap_properties() || check_resource_allocation_info() || check_descriptor_heaps() || check_list_descriptor_handles() ||
        check_srv_at_va() || check_committed_resource3())
        return 1;
    std::printf("xs_proxy_mock_test: PASS (%d allocations refused on purpose)\n", g_failed_news);
    return 0;
}
