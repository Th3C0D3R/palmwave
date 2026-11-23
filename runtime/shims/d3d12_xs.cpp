// Xbox Series D3D12 imports forwarded to the Windows D3D12 exports.
// The SDK types describe the Windows side. The measured guest ABI differs in the root-signature flag word: Xbox
// sets bits in the high byte (observed 0x03000000) and the PC serializer rejects them, so descriptors are copied
// with that byte cleared before forwarding. On a host that serializes only up to 1.1 (Windows 10's built-in
// d3d12.dll), version 1.2 descriptions are rewritten as 1.1 when no static sampler uses 1.2's flags.
// D3D12XboxCreateDevice returns an Xbox-ABI proxy (d3d12_xs_device.h) over the PC device; only the measured slots
// are overridden, the rest follow the PC vtable.
#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#ifdef PALMWAVE_VULKAN_DEVICE
#include "d3d12_com.h"
#include "xbox_present_bridge.h"
#endif
#include "d3d12_trace.h"
#include "d3d12_xs_device.h"
#include "d3d12_xs_rootsig.h"
#include "shims.h"

namespace {

FARPROC host(const char* name) {
    static HMODULE d3d12 = LoadLibraryExA("d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    FARPROC p = d3d12 ? GetProcAddress(d3d12, name) : nullptr;
    if (!p) std::fprintf(stderr, "[rt] d3d12_xs: host d3d12.dll has no %s\n", name);
    return p;
}

using SerializeFn = decltype(&::D3D12SerializeVersionedRootSignature);
using DeserializerFn = decltype(&::D3D12CreateRootSignatureDeserializer);
using LegacySerializeFn = decltype(&::D3D12SerializeRootSignature);
using CreateDeviceFn = decltype(&::D3D12CreateDevice);

// Asked once: does the host serializer take version 1.2 at all? An empty 1.2 description is valid everywhere 1.2 is
// supported; Windows 10's built-in d3d12.dll rejects it with E_INVALIDARG.
bool host_serializes_1_2(SerializeFn fn) {
    static const bool supported = [fn] {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC empty{};
        empty.Version = D3D_ROOT_SIGNATURE_VERSION_1_2;
        ID3DBlob* blob = nullptr;
        ID3DBlob* error = nullptr;
        const HRESULT hr = fn(&empty, &blob, &error);
        if (blob) blob->Release();
        if (error) error->Release();
        return SUCCEEDED(hr);
    }();
    return supported;
}

HRESULT WINAPI serialize_root_signature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
                                        ID3DBlob** blob, ID3DBlob** error) {
    static auto fn = reinterpret_cast<SerializeFn>(host("D3D12SerializeVersionedRootSignature"));
    if (!fn) return E_NOTIMPL;
    if (!desc) return fn(desc, blob, error);

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC cleared{};
    if (!rt::d3d12_xs::copy_versioned_root_signature_desc_clear_xbox_flags(*desc, cleared)) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: D3D12SerializeVersionedRootSignature: unknown descriptor version %d; "
                     "forwarding unchanged (Xbox flags not cleared)\n",
                     static_cast<int>(desc->Version));
        return fn(desc, blob, error);
    }
    if (cleared.Version == D3D_ROOT_SIGNATURE_VERSION_1_2 && !host_serializes_1_2(fn)) {
        // Windows 10's d3d12.dll stops at 1.1 (d3d12_xs_rootsig.h). Rewrite as 1.1 when nothing is lost.
        std::vector<D3D12_STATIC_SAMPLER_DESC> samplers;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC v11{};
        if (!rt::d3d12_xs::downgrade_root_signature_1_2_to_1_1(cleared, samplers, v11)) {
            std::fprintf(stderr,
                         "[rt] d3d12_xs: D3D12SerializeVersionedRootSignature: version 1.2 with static sampler flags, "
                         "and the host d3d12.dll serializes only up to 1.1; refused\n");
            if (blob) *blob = nullptr;
            if (error) *error = nullptr;
            return E_INVALIDARG;
        }
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: the host d3d12.dll serializes root signatures only up to 1.1; version 1.2 "
                         "descriptions without static sampler flags are serialized as 1.1\n");
        return fn(&v11, blob, error);
    }
    return fn(&cleared, blob, error);
}

