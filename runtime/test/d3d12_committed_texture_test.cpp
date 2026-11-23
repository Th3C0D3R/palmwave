// Device slot 127's opt-in committed-texture EXPERIMENT (PALMWAVE_EXPERIMENTAL_XBOX_COMMITTED_TEXTURE) on the real
// Vulkan D3D12 backend, through the Xbox-ABI device proxy. A hypothesis under test, not a console ABI: this checks only
// that the one host texture the experiment creates behaves as a PC RT|UAV R8G8B8A8_UNORM 3840x2160 texture for
// same-image render-target, copy, UAV and SRV work. It cannot establish what 0x00300000 means, the console's DCC,
// metadata, tiling or size, or anything about a frame.
//
//   1. Strict refusal first: the variable unset, the measured tuple through raw slot 127 is the backend's E_INVALIDARG
//      with a null output; with the variable "1", a one-field near miss (Width 3839) is the same refusal.
//   2. The variable "1" and the exact tuple: S_OK. GetDesc reports Flags 0x5 and the title's dims/format;
//      GetHeapProperties reports DEFAULT with ALLOW_DISPLAY kept (plus the backend's implied DENY bits).
//   3. One command list, the texture's first command an enhanced barrier from UNORDERED_ACCESS (the initial layout word
//      3, kept) to RENDER_TARGET; an RTV clear to (51, 102, 204, 255)/255; RENDER_TARGET -> COPY_SOURCE; rows 0, 1080
//      and 2159 (all four corners) copied to a readback buffer; COPY_SOURCE -> UNORDERED_ACCESS; a GLSL compute fixture
//      writes a coordinate pattern into five 64x64 patches (four corners, centre) through the texture's own UAV;
//      UNORDERED_ACCESS -> SHADER_RESOURCE; a second GLSL fixture reads six patches (the five plus one only the clear
//      touched) through the texture's own SRV into a buffer; the buffer is copied to the readback buffer.
//   4. Fence wait, then every texel of the three rows is the clear and every texel of the six patches is its exact
//      oracle (pattern alpha 0x5A, clear alpha 0xFF, so neither can pass for the other).
// The views are made with CreateUnorderedAccessView/CreateShaderResourceView; the fixtures find them only through the
// T# words those calls wrote into the shader-visible heap and the layer's set-0 hash table (a miss is the fallback
// image, which fails the oracle). Nothing writes expected pixels from the CPU, and no Vulkan object is touched.
// The fixtures are hand-written GLSL to the translator's contract (test/glsl/cs_committed_texture_{write,read}.comp),
// not translated RDNA2. Synthetic data only; no game data. Set PALMWAVE_VK_VALIDATION=1 for VK_LAYER_KHRONOS_validation
// (0 errors required). CTest unsets the variable; the test sets and restores it itself (it is read per call).
#include "shader_fixture_path.h"
#include "test_temp_dir.h"
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/shader_library.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#if !defined(PALMWAVE_TEST_CS_COMMITTED_TEXTURE_WRITE_SPV) || !defined(PALMWAVE_TEST_CS_COMMITTED_TEXTURE_READ_SPV)
#error "the committed-texture GLSL fixtures must be compiled and named"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;

namespace {

constexpr char kVariable[] = "PALMWAVE_EXPERIMENTAL_XBOX_COMMITTED_TEXTURE";
constexpr UINT kWidth = 3840, kHeight = 2160;
constexpr UINT kRowBytes = kWidth * 4;  // 15360: a multiple of 256 (pitch) and of 512 (placement)
constexpr UINT kRows[3] = {0, 1080, kHeight - 1};
constexpr UINT kPatch = 64, kPatchTexels = kPatch * kPatch, kPatches = 6, kWrittenPatches = 5;
constexpr UINT kPatchOrigin[kPatches][2] = {{0, 0}, {kWidth - kPatch, 0}, {0, kHeight - kPatch},
                                            {kWidth - kPatch, kHeight - kPatch}, {1888, 1048}, {1024, 512}};
constexpr UINT64 kRowsBytes = UINT64(kRowBytes) * 3;
constexpr UINT64 kOutputOffset = 49152;  // past the rows, 512-aligned
constexpr UINT64 kOutputBytes = UINT64(kPatches) * kPatchTexels * 4;
constexpr std::uint8_t kClear[4] = {51, 102, 204, 255};

using Texel = std::array<std::uint8_t, 4>;

Texel pattern(UINT x, UINT y) {
    return {std::uint8_t(x & 0xFF), std::uint8_t(y & 0xFF), std::uint8_t((x >> 8) | ((y >> 8) << 4)), 0x5A};
}

// Device slot 127 with exactly ten words after `this`, as the title calls it.
using Slot127Fn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                         std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                         std::uint64_t);

