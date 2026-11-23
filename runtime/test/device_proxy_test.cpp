// Exercises the Xbox-ABI device proxy against a real WARP PC device, with no game executable.
//
// It creates the proxy through the registered d3d12_xs!D3D12XboxCreateDevice shim, then calls the measured Xbox
// slots exactly as the guest would (raw function pointers, not the C++ interface):
//   slot 14  CreateDescriptorHeap (returns a heap proxy)
//   heap 9/10 GetCPU/GetGPUDescriptorHandleForHeapStart (Xbox: 8-byte handle in RAX; the PC heap uses the
//             hidden return pointer in RDX, so slot 14 wraps it)
//   slot 62  CreateComputePipelineState
//   slot 76  resource creation with an Xbox layout word (0x101)
//   slot 8/36 CreateCommandQueue/CreateFence (return queue and fence proxies that run GPU work)
// Slot 9 on the device is CreateCommandAllocator on the PC vtable and must still forward, which proves the handle
// getters were not wedged into the device slots. Console output also shows the proxy's first-call slot log.
// It also registers this executable as a synthetic title image with __XMemAllocationHooks__ (registry bounds and
// cleanup), creates title-backed heaps through slot 14, and clears a title-handle RTV to (255, 0, 128, 255) and DSV to
// Depth 0.25 through console list slots 50/51/52 with a readback on WARP. That is a synthetic check, not a title frame.
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <wrl/client.h>

#include "d3d12_trace.h"
#include "d3d12_xs_command_list_iid.h"
#include "d3d12_xs_device.h"
#include "shims.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    if (!condition) ++g_failures;
}

using Microsoft::WRL::ComPtr;

using XboxCreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, const void*, const IID*, void**);
using XboxCreateResourceFn = HRESULT(WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                              const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                              const D3D12_CLEAR_VALUE*, REFIID, void**);
// Xbox slot 62: (desc, two words, iid, out), as XboxBvhBuilder.dll calls it (zeros for the two words).
using XboxCreateComputePsoFn = HRESULT(WINAPI*)(void*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, std::uint64_t,
                                                std::uint64_t, REFIID, void**);
using GetCpuHandleFn = D3D12_CPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*);
using GetGpuHandleFn = D3D12_GPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*);

D3D12_RESOURCE_DESC buffer_desc() {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 4096;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);  // measured Xbox tiled/buffer word
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

D3D12_RESOURCE_DESC texture_desc() {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 4;
    desc.Height = 4;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

D3D12_HEAP_PROPERTIES default_heap() {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    return heap;
}

// ---- A synthetic title's __XMemAllocationHooks__ (registration and slot 14 backing, upstream PR #7021 adapted) ----
//
// The routines and the struct live in this executable, which is registered as the "title image" below, so the
// registry's in-image checks hold for them. Each allocation hands out `next` (null: the hook has no memory) and records
// what it was asked; each free is recorded.
struct TitleHookState {
    std::uint8_t cbv[4096];  // the title's backings: only their addresses are used, never their bytes
    std::uint8_t rtv[1024];
    std::uint8_t dsv[1024];
    void* next;
    std::size_t next_capacity;
    int allocations;
    std::size_t last_size;
    std::uint64_t last_attributes;
    int frees;
    void* last_freed;
    std::uint64_t last_free_attributes;
};
TitleHookState g_title;

void* title_alloc(std::size_t size, std::uint64_t attributes) {
    ++g_title.allocations;
    g_title.last_size = size;
    g_title.last_attributes = attributes;
    return size <= g_title.next_capacity ? g_title.next : nullptr;
}

void title_free(void* address, std::uint64_t attributes) {
    ++g_title.frees;
    g_title.last_freed = address;
    g_title.last_free_attributes = attributes;
}

// Mutable, as a title's exported struct is: the checks below rewrite it.
rt::shims::XMemAllocationHooks g_title_hooks{&title_alloc, &title_free};

void hand_out(void* memory, std::size_t capacity) {
    g_title.next = memory;
    g_title.next_capacity = capacity;
}

void this_image(const void*& base, std::uint64_t& size) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + dos->e_lfanew);
    base = b;
    size = nt->OptionalHeader.SizeOfImage;
}

// Registration bounds and cleanup in the shim registry (no device needed).
void check_hook_registration() {
    using rt::shims::XMemHooksState;
    const void* base = nullptr;
    std::uint64_t size = 0;
    this_image(base, size);
    const void* const owner = &g_title;
    const void* const other_owner = &g_title_hooks;
    check(rt::shims::xmem_allocation_hooks().state == XMemHooksState::None, "no __XMemAllocationHooks__ registered");

    check(rt::shims::add_xmem_allocation_hooks(owner, base, size, &g_title_hooks), "hooks inside the image register");
    auto snapshot = rt::shims::xmem_allocation_hooks();
    check(snapshot.state == XMemHooksState::Ready && snapshot.alloc == &title_alloc && snapshot.free == &title_free &&
              snapshot.generation != 0,
          "the registered hooks are ready with their own pair");
    const std::uint64_t generation = snapshot.generation;
    check(!rt::shims::add_xmem_allocation_hooks(other_owner, base, size, &g_title_hooks) &&
              rt::shims::xmem_allocation_hooks().generation == generation,
          "a second exporting image is refused while one registration is active");
    g_title_hooks = {nullptr, nullptr};
    check(rt::shims::xmem_allocation_hooks().state == XMemHooksState::None, "a struct of two nulls installs no hook");
    g_title_hooks = {&title_alloc, nullptr};
    check(rt::shims::xmem_allocation_hooks().state == XMemHooksState::Invalid, "a half-filled struct is invalid");
    g_title_hooks = {&title_alloc,
                     reinterpret_cast<rt::shims::XMemFreeHookFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                                                "Sleep"))};
    check(rt::shims::xmem_allocation_hooks().state == XMemHooksState::Invalid,
          "a routine outside the registering image is invalid");
    g_title_hooks = {&title_alloc, &title_free};
    rt::shims::remove_xmem_allocation_hooks(other_owner);  // not the active owner: nothing changes
    check(rt::shims::xmem_allocation_hooks().state == XMemHooksState::Ready, "removing another owner keeps the hooks");
    rt::shims::remove_xmem_allocation_hooks(owner);
    check(rt::shims::xmem_allocation_hooks().state == XMemHooksState::None, "removing the owner unregisters");
    const int frees = g_title.frees;
    check(!rt::shims::xmem_free_with_hooks(generation, &title_free, g_title.cbv, 4) && g_title.frees == frees,
          "a free after unregistration does not call the routine");

    const auto* image_end = static_cast<const std::uint8_t*>(base) + size;
    check(!rt::shims::add_xmem_allocation_hooks(owner, base, size, image_end - 8) &&
              rt::shims::xmem_allocation_hooks().state == XMemHooksState::Invalid,
          "a struct running past the image end registers as invalid");
    rt::shims::remove_xmem_allocation_hooks(owner);
    check(!rt::shims::add_xmem_allocation_hooks(owner, base, size, nullptr) &&
              rt::shims::xmem_allocation_hooks().state == XMemHooksState::None,
          "a null struct registers nothing");
    check(rt::shims::add_xmem_allocation_hooks(owner, base, size, &g_title_hooks) &&
              rt::shims::xmem_allocation_hooks().generation > generation,
          "a new registration gets a new generation");
    rt::shims::remove_xmem_allocation_hooks(owner);
}