// The package DLLs import the versioned serializer; the legacy entry point is kept because it is trivial to forward
// with the same flag fix and the test uses it.
HRESULT WINAPI serialize_root_signature_legacy(const D3D12_ROOT_SIGNATURE_DESC* desc,
                                               D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** blob,
                                               ID3DBlob** error) {
    static auto fn = reinterpret_cast<LegacySerializeFn>(host("D3D12SerializeRootSignature"));
    if (!fn) return E_NOTIMPL;
    if (!desc) return fn(desc, version, blob, error);

    D3D12_ROOT_SIGNATURE_DESC cleared{};
    rt::d3d12_xs::copy_root_signature_desc_clear_xbox_flags(*desc, cleared);
    return fn(&cleared, version, blob, error);
}

HRESULT WINAPI create_root_signature_deserializer(const void* data, SIZE_T size, REFIID iid, void** out) {
    static auto fn = reinterpret_cast<DeserializerFn>(host("D3D12CreateRootSignatureDeserializer"));
    if (!fn) return E_NOTIMPL;

    // The host deserializer rejects a blob whose RTS0 flags carry the Xbox high
    // byte, so forward a copy with that byte cleared when present (the same fix
    // the serializer applies to descriptors).
    std::vector<uint8_t> cleared;
    if (rt::d3d12_xs::copy_rts0_blob_clear_xbox_flags(data, static_cast<size_t>(size), cleared)) {
        return fn(cleared.data(), cleared.size(), iid, out);
    }
    return fn(data, size, iid, out);
}

// The descriptor-heap allocator the device proxy consults at slot 14, bridged to the title's registered
// __XMemAllocationHooks__ (adapted from upstream PR #7021, Khuraie, GPL-3.0). The bridge lives here so the standalone
// d3d12_xs_device test targets need no registry. The free routine is captured with the allocation and checked against
// its live registration before it is called; after the image is unregistered the backing is reported, not freed.
namespace xs = rt::d3d12_xs;

xs::DescriptorBackingStatus xmem_descriptor_allocate(std::size_t byte_size, std::uint64_t attributes,
                                                     xs::DescriptorHeapBacking* out) {
    const rt::shims::XMemHooksSnapshot hooks = rt::shims::xmem_allocation_hooks();
    if (hooks.state == rt::shims::XMemHooksState::None) return xs::DescriptorBackingStatus::not_hooked;
    if (hooks.state != rt::shims::XMemHooksState::Ready || !out) return xs::DescriptorBackingStatus::invalid_hooks;
    void* const address = hooks.alloc(byte_size, attributes);  // the title's code, called with no lock held
    if (!address) return xs::DescriptorBackingStatus::out_of_memory;
    out->address = address;
    out->token = hooks.generation;
    out->release_hook = reinterpret_cast<void*>(hooks.free);
    return xs::DescriptorBackingStatus::allocated;
}

void xmem_descriptor_release(const xs::DescriptorHeapBacking& backing, std::uint64_t attributes) {
    const auto free_fn = reinterpret_cast<rt::shims::XMemFreeHookFn>(backing.release_hook);
    if (!rt::shims::xmem_free_with_hooks(backing.token, free_fn, backing.address, attributes))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: descriptor-heap backing %p not freed: the image whose hooks allocated it is no "
                     "longer registered\n", backing.address);
}

constexpr xs::DescriptorHeapAllocator kXmemDescriptorHeapAllocator{&xmem_descriptor_allocate,
                                                                   &xmem_descriptor_release};

// The opt-in hook-backed texture experiment's default page provider (d3d12_xs_device.h, TexturePageProvider): the same
// allocation bridge, and the same generation-checked free as the LOGICAL release at the texture's final COM Release.
// Contract at_final_release_only: what the title's free does with the pointer (a scoped free that ignores interior
// slices and forwards only its pool base was measured) is the title's; a failed creation never calls it.
void xmem_texture_release(const xs::DescriptorHeapBacking& backing, std::uint64_t attributes) {
    const auto free_fn = reinterpret_cast<rt::shims::XMemFreeHookFn>(backing.release_hook);
    if (!rt::shims::xmem_free_with_hooks(backing.token, free_fn, backing.address, attributes))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: hook-backed texture pages %p not released to the title: the image whose hooks "
                     "allocated them is no longer registered\n", backing.address);
}

constexpr xs::TexturePageProvider kXmemTexturePageProvider{&xmem_descriptor_allocate, &xmem_texture_release,
                                                           xs::TexturePageReleaseContract::at_final_release_only};