struct Request {
    D3D12_HEAP_PROPERTIES heap{};
    D3D12_RESOURCE_DESC1 desc{};
    D3D12_CLEAR_VALUE clear{};
};

Request measured_request() {
    Request r;
    r.heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    r.heap.CreationNodeMask = 1;
    r.heap.VisibleNodeMask = 1;
    r.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    r.desc.Width = kWidth;
    r.desc.Height = kHeight;
    r.desc.DepthOrArraySize = 1;
    r.desc.MipLevels = 1;
    r.desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // 28
    r.desc.SampleDesc = {1, 0};
    r.desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    r.desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x00300005);
    r.clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // colour all zero bits
    return r;
}

HRESULT call127(void* proxy, Request& r, ID3D12Resource** out) {
    static const IID resource_iid = __uuidof(ID3D12Resource);
    const auto fn = reinterpret_cast<Slot127Fn>(slot(proxy, 127));
    const std::uint64_t result =
        fn(proxy, reinterpret_cast<std::uint64_t>(&r.heap), std::uint64_t(D3D12_HEAP_FLAG_ALLOW_DISPLAY),
           reinterpret_cast<std::uint64_t>(&r.desc), 3 /* initial layout word, kept */,
           reinterpret_cast<std::uint64_t>(&r.clear), 0, 0, 0, reinterpret_cast<std::uint64_t>(&resource_iid),
           reinterpret_cast<std::uint64_t>(out));
    return static_cast<HRESULT>(static_cast<std::uint32_t>(result));
}

// The process's value of the variable, given back on every return path.
class EnvRestore {
public:
    EnvRestore() {
        SetLastError(ERROR_SUCCESS);
        const DWORD n = GetEnvironmentVariableA(kVariable, value_, sizeof(value_));
        present_ = n != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    }
    ~EnvRestore() { SetEnvironmentVariableA(kVariable, present_ ? value_ : nullptr); }
    EnvRestore(const EnvRestore&) = delete;
    EnvRestore& operator=(const EnvRestore&) = delete;

private:
    char value_[64] = {};
    bool present_ = false;
};

D3D12_TEXTURE_BARRIER texture_barrier(ID3D12Resource* resource, D3D12_BARRIER_SYNC sync_before,
                                      D3D12_BARRIER_SYNC sync_after, D3D12_BARRIER_ACCESS access_before,
                                      D3D12_BARRIER_ACCESS access_after, D3D12_BARRIER_LAYOUT layout_before,
                                      D3D12_BARRIER_LAYOUT layout_after) {
    D3D12_TEXTURE_BARRIER b{};
    b.SyncBefore = sync_before;
    b.SyncAfter = sync_after;
    b.AccessBefore = access_before;
    b.AccessAfter = access_after;
    b.LayoutBefore = layout_before;
    b.LayoutAfter = layout_after;
    b.pResource = resource;
    b.Subresources = {0xffffffff, 0, 0, 0, 0, 0};
    b.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
    return b;
}

void barrier(ID3D12GraphicsCommandList7* list, const D3D12_TEXTURE_BARRIER& b) {
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 1;
    group.pTextureBarriers = &b;
    list->Barrier(1, &group);
}

}  // namespace