bool load_compute_shader(ComPtr<ID3DBlob>& bytecode) {
    HMODULE compiler = LoadLibraryExA("d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(compiler != nullptr, "d3dcompiler_47.dll loads");
    auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    check(compile != nullptr, "d3dcompiler_47.dll exports D3DCompile");
    if (!compile) return false;

    static const char kSource[] =
        "[numthreads(1, 1, 1)]\n"
        "void main(uint3 id : SV_DispatchThreadID) { }\n";
    ComPtr<ID3DBlob> errors;
    HRESULT hr = compile(kSource, sizeof(kSource) - 1, "device_proxy_test.hlsl", nullptr, nullptr, "main",
                         "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, bytecode.GetAddressOf(),
                         errors.GetAddressOf());
    if (FAILED(hr) && errors) {
        std::printf("shader compile error: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
    }
    check(SUCCEEDED(hr) && bytecode, "trivial compute shader compiles (cs_5_0)");
    return SUCCEEDED(hr) && bytecode;
}

}  // namespace

int main(int argc, char** argv) {
    // --trace-d3d12, as palmwave takes it: device_proxy_trace checks that the parameter words still print then.
    if (argc == 2 && std::strcmp(argv[1], "--trace-d3d12") == 0) rt::d3d12_trace::set_enabled(true);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("device_proxy_test: Xbox-ABI device proxy over WARP\n");

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    check(SUCCEEDED(hr) && factory, "DXGI factory created");
    ComPtr<IDXGIAdapter> warp;
    if (factory) hr = factory->EnumWarpAdapter(IID_PPV_ARGS(warp.GetAddressOf()));
    if (!factory) {
        std::printf("device_proxy_test: FAILED (no DXGI factory)\n");
        return 1;
    }
    if (!warp) {
        // Windows always has WARP; Wine doesn't. CTest reads exit code 77 as a skip (SKIP_RETURN_CODE).
        std::printf("SKIP device_proxy_test: no WARP adapter (0x%08lX)\n", static_cast<unsigned long>(hr));
        return 77;
    }
    check(true, "WARP adapter enumerated");

    auto create_device = reinterpret_cast<XboxCreateDeviceFn>(
        rt::shims::find("d3d12_xs.dll", "D3D12XboxCreateDevice"));
    check(create_device != nullptr, "shim registry exposes d3d12_xs!D3D12XboxCreateDevice");
    if (!create_device) return 1;

    // Word 0 is the version that selects the console queue layout; 0 keeps the PC queue layout this test's PC calls
    // (Signal 14, ExecuteCommandLists 10) need. Word 1 is device_proxy_trace's marker, logged only.
    std::uint32_t params[24] = {0, 0x1234abcd};
    const IID iid_device = __uuidof(ID3D12Device);
    void* created = nullptr;
    hr = create_device(warp.Get(), params, &iid_device, &created);
    check(SUCCEEDED(hr) && created != nullptr, "D3D12XboxCreateDevice returns a device");
    if (!created) {
        std::printf("device_proxy_test: FAILED (no device)\n");
        return 1;
    }

    ComPtr<ID3D12Device> device;
    device.Attach(static_cast<ID3D12Device*>(created));
    check(rt::d3d12_xs::is_device_proxy(device.Get()), "returned pointer is an Xbox-ABI proxy");

    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device.Get());
    check(real != nullptr, "proxy exposes its PC device");
    unsigned slots = rt::d3d12_xs::device_proxy_slot_count(device.Get());
    std::printf("proxy forwards %u PC vtable slots (ID3D12Device..Device14 max is 83)\n", slots);
    check(slots >= 44, "proxy forwards at least the base ID3D12Device vtable");
    check(rt::d3d12_xs::device_slot_overridden(9), "device slot 9 is CreateCommandAllocator (allocator proxies)");
    check(!rt::d3d12_xs::device_slot_overridden(10), "device slot 10 is forwarded (PC CreateGraphicsPipelineState)");
    check(rt::d3d12_xs::device_slot_overridden(14), "device slot 14 is the descriptor-heap override");
    check(rt::d3d12_xs::device_slot_overridden(62), "device slot 62 is the compute-PSO override");
    check(rt::d3d12_xs::device_slot_overridden(76), "device slot 76 is the resource override");
    check(rt::d3d12_xs::device_slot_overridden(67),
          "device slot 67 is the DeserializeComputePipelineStateX override (console native-code compute pipeline, not PC CreateProtectedResourceSession1)");

    void** proxy_vtbl = *reinterpret_cast<void***>(device.Get());
    void** real_vtbl = real ? *reinterpret_cast<void***>(real) : nullptr;
    check(proxy_vtbl != nullptr && proxy_vtbl[14] != real_vtbl[14], "slot 14 thunk differs from the PC entry");
    if (slots > 62) check(proxy_vtbl[62] != real_vtbl[62], "slot 62 thunk differs from PC CreateStateObject");
    if (slots > 67) check(proxy_vtbl[67] != real_vtbl[67], "slot 67 thunk differs from PC CreateProtectedResourceSession1");
    if (slots > 76) check(proxy_vtbl[76] != real_vtbl[76], "slot 76 thunk differs from PC CreateCommittedResource3");

    // The hit counters are for tests and count from the first read or reset, so that a title's calls write no
    // shared counter. The QueryInterface of the creation above was before that: its flag is set, its hit is not.
    check(rt::d3d12_xs::device_slot_first_call_seen(0), "slot 0 first-call flag is set by the creation");
    check(rt::d3d12_xs::device_slot_hits(0) == 0, "slot 0 has no hit before the first read of a counter");
    device->AddRef();
    device->Release();
    check(rt::d3d12_xs::device_slot_hits(1) == 1 && rt::d3d12_xs::device_slot_hits(2) == 1,
          "slots 1 and 2 count the calls after the first read");

    rt::d3d12_xs::reset_device_proxy_stats();

    // QI/refcount behaviour: device interfaces resolve to the same proxy, non-device IIDs do not.
    ID3D12Device* same = nullptr;
    hr = device->QueryInterface(__uuidof(ID3D12Device), reinterpret_cast<void**>(&same));
    check(SUCCEEDED(hr) && same == device.Get(), "QueryInterface(IID_ID3D12Device) returns the same proxy");
    if (same) same->Release();
    ComPtr<ID3D12Device1> device1;
    hr = device->QueryInterface(IID_PPV_ARGS(device1.GetAddressOf()));
    check(SUCCEEDED(hr) && device1 != nullptr, "QueryInterface(IID_ID3D12Device1) succeeds");
    ComPtr<ID3D12CommandQueue> not_a_device;
    hr = device->QueryInterface(IID_PPV_ARGS(not_a_device.GetAddressOf()));
    check(FAILED(hr) && !not_a_device, "QueryInterface rejects a non-device IID");
    check(rt::d3d12_xs::device_slot_hits(0) >= 3, "slot 0 hit counter moves");

    // Generic-forwarder coverage: slot 43 returns an 8-byte struct through the hidden pointer, and slot 30 has
    // two stack arguments. Both must reach the PC device unchanged (they are not measured overrides).
    const LUID proxy_luid = device->GetAdapterLuid();
    const LUID real_luid = real ? real->GetAdapterLuid() : LUID{};
    check(proxy_luid.LowPart == real_luid.LowPart && proxy_luid.HighPart == real_luid.HighPart,
          "slot 43 forwards GetAdapterLuid (struct return)");
    D3D12_RESOURCE_DESC reserved_desc = buffer_desc();
    reserved_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> reserved;
    hr = device->CreateReservedResource(&reserved_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                        IID_PPV_ARGS(reserved.GetAddressOf()));
    check(SUCCEEDED(hr) && reserved != nullptr, "slot 30 forwards CreateReservedResource (stack arguments)");

    // Slot 14: descriptor heap through the proxy. The proxy wraps the real PC heap for the handle ABI.
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 8;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    const unsigned long refs_before_heap = rt::d3d12_xs::device_proxy_ref_count(device.Get());
    hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(heap.GetAddressOf()));
    check(SUCCEEDED(hr) && heap != nullptr, "slot 14 creates a descriptor heap");
    check(rt::d3d12_xs::device_slot_hits(14) == 1, "slot 14 hit counter is 1");
    check(heap && rt::d3d12_xs::device_proxy_ref_count(device.Get()) == refs_before_heap + 1,
          "heap holds one device proxy reference");

    if (heap) {
        check(rt::d3d12_xs::is_heap_proxy(heap.Get()), "slot 14 returns a heap proxy");
        ID3D12DescriptorHeap* real_heap = rt::d3d12_xs::heap_proxy_real_heap(heap.Get());
        check(real_heap != nullptr, "heap proxy exposes the PC heap");
        check(rt::d3d12_xs::heap_slot_overridden(9), "heap slot 9 is the CPU-handle override");
        check(rt::d3d12_xs::heap_slot_overridden(10), "heap slot 10 is the GPU-handle override");
        ID3D12DescriptorHeap* same_heap = nullptr;
        hr = heap->QueryInterface(__uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&same_heap));
        check(SUCCEEDED(hr) && same_heap == heap.Get(), "heap QueryInterface returns the same heap proxy");
        if (same_heap) same_heap->Release();

        // ID3D12DeviceChild::GetDevice must return the Xbox-ABI device, not the raw PC device, and AddRef it.
        check(rt::d3d12_xs::heap_slot_overridden(7), "heap slot 7 is the GetDevice override");
        ID3D12Device* heap_device = nullptr;
        const unsigned long refs_before_get = rt::d3d12_xs::device_proxy_ref_count(device.Get());
        hr = heap->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&heap_device));
        check(SUCCEEDED(hr) && heap_device == device.Get() && rt::d3d12_xs::is_device_proxy(heap_device),
              "heap GetDevice returns the device proxy");
        check(rt::d3d12_xs::device_proxy_ref_count(device.Get()) == refs_before_get + 1,
              "GetDevice AddRefs the device proxy");
        check(rt::d3d12_xs::heap_slot_hits(7) == 1, "heap slot 7 hit counter is 1");
        if (heap_device) heap_device->Release();
        check(rt::d3d12_xs::device_proxy_ref_count(device.Get()) == refs_before_get,
              "GetDevice release balances");

        // The Xbox call shape: `this` only, 8-byte handle in RAX. The proxy converts from the PC heap's
        // hidden return pointer (RDX). A PC C++ call through the proxy uses the PC convention, so it is not
        // used here; the real heap below shows that convention and the conversion is compared against it.
        void** proxy_heap_vtbl = *reinterpret_cast<void***>(heap.Get());
        auto get_cpu = reinterpret_cast<GetCpuHandleFn>(proxy_heap_vtbl[9]);
        auto get_gpu = reinterpret_cast<GetGpuHandleFn>(proxy_heap_vtbl[10]);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = get_cpu(heap.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = get_gpu(heap.Get());
        check(cpu.ptr != 0, "heap proxy slot 9 returns a CPU handle in RAX");
        check(gpu.ptr != 0, "heap proxy slot 10 returns a GPU handle in RAX");
        if (real_heap) {
            void** real_heap_vtbl = *reinterpret_cast<void***>(real_heap);
            check(proxy_heap_vtbl[9] != real_heap_vtbl[9], "heap slot 9 thunk differs from the PC heap entry");
            check(proxy_heap_vtbl[10] != real_heap_vtbl[10], "heap slot 10 thunk differs from the PC heap entry");
        }

        // PC reality check: the real heap takes a hidden return pointer in RDX. This is the deviation that
        // makes the wrapper necessary; a one-argument call on the real heap is invalid and is not made.
        if (real_heap) {
            void** real_heap_vtbl = *reinterpret_cast<void***>(real_heap);
            using GetCpuOutFn = void(WINAPI*)(ID3D12DescriptorHeap*, D3D12_CPU_DESCRIPTOR_HANDLE*);
            using GetGpuOutFn = void(WINAPI*)(ID3D12DescriptorHeap*, D3D12_GPU_DESCRIPTOR_HANDLE*);
            constexpr SIZE_T kSentinel = 0x1111111111111111ull;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu_pc{};
            D3D12_GPU_DESCRIPTOR_HANDLE gpu_pc{};
            cpu_pc.ptr = kSentinel;
            gpu_pc.ptr = kSentinel;
            reinterpret_cast<GetCpuOutFn>(real_heap_vtbl[9])(real_heap, &cpu_pc);
            reinterpret_cast<GetGpuOutFn>(real_heap_vtbl[10])(real_heap, &gpu_pc);
            check(cpu_pc.ptr != kSentinel, "PC heap slot 9 writes through the RDX return pointer");
            check(gpu_pc.ptr != kSentinel, "PC heap slot 10 writes through the RDX return pointer");
            check(cpu_pc.ptr == cpu.ptr, "proxy CPU handle equals the PC heap's hidden-pointer result");
            check(gpu_pc.ptr == gpu.ptr, "proxy GPU handle equals the PC heap's hidden-pointer result");
        }

        const D3D12_DESCRIPTOR_HEAP_DESC got_desc = heap->GetDesc();
        check(got_desc.NumDescriptors == heap_desc.NumDescriptors, "heap slot 8 forwards GetDesc");
        check(rt::d3d12_xs::heap_slot_hits(9) >= 1, "heap slot 9 hit counter moves");
        check(rt::d3d12_xs::heap_slot_hits(10) >= 1, "heap slot 10 hit counter moves");
        check(rt::d3d12_xs::heap_slot_first_call_seen(9), "heap slot 9 first-call flag is set");
    }

    // Slot 9 on the device must remain CreateCommandAllocator, not a handle getter.
    ComPtr<ID3D12CommandAllocator> allocator;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf()));
    check(SUCCEEDED(hr) && allocator != nullptr, "device slot 9 forwards CreateCommandAllocator");
    // A PC runtime allocator is not wrapped: the PC runtime's lists could not resolve an allocator proxy.
    check(!rt::d3d12_xs::is_allocator_proxy(allocator.Get()), "slot 9 hands out the PC allocator unwrapped");
    check(rt::d3d12_xs::device_slot_hits(9) == 1, "slot 9 hit counter is 1");

    // Slot 76: buffer resource with the measured Xbox layout word 0x101; PC committed resources take ROW_MAJOR.
    const IID iid_resource = __uuidof(ID3D12Resource);
    auto create_resource = reinterpret_cast<XboxCreateResourceFn>(proxy_vtbl[76]);
    const D3D12_HEAP_PROPERTIES heap_properties = default_heap();
    D3D12_RESOURCE_DESC xbox_buffer = buffer_desc();
    ComPtr<ID3D12Resource> buffer;
    hr = create_resource(device.Get(), &heap_properties, D3D12_HEAP_FLAG_NONE, &xbox_buffer,
                         D3D12_RESOURCE_STATE_COMMON, nullptr, iid_resource,
                         reinterpret_cast<void**>(buffer.GetAddressOf()));
    check(SUCCEEDED(hr) && buffer != nullptr, "slot 76 creates a committed buffer");
    if (buffer) {
        const D3D12_RESOURCE_DESC got = buffer->GetDesc();
        check(got.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR, "slot 76 maps 0x101 to ROW_MAJOR for a buffer");
        check(got.Width == 4096, "slot 76 preserves the resource width");
    }

    // The same Xbox word on a texture maps to UNKNOWN, which is what PC committed textures require.
    D3D12_RESOURCE_DESC xbox_texture = texture_desc();
    ComPtr<ID3D12Resource> texture;
    hr = create_resource(device.Get(), &heap_properties, D3D12_HEAP_FLAG_NONE, &xbox_texture,
                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr, iid_resource,
                         reinterpret_cast<void**>(texture.GetAddressOf()));
    check(SUCCEEDED(hr) && texture != nullptr, "slot 76 creates a committed texture");
    if (texture) {
        check(texture->GetDesc().Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN,
              "slot 76 maps 0x101 to UNKNOWN for a texture");
    }
    check(rt::d3d12_xs::device_slot_hits(76) == 2, "slot 76 hit counter is 2");
    check(rt::d3d12_xs::device_slot_first_call_seen(76), "slot 76 first-call flag is set");

    // Xbox device slots from the public HLE. On the Vulkan device the PC entries at 78/79 are
    // CreateReservedResource2/CreateSampler2 and 80 is past the 80-slot table, so success here can only come from
    // the routes: frame events return S_OK and a fresh token, CreateCommandQueueX returns a queue proxy and
    // CreateCommandListX (desc, allocator, initialState, riid, out; no node mask) reaches PC CreateCommandList.
    {
        using SetFrameIntervalFn = HRESULT(WINAPI*)(void*, void*, UINT, UINT, UINT);
        using ScheduleFrameEventFn = HRESULT(WINAPI*)(void*, UINT, UINT, void*, UINT);
        using WaitFrameEventFn = HRESULT(WINAPI*)(void*, UINT, UINT, void*, UINT, UINT64*);
        using CreateQueueXFn = HRESULT(WINAPI*)(void*, const D3D12_COMMAND_QUEUE_DESC*, const IID*, void**);
        using CreateListXFn = HRESULT(WINAPI*)(void*, const void*, ID3D12CommandAllocator*, ID3D12PipelineState*,
                                               const IID*, void**);

        hr = reinterpret_cast<SetFrameIntervalFn>(proxy_vtbl[78])(device.Get(), nullptr, 16667, 1, 0);
        check(hr == S_OK, "slot 78 SetFrameIntervalX returns S_OK");
        hr = reinterpret_cast<ScheduleFrameEventFn>(proxy_vtbl[79])(device.Get(), 0, 0, nullptr, 0);
        check(hr == S_OK, "slot 79 ScheduleFrameEventX returns S_OK");

        std::uint64_t token = 0, token2 = 0;
        hr = reinterpret_cast<WaitFrameEventFn>(proxy_vtbl[80])(device.Get(), 0, INFINITE, nullptr, 0, &token);
        check(hr == S_OK && token != 0, "slot 80 WaitFrameEventX returns a non-zero token");
        hr = reinterpret_cast<WaitFrameEventFn>(proxy_vtbl[80])(device.Get(), 0, 0, nullptr, 0, &token2);
        check(hr == S_OK && token2 != 0 && token2 != token, "slot 80 returns a fresh token on the next wait");
        hr = reinterpret_cast<WaitFrameEventFn>(proxy_vtbl[80])(device.Get(), 0, 0, nullptr, 0, nullptr);
        check(hr == E_POINTER, "slot 80 refuses a null token with E_POINTER");
        check(rt::d3d12_xs::device_slot_hits(78) == 1 && rt::d3d12_xs::device_slot_hits(79) == 1 &&
                  rt::d3d12_xs::device_slot_hits(80) == 3,
              "slots 78/79/80 hit counters are 1/1/3");

        D3D12_COMMAND_QUEUE_DESC xbox_queue{};
        xbox_queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        const IID iid_queue = __uuidof(ID3D12CommandQueue);
        ComPtr<ID3D12CommandQueue> xbox_queue_proxy;
        hr = reinterpret_cast<CreateQueueXFn>(proxy_vtbl[48])(device.Get(), &xbox_queue, &iid_queue,
                                                               reinterpret_cast<void**>(xbox_queue_proxy.GetAddressOf()));
        check(hr == S_OK && rt::d3d12_xs::is_queue_proxy(xbox_queue_proxy.Get()),
              "slot 48 CreateCommandQueueX returns a queue proxy");
        check(rt::d3d12_xs::device_slot_hits(48) == 1, "slot 48 hit counter is 1");

        ComPtr<ID3D12CommandAllocator> xbox_allocator;
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(xbox_allocator.GetAddressOf()));
        struct XboxCommandListDesc {
            UINT Type;
            UINT NodeMask;
        } list_desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 7};  // the node mask is ignored by the console call
        // The measured console IID the title passes; the console-shaped proxy is held as IUnknown, never as a PC
        // graphics list (its table is shifted from the PC one).
        const IID iid_list = rt::d3d12_xs::kCommandListIidPairs[0].console;
        ComPtr<IUnknown> xbox_list;
        hr = SUCCEEDED(hr) ? reinterpret_cast<CreateListXFn>(proxy_vtbl[73])(
                                 device.Get(), &list_desc, xbox_allocator.Get(), nullptr, &iid_list,
                                 reinterpret_cast<void**>(xbox_list.GetAddressOf()))
                           : hr;
        check(hr == S_OK && xbox_list != nullptr, "slot 73 CreateCommandListX returns a command list");
        check(rt::d3d12_xs::is_list_proxy(xbox_list.Get()) &&
                  rt::d3d12_xs::list_proxy_real_list(xbox_list.Get()) != nullptr,
              "slot 73 returns the console-shaped list proxy over a real PC list");
        check(rt::d3d12_xs::device_slot_hits(73) == 1, "slot 73 hit counter is 1");
        if (xbox_list) {
            void* as_pc = reinterpret_cast<void*>(1);
            hr = xbox_list->QueryInterface(__uuidof(ID3D12GraphicsCommandList), &as_pc);
            check(hr == E_NOINTERFACE && as_pc == nullptr,
                  "the console list refuses ID3D12GraphicsCommandList and clears the output");
            void* as_base = nullptr;
            hr = xbox_list->QueryInterface(__uuidof(ID3D12CommandList), &as_base);
            check(hr == S_OK && as_base == xbox_list.Get(), "the console list answers ID3D12CommandList");
            if (as_base) static_cast<IUnknown*>(as_base)->Release();
        }
    }

    // Slot 62: an empty root signature and a trivial compute shader through the Xbox signature. The PC entry at
    // slot 62 is CreateStateObject, so success here proves the override reached CreateComputePipelineState.
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> root_blob;
    ComPtr<ID3DBlob> root_error;
    hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, root_blob.GetAddressOf(),
                                     root_error.GetAddressOf());
    check(SUCCEEDED(hr) && root_blob != nullptr, "empty root signature serializes");
    ComPtr<ID3D12RootSignature> root_signature;
    if (root_blob) {
        hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                         IID_PPV_ARGS(root_signature.GetAddressOf()));
    }
    check(SUCCEEDED(hr) && root_signature != nullptr, "device slot 16 forwards CreateRootSignature");

    ComPtr<ID3DBlob> bytecode;
    if (load_compute_shader(bytecode) && root_signature) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root_signature.Get();
        pso_desc.CS.pShaderBytecode = bytecode->GetBufferPointer();
        pso_desc.CS.BytecodeLength = bytecode->GetBufferSize();
        pso_desc.NodeMask = 0;
        pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

        auto create_pso = reinterpret_cast<XboxCreateComputePsoFn>(proxy_vtbl[62]);
        const IID iid_pso = __uuidof(ID3D12PipelineState);
        ComPtr<ID3D12PipelineState> pso;
        hr = create_pso(device.Get(), &pso_desc, 0, 0, iid_pso, reinterpret_cast<void**>(pso.GetAddressOf()));
        check(SUCCEEDED(hr) && pso != nullptr, "slot 62 creates a compute PSO via CreateComputePipelineState");
        check(rt::d3d12_xs::device_slot_hits(62) == 1, "slot 62 hit counter is 1");
        check(rt::d3d12_xs::device_slot_first_call_seen(62), "slot 62 first-call flag is set");

        // Slot 67: Xbox native compute pipeline creation. Maps the console descriptor (+0x00 root signature,
        // +0x28 kind 6 code blob, +0x30 code size) to D3D12_COMPUTE_PIPELINE_STATE_DESC and calls CreateComputePipelineState.
        struct XboxNativeComputeDesc {
            ID3D12RootSignature* root_signature;
            const void* kind0;
            std::uint64_t kind0_size;
            const void* kind1;
            std::uint64_t kind1_size;
            const void* kind6;
            std::uint64_t kind6_size;
            const void* kind7;
            std::uint64_t kind7_size;
        } xbox_desc{};
        xbox_desc.root_signature = root_signature.Get();
        xbox_desc.kind6 = bytecode->GetBufferPointer();
        xbox_desc.kind6_size = bytecode->GetBufferSize();

        using XboxCreateNativeComputePsoFn = HRESULT(WINAPI*)(void*, const void*, const IID*, void**);
        auto create_native_pso = reinterpret_cast<XboxCreateNativeComputePsoFn>(proxy_vtbl[67]);
        ComPtr<ID3D12PipelineState> pso67;
        hr = create_native_pso(device.Get(), &xbox_desc, &iid_pso, reinterpret_cast<void**>(pso67.GetAddressOf()));
        check(SUCCEEDED(hr) && pso67 != nullptr, "slot 67 creates a compute PSO via CreateComputePipelineState");
        check(rt::d3d12_xs::device_slot_hits(67) == 1, "slot 67 hit counter is 1");
        check(rt::d3d12_xs::device_slot_first_call_seen(67), "slot 67 first-call flag is set");
    }

    // _COM_Outptr_ (d3d12.h): a failed create leaves NULL in the output. WARP accepts invalid heap descriptions,
    // so the PC-failure case for slot 14 is in d3d12_vulkan_device_test; these are the proxy's own early returns.
    {
        void* proxy_out = reinterpret_cast<void*>(0x1111);
        using Slot14Fn = HRESULT(WINAPI*)(void*, const D3D12_DESCRIPTOR_HEAP_DESC*, const IID*, void**);
        hr = reinterpret_cast<Slot14Fn>(proxy_vtbl[14])(device.Get(), &heap_desc, nullptr, &proxy_out);
        check(FAILED(hr) && proxy_out == nullptr, "slot 14 nulls its output for a null IID");
        using Slot62Fn = HRESULT(WINAPI*)(void*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, std::uint64_t,
                                          std::uint64_t, const IID*, void**);
        proxy_out = reinterpret_cast<void*>(0x1111);
        D3D12_COMPUTE_PIPELINE_STATE_DESC empty_pso{};
        hr = reinterpret_cast<Slot62Fn>(proxy_vtbl[62])(device.Get(), &empty_pso, 0, 0, nullptr, &proxy_out);
        check(FAILED(hr) && proxy_out == nullptr, "slot 62 nulls its output for a null IID");
        using Slot67Fn = HRESULT(WINAPI*)(void*, const void*, const IID*, void**);
        proxy_out = reinterpret_cast<void*>(0x1111);
        const IID iid_pso = __uuidof(ID3D12PipelineState);
        hr = reinterpret_cast<Slot67Fn>(proxy_vtbl[67])(device.Get(), nullptr, &iid_pso, &proxy_out);
        check(FAILED(hr) && proxy_out == nullptr, "slot 67 nulls its output for a null description");
        proxy_out = reinterpret_cast<void*>(0x1111);
        hr = reinterpret_cast<Slot67Fn>(proxy_vtbl[67])(device.Get(), &root_desc, nullptr, &proxy_out);
        check(FAILED(hr) && proxy_out == nullptr, "slot 67 nulls its output for a null IID");
        using Slot76Fn = HRESULT(WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                          const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                          const D3D12_CLEAR_VALUE*, const IID*, void**);
        proxy_out = reinterpret_cast<void*>(0x1111);
        hr = reinterpret_cast<Slot76Fn>(proxy_vtbl[76])(device.Get(), &heap_properties, D3D12_HEAP_FLAG_NONE,
                                                         nullptr, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         &iid_resource, &proxy_out);
        check(FAILED(hr) && proxy_out == nullptr, "slot 76 nulls its output for a null description");
    }

    // Slots 8 and 36 on the device the shim made: a title gets queue and fence proxies, and they run GPU work.
    {
        using CreateQueueFn = HRESULT(WINAPI*)(void*, const D3D12_COMMAND_QUEUE_DESC*, const IID*, void**);
        using CreateFenceFn = HRESULT(WINAPI*)(void*, UINT64, D3D12_FENCE_FLAGS, const IID*, void**);
        void** vtable = *reinterpret_cast<void***>(device.Get());
        const D3D12_COMMAND_QUEUE_DESC queue_desc{};
        const IID iid_queue = __uuidof(ID3D12CommandQueue);
        const IID iid_fence = __uuidof(ID3D12Fence);
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Fence> fence;
        hr = reinterpret_cast<CreateQueueFn>(vtable[8])(device.Get(), &queue_desc, &iid_queue,
                                                        reinterpret_cast<void**>(queue.GetAddressOf()));
        check(SUCCEEDED(hr) && rt::d3d12_xs::is_queue_proxy(queue.Get()), "slot 8 returns a queue proxy");
        hr = reinterpret_cast<CreateFenceFn>(vtable[36])(device.Get(), 0, D3D12_FENCE_FLAG_NONE, &iid_fence,
                                                         reinterpret_cast<void**>(fence.GetAddressOf()));
        check(SUCCEEDED(hr) && rt::d3d12_xs::is_fence_proxy(fence.Get()), "slot 36 returns a fence proxy");
        if (queue && fence) {
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            check(event && SUCCEEDED(queue->Signal(fence.Get(), 1)) &&
                      SUCCEEDED(fence->SetEventOnCompletion(1, event)) && WaitForSingleObject(event, 5000) == 0,
                  "the queue proxy signals the fence proxy on the GPU");
            if (event) CloseHandle(event);
            ComPtr<ID3D12Device> owner;
            check(SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(owner.GetAddressOf()))) && owner.Get() == device.Get(),
                  "the queue proxy's GetDevice is the device the shim returned");
        }
    }

    // Title-backed descriptor heaps on WARP (upstream PRs #7021 and #7880, adapted). The device came from
    // D3D12XboxCreateDevice, so slot 14 consults the registry's hooks; this executable plays the title image.
    check_hook_registration();
    {
        using T = rt::d3d12_xs::DescriptorHandleTranslation;
        using GetCpuOutFn = void(WINAPI*)(ID3D12DescriptorHeap*, D3D12_CPU_DESCRIPTOR_HANDLE*);
        using GetGpuOutFn = void(WINAPI*)(ID3D12DescriptorHeap*, D3D12_GPU_DESCRIPTOR_HANDLE*);
        const void* image = nullptr;
        std::uint64_t image_size = 0;
        this_image(image, image_size);
        g_title_hooks = {&title_alloc, &title_free};
        check(rt::shims::add_xmem_allocation_hooks(&g_title, image, image_size, &g_title_hooks),
              "the synthetic title image registers its hooks");
        const UINT cbv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_DESCRIPTOR_HEAP_DESC cbv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};

        // Failures: a hook without memory and an invalid struct both fail slot 14 with a null output.
        ComPtr<ID3D12DescriptorHeap> refused;
        hand_out(nullptr, 0);
        hr = device->CreateDescriptorHeap(&cbv_desc, IID_PPV_ARGS(refused.GetAddressOf()));
        check(hr == E_OUTOFMEMORY && !refused && g_title.allocations == 1,
              "slot 14 fails with E_OUTOFMEMORY when the title's hook returns no memory");
        g_title_hooks.free = nullptr;
        hr = device->CreateDescriptorHeap(&cbv_desc, IID_PPV_ARGS(refused.GetAddressOf()));
        check(hr == E_FAIL && !refused && g_title.allocations == 1, "slot 14 fails for half-filled hooks");
        g_title_hooks.free = &title_free;

        // A title-backed heap: the hook is asked for NumDescriptors * increment with 0x00800004.
        hand_out(g_title.cbv, sizeof(g_title.cbv));
        ComPtr<ID3D12DescriptorHeap> hooked;
        hr = device->CreateDescriptorHeap(&cbv_desc, IID_PPV_ARGS(hooked.GetAddressOf()));
        check(SUCCEEDED(hr) && hooked && g_title.allocations == 2 && g_title.last_size == 4u * cbv_inc &&
                  g_title.last_attributes == 0x00800004,
              "slot 14 asks __XMemAllocationHooks__ for the heap's bytes with the D3D descriptor-heap attributes");
        if (hooked) {
            void** vtbl = *reinterpret_cast<void***>(hooked.Get());
            ID3D12DescriptorHeap* real_hooked = rt::d3d12_xs::heap_proxy_real_heap(hooked.Get());
            void** real_vtbl_h = *reinterpret_cast<void***>(real_hooked);
            D3D12_CPU_DESCRIPTOR_HANDLE real_cpu{};
            D3D12_GPU_DESCRIPTOR_HANDLE real_gpu{};
            reinterpret_cast<GetCpuOutFn>(real_vtbl_h[9])(real_hooked, &real_cpu);
            reinterpret_cast<GetGpuOutFn>(real_vtbl_h[10])(real_hooked, &real_gpu);
            const auto base = reinterpret_cast<SIZE_T>(g_title.cbv);
            check(reinterpret_cast<GetCpuHandleFn>(vtbl[9])(hooked.Get()).ptr == base,
                  "heap slot 9 returns the title's backing address");
            check(reinterpret_cast<GetGpuHandleFn>(vtbl[10])(hooked.Get()).ptr == real_gpu.ptr && real_gpu.ptr,
                  "heap slot 10 keeps the genuine GPU start");
            D3D12_CPU_DESCRIPTOR_HANDLE out{};
            check(rt::d3d12_xs::translate_cpu_descriptor_range(device.Get(), {base + cbv_inc}, 3,
                                                                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                                &out) == T::translated &&
                      out.ptr == real_cpu.ptr + cbv_inc,
                  "a title handle translates to the real heap at the same offset");
            check(rt::d3d12_xs::translate_cpu_descriptor_range(device.Get(), {base + cbv_inc}, 4,
                                                                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                                &out) == T::invalid,
                  "a span past the title heap's end is invalid");
            // The plain heap made before the hooks keeps its genuine handles.
            if (heap) {
                const SIZE_T plain = reinterpret_cast<GetCpuHandleFn>((*reinterpret_cast<void***>(heap.Get()))[9])(
                                         heap.Get()).ptr;
                check(rt::d3d12_xs::translate_cpu_descriptor_range(device.Get(), {plain}, 1,
                                                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                                    &out) == T::unchanged && out.ptr == plain,
                      "a normal PC heap handle stays valid and unchanged");
                const std::uint64_t refused_before = rt::d3d12_xs::refused_descriptor_handle_calls();
                D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
                cbv.SizeInBytes = 256;
                device->CreateConstantBufferView(&cbv, {plain});
                device->CreateConstantBufferView(&cbv, {base + cbv_inc});  // translated, WARP writes the real heap
                check(rt::d3d12_xs::refused_descriptor_handle_calls() == refused_before,
                      "slot 17 forwards plain and title handles on WARP");
                device->CreateConstantBufferView(&cbv, {base + 1});            // misaligned custom handle
                device->CreateConstantBufferView(&cbv, {base + cbv_inc / 2});  // between descriptors
                check(rt::d3d12_xs::refused_descriptor_handle_calls() == refused_before + 2,
                      "slot 17 refuses invalid title pointers instead of handing them to WARP");
            }
        }
        const int frees = g_title.frees;
        hooked.Reset();
        check(g_title.frees == frees + 1 && g_title.last_freed == g_title.cbv &&
                  g_title.last_free_attributes == 0x00800004,
              "the last Release frees the backing once through the title's free hook");

        // A synthetic GPU check through the console list: title-backed RTV and DSV heaps, OMSetRenderTargets (50),
        // ClearRenderTargetView (52) with (1, 0, 128/255, 1) and ClearDepthStencilView (51) with Depth 0.25, then a
        // copy to a readback buffer. This exercises the translation and the typed slot 51 on WARP; it is not a
        // title frame.
        ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
        hand_out(g_title.rtv, sizeof(g_title.rtv));
        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        HRESULT hr_rtv = device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(rtv_heap.GetAddressOf()));
        hand_out(g_title.dsv, sizeof(g_title.dsv));
        D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        HRESULT hr_dsv = device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(dsv_heap.GetAddressOf()));
        check(SUCCEEDED(hr_rtv) && SUCCEEDED(hr_dsv) && rtv_heap && dsv_heap, "title-backed RTV and DSV heaps");

        D3D12_HEAP_PROPERTIES readback_heap = default_heap();
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rt_desc = texture_desc();
        rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_RESOURCE_DESC ds_desc = rt_desc;
        ds_desc.Format = DXGI_FORMAT_D32_FLOAT;
        ds_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_RESOURCE_DESC rb_desc = buffer_desc();
        rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> rt_tex, ds_tex, rb_buf;
        const HRESULT hr_rt = device->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                                              D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                              IID_PPV_ARGS(rt_tex.GetAddressOf()));
        const HRESULT hr_ds = device->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_NONE, &ds_desc,
                                                              D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr,
                                                              IID_PPV_ARGS(ds_tex.GetAddressOf()));
        const HRESULT hr_rb = device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
                                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                              IID_PPV_ARGS(rb_buf.GetAddressOf()));
        check(SUCCEEDED(hr_rt) && SUCCEEDED(hr_ds) && SUCCEEDED(hr_rb), "render target, depth and readback resources");

        if (rtv_heap && dsv_heap && rt_tex && ds_tex && rb_buf) {
            const UINT rtv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            const UINT dsv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            const auto rtv_base = reinterpret_cast<GetCpuHandleFn>((*reinterpret_cast<void***>(rtv_heap.Get()))[9])(
                rtv_heap.Get());
            const auto dsv_base = reinterpret_cast<GetCpuHandleFn>((*reinterpret_cast<void***>(dsv_heap.Get()))[9])(
                dsv_heap.Get());
            check(rtv_base.ptr == reinterpret_cast<SIZE_T>(g_title.rtv) &&
                      dsv_base.ptr == reinterpret_cast<SIZE_T>(g_title.dsv),
                  "RTV and DSV heaps report the title's addresses");
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtv_base.ptr + rtv_inc};
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv{dsv_base.ptr + dsv_inc};
            device->CreateRenderTargetView(rt_tex.Get(), nullptr, rtv);  // slot 20, translated
            device->CreateDepthStencilView(ds_tex.Get(), nullptr, dsv);  // slot 21, translated

            using CreateQueueFn = HRESULT(WINAPI*)(void*, const D3D12_COMMAND_QUEUE_DESC*, const IID*, void**);
            using CreateListXFn = HRESULT(WINAPI*)(void*, const void*, ID3D12CommandAllocator*, ID3D12PipelineState*,
                                                   const IID*, void**);
            using CloseFn = HRESULT(WINAPI*)(void*);
            using BarrierFn = void(WINAPI*)(void*, UINT, const D3D12_RESOURCE_BARRIER*);
            using CopyTextureFn = void(WINAPI*)(void*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
                                                const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
            using OmSetFn = void(WINAPI*)(void*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                          const D3D12_CPU_DESCRIPTOR_HANDLE*);
            using ClearRtvFn = void(WINAPI*)(void*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*);
            using ClearDsvFn = void(WINAPI*)(void*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT,
                                             const D3D12_RECT*);
            const D3D12_COMMAND_QUEUE_DESC queue_desc{};
            const IID iid_queue = __uuidof(ID3D12CommandQueue);
            ComPtr<ID3D12CommandQueue> queue;
            hr = reinterpret_cast<CreateQueueFn>(proxy_vtbl[8])(device.Get(), &queue_desc, &iid_queue,
                                                                reinterpret_cast<void**>(queue.GetAddressOf()));
            ComPtr<ID3D12CommandAllocator> list_allocator;
            const HRESULT hr_alloc = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                    IID_PPV_ARGS(list_allocator.GetAddressOf()));
            struct {
                UINT Type;
                UINT NodeMask;
            } list_desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 0};
            const IID iid_list = rt::d3d12_xs::kCommandListIidPairs[0].console;
            ComPtr<IUnknown> list;
            const HRESULT hr_list = SUCCEEDED(hr_alloc)
                                        ? reinterpret_cast<CreateListXFn>(proxy_vtbl[73])(
                                              device.Get(), &list_desc, list_allocator.Get(), nullptr, &iid_list,
                                              reinterpret_cast<void**>(list.GetAddressOf()))
                                        : hr_alloc;
            ComPtr<ID3D12Fence> fence;
            const HRESULT hr_fence = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
            check(SUCCEEDED(hr) && SUCCEEDED(hr_list) && SUCCEEDED(hr_fence) && queue && list && fence &&
                      rt::d3d12_xs::is_list_proxy(list.Get()),
                  "queue, console list proxy and fence for the clear check");

            if (queue && list && fence) {
                void** lv = *reinterpret_cast<void***>(list.Get());
                const FLOAT color[4] = {1.0f, 0.0f, 128.0f / 255.0f, 1.0f};
                reinterpret_cast<OmSetFn>(lv[50])(list.Get(), 1, &rtv, FALSE, &dsv);
                reinterpret_cast<ClearRtvFn>(lv[52])(list.Get(), rtv, color, 0, nullptr);
                reinterpret_cast<ClearDsvFn>(lv[51])(list.Get(), dsv, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 0, nullptr);
                D3D12_RESOURCE_BARRIER barriers[2]{};
                barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[0].Transition = {rt_tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
                barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[1].Transition = {ds_tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                          D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE};
                reinterpret_cast<BarrierFn>(lv[26])(list.Get(), 2, barriers);
                D3D12_TEXTURE_COPY_LOCATION rt_dst{}, rt_src{}, ds_dst{}, ds_src{};
                rt_dst.pResource = rb_buf.Get();
                rt_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                rt_dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 256}};
                rt_src.pResource = rt_tex.Get();
                rt_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                ds_dst.pResource = rb_buf.Get();
                ds_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                ds_dst.PlacedFootprint = {1536, {DXGI_FORMAT_R32_TYPELESS, 4, 4, 1, 256}};
                ds_src.pResource = ds_tex.Get();
                ds_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                reinterpret_cast<CopyTextureFn>(lv[16])(list.Get(), &rt_dst, 0, 0, 0, &rt_src, nullptr);
                reinterpret_cast<CopyTextureFn>(lv[16])(list.Get(), &ds_dst, 0, 0, 0, &ds_src, nullptr);
                check(rt::d3d12_xs::list_proxy_unsupported_slot(list.Get()) == 0 &&
                          reinterpret_cast<CloseFn>(lv[9])(list.Get()) == S_OK,
                      "the console list records the title-handle clears and closes");

                ID3D12CommandList* const lists[] = {static_cast<ID3D12CommandList*>(static_cast<void*>(list.Get()))};
                queue->ExecuteCommandLists(1, lists);
                HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                const bool done = event && SUCCEEDED(queue->Signal(fence.Get(), 1)) &&
                                  SUCCEEDED(fence->SetEventOnCompletion(1, event)) &&
                                  WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
                if (event) CloseHandle(event);
                check(done, "the clear commands complete on WARP");
                void* mapped = nullptr;
                const D3D12_RANGE read{0, 4096};
                if (done && SUCCEEDED(rb_buf->Map(0, &read, &mapped)) && mapped) {
                    const auto* bytes = static_cast<const std::uint8_t*>(mapped);
                    float depth = -1.0f;
                    std::memcpy(&depth, bytes + 1536, sizeof(depth));
                    std::printf("readback RGBA %u %u %u %u, depth %.7f\n", bytes[0], bytes[1], bytes[2], bytes[3],
                                static_cast<double>(depth));
                    check(bytes[0] == 255 && bytes[1] == 0 && bytes[2] == 128 && bytes[3] == 255,
                          "console slot 52 clears the title-handle RTV to (255, 0, 128, 255)");
                    check(depth == 0.25f, "console slot 51 clears the title-handle DSV to Depth 0.25 (XMM3 kept)");
                    const D3D12_RANGE written{0, 0};
                    rb_buf->Unmap(0, &written);
                } else {
                    check(false, "the readback buffer maps");
                }
            }
        }
        const int frees_before_heaps = g_title.frees;
        rtv_heap.Reset();
        dsv_heap.Reset();
        check(g_title.frees == frees_before_heaps + 2, "both clear-check heaps free their title backing once");
        rt::shims::remove_xmem_allocation_hooks(&g_title);
        check(rt::d3d12_xs::title_descriptor_range_count() == 0, "no title range outlives its heap");
    }

    // Hold a reference to the PC device so the final check can prove the proxy's Release balances its own.
    if (real) real->AddRef();
    // Drop every device child before the proxy so the PC device is not released first.
    root_signature.Reset();
    reserved.Reset();
    texture.Reset();
    buffer.Reset();
    allocator.Reset();
    device1.Reset();
    const unsigned long refs_before_heap_release = rt::d3d12_xs::device_proxy_ref_count(device.Get());
    heap.Reset();
    check(rt::d3d12_xs::device_proxy_ref_count(device.Get()) + 1 == refs_before_heap_release,
          "heap release drops its device reference");
    device.Reset();
    if (real) {
        check(real->GetNodeCount() > 0, "PC device survives the proxy's final Release");
        real->Release();
    }

    std::printf("device_proxy_test: %s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