// Measured shape: D3D12XboxCreateDevice(adapter, params, riid, out), where params is a 24-word block that is only
// logged and not used to create the device. The struct's total size is unknown, so the log reads 24 x 32-bit words
// (96 bytes) and no more. The PC D3D12CreateDevice needs a minimum feature level instead, so 12_0 is used as the
// least restrictive floor; higher feature levels are still exposed by the created device.
HRESULT WINAPI D3D12XboxCreateDevice(IUnknown* adapter, const void* params, const IID* riid, void** out) {
    constexpr auto method = "D3D12XboxCreateDevice";
    using rt::d3d12_trace::result;
    // Slot 14 of every device proxy consults the title's hooks from now on (an explicit test allocator still wins).
    rt::d3d12_xs::set_default_descriptor_heap_allocator(&kXmemDescriptorHeapAllocator);
    // Consulted only by slot 127's double opt-in hook-backed texture experiment (an explicit test provider still wins).
    rt::d3d12_xs::set_default_texture_page_provider(&kXmemTexturePageProvider);
    // Device slot 61 is CreateSamplerX only for a caller in an image this loader mapped (the title); any other caller
    // keeps the PC CreateMetaCommand. Caller split from upstream PR #7115 (mrp1no, 8f756ebe, GPL-3.0).
    rt::d3d12_xs::set_console_caller([](std::uint64_t address) {
        std::uint64_t base = 0;
        return rt::shims::mapped_image_base(address, base);
    });
    if (!out) return result(method, E_POINTER);
    *out = nullptr;
    if (!riid) return result(method, E_POINTER);

    const bool tracing = rt::d3d12_trace::enabled();
    if (!tracing)
        std::fprintf(stderr, "[rt] d3d12_xs: D3D12XboxCreateDevice adapter=%p params=%p riid=%08lx-%04x-%04x\n",
                     adapter, params, static_cast<unsigned long>(riid->Data1), riid->Data2, riid->Data3);
    // The 24 words are the title's device parameters (docs/boot.md, question 1). Unlike the pointers above they are the
    // same on every run, so they are printed with --trace-d3d12 as well, and one traced run shows them.
    if (params) {
        const uint32_t* words = static_cast<const uint32_t*>(params);
        char line[24 * 11 + 1];
        line[0] = '\0';
        size_t used = 0;
        for (int i = 0; i < 24 && used < sizeof(line); ++i) {
            int n = std::snprintf(line + used, sizeof(line) - used, "%s%08x", i ? " " : "",
                                  static_cast<unsigned>(words[i]));
            if (n < 0) break;
            used += static_cast<size_t>(n);
        }
        std::fprintf(stderr, "[rt] d3d12_xs: D3D12XboxCreateDevice params[24] =%s\n", line);
    } else {
        std::fprintf(stderr, "[rt] d3d12_xs: D3D12XboxCreateDevice params = null\n");
    }
    // Word 0 is the parameter block's version. It selects the console queue method layout of the device's queues
    // (create_device_proxy; public OpenScarlett/FiveStar sources, not an SDK document). No block keeps the PC layout.
    const std::uint32_t xbox_version = params ? *static_cast<const std::uint32_t*>(params) : 0;
    std::fprintf(stderr, "[rt] d3d12_xs: D3D12XboxCreateDevice version word 0x%08x selects the %s queue layout\n",
                 static_cast<unsigned>(xbox_version),
                 xbox_version == 0              ? "PC"
                 : xbox_version < 0x00020004u   ? "legacy console"
                 : xbox_version == 0x00020004u  ? "modern console"
                                                : "unknown (refused) console");

    static std::atomic<bool> proxy_logged{false};
    if (!proxy_logged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: D3D12XboxCreateDevice: wrapping the PC ID3D12Device in the Xbox-ABI proxy "
                     "(slot 62 -> CreateComputePipelineState, slot 76 -> CreateCommittedResource).\n");
    }