int main() {
    vktest::Device d;
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) return 77;  // no Vulkan GPU: ctest skips
    if (device_status != vktest::DeviceStatus::ready) return 1;
    if (!d.bindless) {
        std::printf("SKIP: the device lacks the descriptor indexing features of the bindless set 0\n");
        return 77;
    }
    {
        VkPhysicalDeviceProperties props{};
        d.instance_fn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(d.physical, &props);
        std::printf("d3d12_committed_texture_test: GPU %s, driver version 0x%08x\n", props.deviceName,
                    props.driverVersion);
    }
    const EnvRestore restore;

    // Two programs, two GLSL modules, one translator-style index (both list set-0 bindings).
    const auto write_program = fssm_program({0x43540001, 0xbf810000});
    const auto read_program = fssm_program({0x43540002, 0xbf810000});
    const auto write_container = shader_container(write_program);
    const auto read_container = shader_container(read_program);
    const testfiles::ScratchDir scratch("palmwave_committed_texture_test");
    const auto& library = scratch.path();
    EXPECT(testfiles::copy_file(vktest::shader_fixture_path(PALMWAVE_TEST_CS_COMMITTED_TEXTURE_WRITE_SPV),
                                library / "cs_committed_texture_write.spv"));
    EXPECT(testfiles::copy_file(vktest::shader_fixture_path(PALMWAVE_TEST_CS_COMMITTED_TEXTURE_READ_SPV),
                                library / "cs_committed_texture_read.spv"));
    {
        std::ofstream index(library / "index.json", std::ios::binary);
        index << "{\"cs_committed_texture_write\": {\"stage\": \"cs\", \"fssm_sha256\": \""
              << palmwave::sha256_hex(write_program)
              << "\", \"bindings\": [2, 8], \"push_constant_bytes\": 144, \"local_size\": [8, 8, 1], "
                 "\"uses_code_base\": false, \"wave64\": false}, "
                 "\"cs_committed_texture_read\": {\"stage\": \"cs\", \"fssm_sha256\": \""
              << palmwave::sha256_hex(read_program)
              << "\", \"bindings\": [0, 2], \"push_constant_bytes\": 144, \"local_size\": [8, 8, 1], "
                 "\"uses_code_base\": false, \"wave64\": false}}";
    }

    std::vector<std::uint8_t> rows(kRowsBytes), output(kOutputBytes);
    {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        info.buffer_device_address = true;
        info.bindless = d.bindless;
        info.bindless_partially_bound = d.partially_bound;
        info.image_view_min_lod = d.image_view_min_lod;
        info.shader_dir = library.string();
        ID3D12Device* inner = nullptr;  // owned by the proxy below
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        Ref<ID3D12Device> proxy;
        EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                           reinterpret_cast<void**>(&proxy.p))));
        rt::d3d12::set_device_outer(inner, proxy.p);
        if (rt::d3d12::device_push_constants_need_fallback(inner)) {
            std::printf("d3d12_committed_texture_test: skipped, the PSOs need 144 push-constant bytes and this GPU "
                        "has %u\n", rt::d3d12::device_max_push_constants_size(inner));
            return 77;
        }

        // 1. Strict first: unset -> the backend's refusal of the unknown bits; "1" with a near miss -> the same.
        {
            ID3D12Resource* probe = reinterpret_cast<ID3D12Resource*>(1);  // cleared by the proxy, never released
            EXPECT(SetEnvironmentVariableA(kVariable, nullptr) != FALSE || GetLastError() == ERROR_ENVVAR_NOT_FOUND);
            Request strict = measured_request();
            EXPECT(call127(proxy.p, strict, &probe) == E_INVALIDARG && probe == nullptr);
            EXPECT(SetEnvironmentVariableA(kVariable, "1") != FALSE);
            Request near_miss = measured_request();
            near_miss.desc.Width = kWidth - 1;
            probe = reinterpret_cast<ID3D12Resource*>(1);
            EXPECT(call127(proxy.p, near_miss, &probe) == E_INVALIDARG && probe == nullptr);
            EXPECT(static_cast<std::uint32_t>(near_miss.desc.Flags) == 0x00300005u);
        }
        Ref<ID3D12Resource> texture;

        // 2. The experiment: the exact tuple, the variable exactly "1". The title's description is not written.
        Request request = measured_request();
        EXPECT(call127(proxy.p, request, &texture.p) == S_OK && texture.p);
        EXPECT(static_cast<std::uint32_t>(request.desc.Flags) == 0x00300005u);
        EXPECT(SetEnvironmentVariableA(kVariable, nullptr) != FALSE);  // nothing below goes through slot 127
        const D3D12_RESOURCE_DESC got = texture->GetDesc();
        EXPECT(got.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && got.Width == kWidth && got.Height == kHeight &&
               got.DepthOrArraySize == 1 && got.MipLevels == 1 && got.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
               got.SampleDesc.Count == 1 && got.SampleDesc.Quality == 0 && got.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN);
        EXPECT(static_cast<UINT>(got.Flags) ==
               static_cast<UINT>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
        D3D12_HEAP_PROPERTIES heap_props{};
        D3D12_HEAP_FLAGS heap_flags{};
        EXPECT(SUCCEEDED(texture->GetHeapProperties(&heap_props, &heap_flags)));
        // A committed render-target texture's implicit heap: the flags passed plus the backend's DENY bits
        // (resolved_heap_flags, as d3d12_heap_properties_test measures the host runtime).
        constexpr UINT kWantHeapFlags =
            D3D12_HEAP_FLAG_ALLOW_DISPLAY | D3D12_HEAP_FLAG_DENY_BUFFERS | D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
        EXPECT(heap_props.Type == D3D12_HEAP_TYPE_DEFAULT && static_cast<UINT>(heap_flags) == kWantHeapFlags);
        std::printf("slot 127 experiment: S_OK, GetDesc flags 0x%x, heap flags 0x%x (ALLOW_DISPLAY kept)\n",
                    static_cast<unsigned>(got.Flags), static_cast<unsigned>(heap_flags));

        // Root signature in the console compute layout (as d3d12_set0_compute_test): [0] two constants (s4: the
        // view's slot offset in dwords, s5: the patch), [1] a root UAV (s6:s7: the output), [2] the view table (s8).
        const D3D12_DESCRIPTOR_RANGE view_ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0},
                                                       {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 1}};
        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 2};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor = {1, 0};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {2, view_ranges};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        const auto root_blob = serialize_root_signature(root_desc);
        EXPECT(!root_blob.empty());
        Ref<ID3D12RootSignature> root;
        EXPECT(SUCCEEDED(inner->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));
        Ref<ID3D12PipelineState> write_pso, read_pso;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root.p;
        pso_desc.CS = {write_container.data(), write_container.size()};
        EXPECT(SUCCEEDED(inner->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&write_pso.p))));
        pso_desc.CS = {read_container.data(), read_container.size()};
        EXPECT(SUCCEEDED(inner->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&read_pso.p))));

        // The texture's own views: an RTV, and a UAV (slot 0) and SRV (slot 1) in a shader-visible heap.
        Ref<ID3D12DescriptorHeap> rtv_heap, view_heap;
        const D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1,
                                                       D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        const D3D12_DESCRIPTOR_HEAP_DESC view_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        EXPECT(SUCCEEDED(inner->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap.p))));
        EXPECT(SUCCEEDED(inner->CreateDescriptorHeap(&view_heap_desc, IID_PPV_ARGS(&view_heap.p))));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        inner->CreateRenderTargetView(texture.p, nullptr, rtv);
        const UINT view_step = inner->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const D3D12_CPU_DESCRIPTOR_HANDLE view_cpu = view_heap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_GPU_DESCRIPTOR_HANDLE view_gpu = view_heap->GetGPUDescriptorHandleForHeapStart();
        EXPECT(view_step >= 32 && view_step % 4 == 0 && view_gpu.ptr >= 0x10000 && view_gpu.ptr < (1ull << 32));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        inner->CreateUnorderedAccessView(texture.p, nullptr, &uav, view_cpu);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D = {0, 1, 0, 0.0f};
        inner->CreateShaderResourceView(texture.p, &srv, D3D12_CPU_DESCRIPTOR_HANDLE{view_cpu.ptr + view_step});
        std::uint32_t uav_words[8] = {}, srv_words[8] = {};
        std::memcpy(uav_words, reinterpret_cast<const void*>(view_cpu.ptr), sizeof(uav_words));
        std::memcpy(srv_words, reinterpret_cast<const void*>(view_cpu.ptr + view_step), sizeof(srv_words));
        // Both views must be known to the hash table the fixtures search (a miss there is index 0, the fallback).
        EXPECT(rt::d3d12::bindless_hash_lookup(inner, uav_words) != 0);
        EXPECT(rt::d3d12::bindless_hash_lookup(inner, srv_words) != 0);

        // The output (root UAV) and one readback buffer for the rows and the output.
        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        Ref<ID3D12Resource> out_buffer, readback;
        const auto out_desc = buffer_desc(kOutputBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        EXPECT(SUCCEEDED(inner->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &out_desc,
                                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                        IID_PPV_ARGS(&out_buffer.p))));
        const auto readback_desc = buffer_desc(kOutputOffset + kOutputBytes);
        EXPECT(SUCCEEDED(inner->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&readback.p))));
        const D3D12_GPU_VIRTUAL_ADDRESS out_va = out_buffer->GetGPUVirtualAddress();
        EXPECT(out_va >= 0x10000 && out_va < (1ull << 32));

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        Ref<ID3D12CommandQueue> queue;
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        Ref<ID3D12GraphicsCommandList7> list7;
        Ref<ID3D12Fence> fence;
        EXPECT(SUCCEEDED(inner->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        EXPECT(SUCCEEDED(inner->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        EXPECT(SUCCEEDED(inner->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                                  IID_PPV_ARGS(&list.p))));
        EXPECT(SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list7.p))));
        EXPECT(SUCCEEDED(inner->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));

        // 3a. The texture's first command: initial layout 3 (public UNORDERED_ACCESS, kept) -> RENDER_TARGET.
        barrier(list7.p, texture_barrier(texture.p, D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_RENDER_TARGET,
                                         D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                                         D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_RENDER_TARGET));
        const float clear_color[4] = {kClear[0] / 255.0f, kClear[1] / 255.0f, kClear[2] / 255.0f, 1.0f};
        list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
        barrier(list7.p, texture_barrier(texture.p, D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_COPY,
                                         D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                                         D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_COPY_SOURCE));
        for (UINT r = 0; r < 3; ++r) {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = readback.p;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = UINT64(r) * kRowBytes;
            dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kWidth, 1, 1, kRowBytes};
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = texture.p;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            const D3D12_BOX box{0, kRows[r], 0, kWidth, kRows[r] + 1, 1};
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        }

        // 3b. COPY_SOURCE -> UNORDERED_ACCESS; the write fixture stores the pattern through the texture's UAV.
        barrier(list7.p, texture_barrier(texture.p, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                         D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                         D3D12_BARRIER_LAYOUT_COPY_SOURCE, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS));
        ID3D12DescriptorHeap* heaps[] = {view_heap.p};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root.p);
        list->SetComputeRootUnorderedAccessView(1, out_va);
        list->SetComputeRootDescriptorTable(2, view_gpu);
        list->SetPipelineState(write_pso.p);
        for (UINT p = 0; p < kWrittenPatches; ++p) {
            const std::uint32_t constants[2] = {0 /* the UAV's slot */, p};
            list->SetComputeRoot32BitConstants(0, 2, constants, 0);
            list->Dispatch(kPatch / 8, kPatch / 8, 1);
        }

        // 3c. The UAV writes made visible to shader reads of the same image, then the SRV read into the output.
        barrier(list7.p, texture_barrier(texture.p, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                         D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                         D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                                         D3D12_BARRIER_LAYOUT_SHADER_RESOURCE));
        list->SetPipelineState(read_pso.p);
        for (UINT p = 0; p < kPatches; ++p) {
            const std::uint32_t constants[2] = {view_step / 4 /* the SRV's slot */, p};
            list->SetComputeRoot32BitConstants(0, 2, constants, 0);
            list->Dispatch(kPatch / 8, kPatch / 8, 1);
        }
        {
            D3D12_BUFFER_BARRIER to_copy{};
            to_copy.SyncBefore = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
            to_copy.SyncAfter = D3D12_BARRIER_SYNC_COPY;
            to_copy.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            to_copy.AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE;
            to_copy.pResource = out_buffer.p;
            to_copy.Offset = 0;
            to_copy.Size = UINT64_MAX;
            D3D12_BARRIER_GROUP group{};
            group.Type = D3D12_BARRIER_TYPE_BUFFER;
            group.NumBarriers = 1;
            group.pBufferBarriers = &to_copy;
            list7->Barrier(1, &group);
        }
        list->CopyBufferRegion(readback.p, kOutputOffset, out_buffer.p, 0, kOutputBytes);
        EXPECT(list->Close() == S_OK);
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        EXPECT(rt::d3d12::last_execute_result(queue.p) == palmwave::ReplayResult::success);
        EXPECT(SUCCEEDED(queue->Signal(fence.p, 1)));
        EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, nullptr)));
        void* mapped = nullptr;
        EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)) && mapped);
        std::memcpy(rows.data(), mapped, rows.size());
        std::memcpy(output.data(), static_cast<const std::uint8_t*>(mapped) + kOutputOffset, output.size());
        readback->Unmap(0, nullptr);

        // Everything released after the fence; the texture's last reference is the title's.
        list7.reset();
        list.reset();
        allocator.reset();
        queue.reset();
        fence.reset();
        write_pso.reset();
        read_pso.reset();
        root.reset();
        rtv_heap.reset();
        view_heap.reset();
        out_buffer.reset();
        readback.reset();
        const ULONG left = texture.p->Release();
        texture.p = nullptr;
        EXPECT(left == 0);
        EXPECT(rt::d3d12_xs::device_proxy_ref_count(proxy.p) == 1);
    }

    // 4. The oracles.
    unsigned mismatches = 0;
    for (UINT r = 0; r < 3; ++r)
        for (UINT x = 0; x < kWidth; ++x) {
            const std::uint8_t* got = rows.data() + std::size_t(r) * kRowBytes + std::size_t(x) * 4;
            if (std::memcmp(got, kClear, 4) == 0) continue;
            if (mismatches++ < 8)
                std::fprintf(stderr, "clear row %u x %u: got %u,%u,%u,%u want %u,%u,%u,%u\n", kRows[r], x, got[0],
                             got[1], got[2], got[3], kClear[0], kClear[1], kClear[2], kClear[3]);
        }
    for (UINT p = 0; p < kPatches; ++p)
        for (UINT i = 0; i < kPatchTexels; ++i) {
            const UINT x = kPatchOrigin[p][0] + i % kPatch, y = kPatchOrigin[p][1] + i / kPatch;
            const std::uint8_t* got = output.data() + (std::size_t(p) * kPatchTexels + i) * 4;
            Texel want{};
            if (p < kWrittenPatches) want = pattern(x, y);
            else std::memcpy(want.data(), kClear, 4);  // only the clear touched this patch
            if (std::memcmp(got, want.data(), 4) == 0) continue;
            if (mismatches++ < 8)
                std::fprintf(stderr, "patch %u (%u, %u): got %u,%u,%u,%u want %u,%u,%u,%u\n", p, x, y, got[0], got[1],
                             got[2], got[3], want[0], want[1], want[2], want[3]);
        }
    EXPECT(!mismatches);
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("d3d12_committed_texture_test: the slot-127 experimental texture (flags 0x00300005 -> 0x5 on a copy, "
                "layout word 3 kept, ALLOW_DISPLAY kept) took a UNORDERED_ACCESS -> RENDER_TARGET first barrier, an RTV "
                "clear (%u texels of 3 rows), a UAV pattern write and an SRV read of the same image (%u texels of 6 "
                "patches) exactly; a host-path result only, not a console DCC/tiling/metadata or frame claim\n",
                3 * kWidth, kPatches * kPatchTexels);
    return 0;
}
