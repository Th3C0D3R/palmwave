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
//   - console command lists from device slot 73: the object body, the console->PC slot table, ownership, unwrapping
//     (allocator, bundle, heap array, queue ExecuteCommandLists), unsupported slots failing Close and refusing the
//     submission until Reset, AllocateUploadMemory over a mapped mock buffer, and the unwrapped PC path of slot 12;
//   - --fail-virtualalloc / --fail-virtualprotect / --fail-veh: the guard region's setup failing, injected by
//     patching this executable's own import table before the first proxy table is built (one process each);
//   - --host-import-experiment on|off: device slot 133's opt-in experiment (PALMWAVE_EXPERIMENTAL_XBOX_HOST_IMPORT, set
//     by CTest for the process, never by the test): the one experimental tuple reaches the handler as UAV / COMMON
//     only when the variable is "1"; every near miss is refused before the handler either way.
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
enum class Kind { device, queue, fence, resource, allocator, command_list, list };

constexpr unsigned kMockSlots = 192;
constexpr std::uint64_t kMockGpuBase = 0x00007f0000000000ull;  // mock buffers' GPU addresses, unlike any CPU one
// The console command-list IID the 1.0.2.1 executable passes to device slot 73 (d3d12_xs_command_list_iid.h).
constexpr GUID kConsoleListIid{0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
constexpr std::uint64_t kFenceHandle = 0x1000, kResourceHandle = 0x2000;  // the handles the mock device opens
constexpr std::uint64_t kCompletedValue = 7;                            // what a mock fence's slot 8 returns
constexpr UINT kHeapMarker = 0x26262626;  // the VisibleNodeMask a mock device's slot 26 answers
// {5d1e0c4a-2b7f-4e61-9a3d-8c4b2f1e6a70}: an interface the mock queue implements that the proxy does not model.
constexpr GUID kExtraIid{0x5d1e0c4a, 0x2b7f, 0x4e61, {0x9a, 0x3d, 0x8c, 0x4b, 0x2f, 0x1e, 0x6a, 0x70}};

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
    unsigned calls[kMockSlots];
    std::uint64_t args[kMockSlots][5];
    std::uint64_t array[kMockSlots][4];  // device: the first entries of the array argument, as received
};

Mock g_pool[256];
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
    if (!riid || !mock_has(m, *riid)) return E_NOINTERFACE;
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
    device->last_child = child;
    return mock_query(child, reinterpret_cast<const IID*>(riid), out);
}

void copy_array(Mock* m, unsigned slot, std::uint64_t objects, std::uint64_t count) {
    const auto* in = reinterpret_cast<const std::uint64_t*>(objects);
    for (std::uint32_t i = 0; in && i < static_cast<std::uint32_t>(count) && i < 4; ++i) m->array[slot][i] = in[i];
}

// `words` are the call's first five words (a1..a5), copied into the thunk's own frame.
std::uint64_t mock_call(Mock* m, unsigned slot, const std::uint64_t* words) {
    const std::uint64_t a1 = words[0], a2 = words[1], a3 = words[2], a4 = words[3];
    if (m->kind == Kind::device) {
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

template <std::size_t... I>
void fill_mock_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&mock_slot<static_cast<unsigned>(I)>)), ...);
}

void** mock_vtable() {
    static void* table[kMockSlots];
    static const bool filled = [] {
        fill_mock_vtable(table, std::make_index_sequence<kMockSlots>{});
        table[12] = reinterpret_cast<void*>(&mock_create_command_list);
        table[27] = reinterpret_cast<void*>(&mock_create_committed_resource);
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

int run_env_slots(bool valid) {
    const char* frame_interval = valid ? "96" : "2";
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
    std::uint64_t token = 0;
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
        EXPECT(hr_of(raw(device, 70)) == S_OK && mock->calls[70] == 1);  // not an Xbox slot: forwarded
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

// ---- 10. The refused tail and the guard region ----

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
    if (argc == 3 && std::strcmp(argv[1], "--host-import-experiment") == 0) {
        if (std::strcmp(argv[2], "on") == 0) return run_host_import_experiment(true);
        if (std::strcmp(argv[2], "off") == 0) return run_host_import_experiment(false);
        return 2;
    }
    if (argc == 2) {
        if (std::strcmp(argv[1], "--env-slots") == 0) return run_env_slots(true);
        if (std::strcmp(argv[1], "--env-slots-invalid") == 0) return run_env_slots(false);
        return run_fault_mode(argv[1]);
    }
    if (argc != 1) return 2;
    if (check_placed_resource_refusal() || check_tail(true) || check_levels() || check_fence1() || check_create_failures() || check_open_shared() ||
        check_alloc_failures() || check_allocators() || check_xbox_slots() || check_xbox_queue_descriptions() ||
        check_lists() || check_shared_base_list() || check_experimental_flush() ||
        check_custom_heap_properties())
        return 1;
    std::printf("xs_proxy_mock_test: PASS (%d allocations refused on purpose)\n", g_failed_news);
    return 0;
}