#ifdef PALMWAVE_VULKAN_DEVICE
    // Opt-in until the Vulkan device covers the first frame: PALMWAVE_D3D12=vulkan puts the Vulkan-backed device
    // behind the same proxy instead of the host's D3D12 device.
    char backend[16] = {};
    size_t backend_length = 0;
    if (getenv_s(&backend_length, backend, "PALMWAVE_D3D12") == 0 && std::strcmp(backend, "vulkan") == 0) {
        ID3D12Device* vulkan = nullptr;
        HRESULT vhr = rt::d3d12::create_vulkan_device_standalone(
            __uuidof(ID3D12Device), reinterpret_cast<void**>(&vulkan), rt::d3d12::QueueTestSettings::ignore);
        if (FAILED(vhr)) {
            std::fprintf(stderr, "[rt] d3d12_xs: D3D12XboxCreateDevice: Vulkan device failed (hr=0x%08lx)\n",
                         static_cast<unsigned long>(vhr));
            return result(method, vhr);
        }
        vhr = rt::d3d12_xs::create_device_proxy(vulkan, *riid, out, xbox_version);
        if (FAILED(vhr)) {
            vulkan->Release();
            std::fprintf(stderr, "[rt] d3d12_xs: D3D12XboxCreateDevice: proxy over the Vulkan device failed "
                                 "(hr=0x%08lx)\n", static_cast<unsigned long>(vhr));
            return result(method, vhr);
        }
        // The PresentX bridge (window "Palmwave Game Launcher", presenter, shared frame pacer) goes in BEFORE the outer
        // is set, so its presenter holds the real device and not the proxy. Without it the device still works, the
        // frame events keep the proxy's first-frame counter, and queue PresentX is refused (E_NOTIMPL).
        if (xbox_version == 0x00020004u) {
            if (const HRESULT bridge = rt::d3d12::install_present_bridge(*out); FAILED(bridge))
                std::fprintf(stderr, "[rt] d3d12_xs: D3D12XboxCreateDevice: no PresentX bridge (hr=0x%08lx); nothing "
                                     "can be presented on this device\n", static_cast<unsigned long>(bridge));
        }
        rt::d3d12::set_device_outer(vulkan, static_cast<IUnknown*>(*out));
        if (!tracing)
            std::fprintf(stderr, "[rt] d3d12_xs: D3D12XboxCreateDevice -> proxy %p (Vulkan device %p)\n", *out,
                         static_cast<void*>(vulkan));
        return result(method, S_OK);
    }
#endif

    static auto create = reinterpret_cast<CreateDeviceFn>(host("D3D12CreateDevice"));
    if (!create) return result(method, E_NOTIMPL);

    ID3D12Device* real = nullptr;
    HRESULT hr = create(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device),
                        reinterpret_cast<void**>(&real));
    if (FAILED(hr) || !real) {
        if (real) real->Release();
        std::fprintf(stderr, "[rt] d3d12_xs: D3D12XboxCreateDevice: PC D3D12CreateDevice failed (hr=0x%08lx)\n",
                     static_cast<unsigned long>(hr));
        return result(method, FAILED(hr) ? hr : E_FAIL);
    }

    hr = rt::d3d12_xs::create_device_proxy(real, *riid, out, xbox_version);
    if (FAILED(hr)) {
        // create_device_proxy keeps the caller's reference on failure.
        real->Release();
        std::fprintf(stderr, "[rt] d3d12_xs: D3D12XboxCreateDevice: proxy creation failed (hr=0x%08lx)\n",
                     static_cast<unsigned long>(hr));
        return result(method, hr);
    }
    if (!tracing)
        std::fprintf(stderr, "[rt] d3d12_xs: D3D12XboxCreateDevice -> proxy %p (PC device %p)\n", *out,
                     static_cast<void*>(real));
    return result(method, S_OK);
}

static_assert(std::is_same_v<decltype(&serialize_root_signature), SerializeFn>);
static_assert(std::is_same_v<decltype(&create_root_signature_deserializer), DeserializerFn>);
static_assert(std::is_same_v<decltype(&serialize_root_signature_legacy), LegacySerializeFn>);

// Measured call (RT-5, 1.0.2.1): the title calls it once at startup with one pointer to a zeroed 32-byte block whose
// dword at +4 it sets (a count), and only checks the result for failure. The PC/Vulkan side owns virtual memory,
// so the policy is accepted and ignored.
HRESULT WINAPI configure_virtual_memory(void* /*desc*/) { return S_OK; }

}  // namespace

SHIM_TABLE(d3d12_xs,
    {"d3d12_xs.dll", "D3D12SerializeVersionedRootSignature", reinterpret_cast<void*>(&serialize_root_signature)},
    {"d3d12_xs.dll", "D3D12SerializeRootSignature", reinterpret_cast<void*>(&serialize_root_signature_legacy)},
    {"d3d12_xs.dll", "D3D12CreateRootSignatureDeserializer",
     reinterpret_cast<void*>(&create_root_signature_deserializer)},
    {"d3d12_xs.dll", "D3D12XboxCreateDevice", reinterpret_cast<void*>(&D3D12XboxCreateDevice)},
    {"d3d12_xs.dll", "D3D12ConfigureVirtualMemoryX", reinterpret_cast<void*>(&configure_virtual_memory)},
);
