// Palmwave: GetResourceAllocationInfo and GetResourceAllocationInfo1 on the Vulkan backend, called through the
// ordinary PC ID3D12Device / ID3D12Device4 interfaces, against an independent Vulkan oracle on the same VkDevice.
//
// The oracle is a real VkImage built here from a VkImageCreateInfo written out field by field (vkCreateImage +
// vkGetImageMemoryRequirements; no memory is allocated or bound). The backend's own helpers are not used for it.
// Expected answers follow the current code (shims/d3d12_vulkan_device.cpp, allocation() and place()): a texture with
// Layout UNKNOWN is the Vulkan need rounded up to its D3D12 alignment (64 KiB unless 4 KiB is asked for); a buffer is
// its width rounded up to 64 KiB; resources are laid out in order, each at its own alignment.
//
// The descriptions are source-crafted. Flag bits 0x00300000 are opaque extra bits with no established meaning; the
// test only records that the current texture path does not change the Vulkan image request for them, and that the
// buffer path refuses them. Nothing here establishes console allocation sizes, tiling or general compatibility.
// No game data, no GPU submission, no heap or buffer allocation.
//
// Exit: 0 every case passed; 1 a failure or a counted Vulkan validation error; 77 no Vulkan device, or the main
// 3840x2160 oracle image is unsupported by the device (reported precisely; never a pass without an oracle).
// PALMWAVE_VK_VALIDATION=1 runs under VK_LAYER_KHRONOS_validation.
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

template<class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T* operator->() const { return p; }
};

using ull = unsigned long long;

constexpr UINT64 k4 = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;     // 4 KiB
constexpr UINT64 k64 = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;  // 64 KiB
constexpr UINT kOpaqueExtraBits = 0x00300000u;
constexpr UINT kRtUav = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;  // 0x5
constexpr D3D12_RESOURCE_ALLOCATION_INFO kInvalid{UINT64_MAX, k64};  // the backend's invalid answer
constexpr D3D12_RESOURCE_ALLOCATION_INFO kEmpty{0, 1};               // no resources

struct Counts {
    unsigned passed = 0, failed = 0, skipped = 0;
};
Counts counts;

void tally(bool ok) { ok ? ++counts.passed : ++counts.failed; }

void expect_true(bool ok, const char* name, const char* what) {
    std::printf("%s %s: %s\n", ok ? "PASS" : "FAIL", name, what);
    tally(ok);
}

void skip(const char* name, const char* why) {
    std::printf("SKIP %s: %s\n", name, why);
    ++counts.skipped;
}

bool same(const D3D12_RESOURCE_ALLOCATION_INFO& a, const D3D12_RESOURCE_ALLOCATION_INFO& b) {
    return a.SizeInBytes == b.SizeInBytes && a.Alignment == b.Alignment;
}

void expect_info(const char* name, const D3D12_RESOURCE_ALLOCATION_INFO& got,
                 const D3D12_RESOURCE_ALLOCATION_INFO& want) {
    const bool ok = same(got, want);
    std::printf("%s %s: d3d12 size %llu alignment %llu; expected size %llu alignment %llu\n", ok ? "PASS" : "FAIL",
                name, ull(got.SizeInBytes), ull(got.Alignment), ull(want.SizeInBytes), ull(want.Alignment));
    tally(ok);
}

void expect_each(const char* name, UINT index, const D3D12_RESOURCE_ALLOCATION_INFO1& got,
                 const D3D12_RESOURCE_ALLOCATION_INFO1& want) {
    const bool ok = got.Offset == want.Offset && got.SizeInBytes == want.SizeInBytes && got.Alignment == want.Alignment;
    std::printf("%s %s [%u]: offset %llu size %llu alignment %llu; expected offset %llu size %llu alignment %llu\n",
                ok ? "PASS" : "FAIL", name, index, ull(got.Offset), ull(got.SizeInBytes), ull(got.Alignment),
                ull(want.Offset), ull(want.SizeInBytes), ull(want.Alignment));
    tally(ok);
}

UINT64 align_up(UINT64 value, UINT64 to) { return (value + to - 1) / to * to; }

D3D12_RESOURCE_DESC texture2d(UINT64 width, UINT height, UINT flags, UINT64 alignment = 0) {
    D3D12_RESOURCE_DESC d;
    std::memset(&d, 0, sizeof(d));
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Alignment = alignment;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc = {1, 0};
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(flags);
    return d;
}

D3D12_RESOURCE_DESC buffer(UINT64 bytes, UINT flags = 0) {
    D3D12_RESOURCE_DESC d;
    std::memset(&d, 0, sizeof(d));
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc = {1, 0};
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(flags);
    return d;
}

// The caller's descriptions, byte for byte (padding included), before a call.
struct Snapshot {
    const D3D12_RESOURCE_DESC* descs = nullptr;
    std::vector<unsigned char> bytes;
};

Snapshot snapshot(const D3D12_RESOURCE_DESC* descs, UINT owned) {
    Snapshot s;
    s.descs = descs;
    if (descs && owned) {
        s.bytes.resize(sizeof(D3D12_RESOURCE_DESC) * owned);
        std::memcpy(s.bytes.data(), descs, s.bytes.size());
    }
    return s;
}

void expect_unchanged(const char* name, const Snapshot& s) {
    if (s.bytes.empty()) return;
    expect_true(std::memcmp(s.descs, s.bytes.data(), s.bytes.size()) == 0, name,
                "caller descriptions byte-identical after the call");
}

// `owned`: how many descriptions the caller has at `descs`, which may exceed `count` (count 0 with a real array).
D3D12_RESOURCE_ALLOCATION_INFO query(ID3D12Device* device, const char* name, UINT mask, UINT count,
                                     const D3D12_RESOURCE_DESC* descs, UINT owned) {
    const Snapshot before = snapshot(descs, owned);
    const D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo(mask, count, descs);
    expect_unchanged(name, before);
    return info;
}

D3D12_RESOURCE_ALLOCATION_INFO query1(ID3D12Device4* device, const char* name, UINT mask, UINT count,
                                      const D3D12_RESOURCE_DESC* descs, UINT owned,
                                      D3D12_RESOURCE_ALLOCATION_INFO1* each) {
    const Snapshot before = snapshot(descs, owned);
    const D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo1(mask, count, descs, each);
    expect_unchanged(name, before);
    return info;
}

enum class Oracle { ok, unsupported, failed };

struct VulkanNeed {
    Oracle status = Oracle::failed;
    VkMemoryRequirements need{};
};

// The independent oracle: an RGBA8 2D optimal-tiling image with one mip, layer and sample, create flags 0 and
// exclusive sharing, made on the same VkDevice the backend runs on, then destroyed. No memory is bound.
VulkanNeed vulkan_need(const vktest::Device& vk, const char* name, std::uint32_t width, std::uint32_t height,
                       VkImageUsageFlags usage) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = 0;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VulkanNeed out;
    VkImageFormatProperties limits{};
    const VkResult supported =
        vk.instance_fn<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties")(
            vk.physical, info.format, info.imageType, info.tiling, info.usage, info.flags, &limits);
    if (supported == VK_ERROR_FORMAT_NOT_SUPPORTED) {
        std::printf("UNAVAILABLE oracle %s: VK_FORMAT_R8G8B8A8_UNORM, 2D, optimal tiling, create flags 0, usage 0x%x "
                    "is not supported by this device (VK_ERROR_FORMAT_NOT_SUPPORTED)\n",
                    name, unsigned(usage));
        out.status = Oracle::unsupported;
        return out;
    }
    if (supported != VK_SUCCESS) {
        std::printf("FAIL oracle %s: vkGetPhysicalDeviceImageFormatProperties returned VkResult %d\n", name,
                    int(supported));
        out.status = Oracle::failed;
        return out;
    }
    if (width > limits.maxExtent.width || height > limits.maxExtent.height ||
        !(limits.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) {
        std::printf("UNAVAILABLE oracle %s: %ux%u exceeds the device's maximum extent %ux%u for usage 0x%x, or one "
                    "sample is not offered (sampleCounts 0x%x)\n",
                    name, width, height, limits.maxExtent.width, limits.maxExtent.height, unsigned(usage),
                    unsigned(limits.sampleCounts));
        out.status = Oracle::unsupported;
        return out;
    }

    VkImage image = VK_NULL_HANDLE;
    const VkResult created = vk.fn<PFN_vkCreateImage>("vkCreateImage")(vk.device, &info, nullptr, &image);
    if (created != VK_SUCCESS || !image) {
        std::printf("FAIL oracle %s: vkCreateImage returned VkResult %d for a combination the device reports as "
                    "supported\n",
                    name, int(created));
        out.status = Oracle::failed;
        return out;
    }
    vk.fn<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(vk.device, image, &out.need);
    vk.fn<PFN_vkDestroyImage>("vkDestroyImage")(vk.device, image, nullptr);
    out.status = Oracle::ok;
    std::printf("vulkan oracle %s: %ux%u usage 0x%x -> size %llu alignment %llu memoryTypeBits 0x%x\n", name, width,
                height, unsigned(usage), ull(out.need.size), ull(out.need.alignment), out.need.memoryTypeBits);
    return out;
}

// Informational only: a Vulkan alignment above the D3D12 alignment reported for the same texture.
void note_alignment(const char* name, const VkMemoryRequirements& need, UINT64 d3d12_alignment) {
    if (need.alignment > d3d12_alignment)
        std::printf("note %s: Vulkan alignment %llu exceeds the reported D3D12 alignment %llu\n", name,
                    ull(need.alignment), ull(d3d12_alignment));
}

void print_adapter(const vktest::Device& vk) {
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    const auto get = vk.instance_fn<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2");
    if (!get) {
        std::printf("adapter: vkGetPhysicalDeviceProperties2 unavailable; driver details not printed\n");
        return;
    }
    get(vk.physical, &props);
    const VkPhysicalDeviceProperties& p = props.properties;
    std::printf("adapter: %s, vendor 0x%04x device 0x%04x, Vulkan %u.%u.%u, driverVersion 0x%08x, driver %s (%s)\n",
                p.deviceName, p.vendorID, p.deviceID, VK_API_VERSION_MAJOR(p.apiVersion),
                VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion,
                driver.driverName, driver.driverInfo);
    const bool device_query =
        vk.fn<PFN_vkGetDeviceImageMemoryRequirements>("vkGetDeviceImageMemoryRequirements") != nullptr;
    std::printf("adapter: vkGetDeviceImageMemoryRequirements %s (not used as the oracle)\n",
                device_query ? "resolvable" : "not resolvable");
}

constexpr VkImageUsageFlags kMainUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageUsageFlags kSampledUsage =
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

// Returns false when the main oracle image is unavailable on this device.
bool run_oracle_cases(const vktest::Device& vk, ID3D12Device* device, ID3D12Device4* device4) {
    const VulkanNeed big = vulkan_need(vk, "3840x2160 RT|UAV", 3840, 2160, kMainUsage);
    const VulkanNeed small_need = vulkan_need(vk, "64x64 sampled", 64, 64, kSampledUsage);
    if (big.status == Oracle::failed) tally(false);
    if (small_need.status == Oracle::failed) tally(false);
    if (big.status != Oracle::ok) {
        const char* why = big.status == Oracle::unsupported ? "main oracle image unsupported on this device"
                                                            : "main oracle image could not be queried";
        for (const char* name : {"main flags 0x00300005", "control flags 0x5", "main GetResourceAllocationInfo1",
                                 "array of three"})
            skip(name, why);
        return big.status != Oracle::unsupported;
    }

    const D3D12_RESOURCE_ALLOCATION_INFO main_want{align_up(big.need.size, k64), k64};

    // Main case: RT | UAV | opaque extra bits.
    const D3D12_RESOURCE_DESC main_desc = texture2d(3840, 2160, kRtUav | kOpaqueExtraBits);
    const D3D12_RESOURCE_ALLOCATION_INFO main_got = query(device, "main flags 0x00300005", 0, 1, &main_desc, 1);
    expect_info("main flags 0x00300005", main_got, main_want);
    note_alignment("main flags 0x00300005", big.need, main_got.Alignment);

    // Control: the same description with flags 0x5 only.
    const D3D12_RESOURCE_DESC control_desc = texture2d(3840, 2160, kRtUav);
    const D3D12_RESOURCE_ALLOCATION_INFO control_got = query(device, "control flags 0x5", 0, 1, &control_desc, 1);
    expect_info("control flags 0x5", control_got, main_want);
    expect_true(same(main_got, control_got), "main vs control",
                "the opaque extra bits leave the reported texture size and alignment unchanged in current code");

    if (!device4) {
        for (const char* name : {"main GetResourceAllocationInfo1", "array of three"})
            skip(name, "ID3D12Device4 not exposed (counted as a failure above)");
        return true;
    }

    {
        D3D12_RESOURCE_ALLOCATION_INFO1 each{0xdead, 0xbeef, 0xcafe};
        const D3D12_RESOURCE_ALLOCATION_INFO total =
            query1(device4, "main GetResourceAllocationInfo1", 0, 1, &main_desc, 1, &each);
        expect_info("main GetResourceAllocationInfo1 total", total, main_want);
        // D3D12_RESOURCE_ALLOCATION_INFO1 is {Offset, Alignment, SizeInBytes}.
        expect_each("main GetResourceAllocationInfo1", 0, each, {0, main_want.Alignment, main_want.SizeInBytes});
    }

    if (small_need.status != Oracle::ok) {
        skip("array of three", small_need.status == Oracle::unsupported ? "64x64 oracle image unsupported on this device"
                                                                   : "64x64 oracle image could not be queried");
        return true;
    }

    // A small array: a 4 KiB-aligned 64x64 texture (no attachment flags, so the small alignment is allowed), a
    // 300-byte buffer (D3D12's fixed rule) and the main texture. Each is expected independently, then laid out.
    const D3D12_RESOURCE_DESC array[3] = {texture2d(64, 64, 0, k4), buffer(300), main_desc};
    const D3D12_RESOURCE_ALLOCATION_INFO one[3] = {
        {align_up(small_need.need.size, k4), k4}, {align_up(300, k64), k64}, main_want};
    note_alignment("array [0] 64x64", small_need.need, k4);
    D3D12_RESOURCE_ALLOCATION_INFO1 want_each[3]{};
    UINT64 end = 0, alignment = 1;
    for (int i = 0; i < 3; ++i) {
        const UINT64 offset = align_up(end, one[i].Alignment);
        want_each[i] = {offset, one[i].Alignment, one[i].SizeInBytes};
        end = offset + one[i].SizeInBytes;
        alignment = alignment > one[i].Alignment ? alignment : one[i].Alignment;
    }
    const D3D12_RESOURCE_ALLOCATION_INFO want_total{align_up(end, alignment), alignment};

    for (int i = 0; i < 3; ++i) {
        char name[48];
        std::snprintf(name, sizeof(name), "array element %d alone", i);
        expect_info(name, query(device, name, 0, 1, &array[i], 1), one[i]);
    }
    expect_info("array of three total", query(device, "array of three total", 0, 3, array, 3), want_total);
    D3D12_RESOURCE_ALLOCATION_INFO1 each[3] = {{0xdead, 0xbeef, 0xcafe}, {0xdead, 0xbeef, 0xcafe},
                                               {0xdead, 0xbeef, 0xcafe}};
    const D3D12_RESOURCE_ALLOCATION_INFO total1 =
        query1(device4, "array of three GetResourceAllocationInfo1", 0, 3, array, 3, each);
    expect_info("array of three GetResourceAllocationInfo1 total", total1, want_total);
    for (UINT i = 0; i < 3; ++i) expect_each("array of three GetResourceAllocationInfo1", i, each[i], want_each[i]);
    return true;
}

// Refusals and edge answers the current code establishes; none of them needs the Vulkan oracle.
void run_refusal_cases(ID3D12Device* device, ID3D12Device4* device4) {
    const D3D12_RESOURCE_DESC main_desc = texture2d(3840, 2160, kRtUav | kOpaqueExtraBits);

    // Count 0: no resources, size 0 alignment 1, with or without a description pointer.
    expect_info("count 0, null descriptions", query(device, "count 0, null descriptions", 0, 0, nullptr, 0), kEmpty);
    expect_info("count 0, real descriptions", query(device, "count 0, real descriptions", 0, 0, &main_desc, 1),
                kEmpty);
    if (device4)
        expect_info("count 0, GetResourceAllocationInfo1",
                    query1(device4, "count 0, GetResourceAllocationInfo1", 0, 0, nullptr, 0, nullptr), kEmpty);

    // A visible mask above 1: the invalid answer for one resource and for none; the per-resource array untouched.
    expect_info("visible mask 2, count 1", query(device, "visible mask 2, count 1", 2, 1, &main_desc, 1), kInvalid);
    expect_info("visible mask 2, count 0", query(device, "visible mask 2, count 0", 2, 0, nullptr, 0), kInvalid);
    if (device4) {
        const D3D12_RESOURCE_ALLOCATION_INFO1 sentinel{0xdead, 0xbeef, 0xcafe};
        D3D12_RESOURCE_ALLOCATION_INFO1 each = sentinel;
        expect_info("visible mask 2, GetResourceAllocationInfo1",
                    query1(device4, "visible mask 2, GetResourceAllocationInfo1", 2, 1, &main_desc, 1, &each),
                    kInvalid);
        expect_true(std::memcmp(&each, &sentinel, sizeof(each)) == 0, "visible mask 2, GetResourceAllocationInfo1",
                    "per-resource output left as the caller set it");
    }

    // A nonzero count with no descriptions.
    expect_info("count 1, null descriptions", query(device, "count 1, null descriptions", 0, 1, nullptr, 0),
                kInvalid);
    if (device4) {
        D3D12_RESOURCE_ALLOCATION_INFO1 each{0xdead, 0xbeef, 0xcafe};
        expect_info("count 1, null descriptions, GetResourceAllocationInfo1",
                    query1(device4, "count 1, null descriptions, GetResourceAllocationInfo1", 0, 1, nullptr, 0, &each),
                    kInvalid);
        std::printf("info count 1, null descriptions, GetResourceAllocationInfo1 [0]: offset %llu size %llu alignment "
                    "%llu (printed, not asserted)\n",
                    ull(each.Offset), ull(each.SizeInBytes), ull(each.Alignment));
    }

    struct Refused {
        const char* name;
        D3D12_RESOURCE_DESC desc;
    };
    D3D12_RESOURCE_DESC zero_width = main_desc;
    zero_width.Width = 0;
    D3D12_RESOURCE_DESC odd_alignment = main_desc;
    odd_alignment.Alignment = 8192;  // neither 4 KiB nor 64 KiB
    D3D12_RESOURCE_DESC small_with_target = main_desc;
    small_with_target.Alignment = k4;  // 4 KiB is refused for render targets
    D3D12_RESOURCE_DESC row_major = main_desc;
    row_major.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_RESOURCE_DESC deny_srv = texture2d(3840, 2160, kRtUav | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    D3D12_RESOURCE_DESC too_many_mips = main_desc;
    too_many_mips.MipLevels = 13;  // the full chain of 3840x2160 is 12
    D3D12_RESOURCE_DESC msaa_uav = texture2d(3840, 2160, kRtUav);
    msaa_uav.SampleDesc.Count = 4;
    const Refused refused[] = {
        {"zero width", zero_width},
        {"alignment 8 KiB", odd_alignment},
        {"alignment 4 KiB with a render target", small_with_target},
        {"texture with ROW_MAJOR layout", row_major},
        {"DENY_SHADER_RESOURCE without depth-stencil", deny_srv},
        {"MipLevels beyond the full chain", too_many_mips},
        {"4x MSAA with unordered access", msaa_uav},
        {"buffer with opaque extra bits 0x00300000", buffer(300, kOpaqueExtraBits)},
    };
    for (const Refused& r : refused) expect_info(r.name, query(device, r.name, 0, 1, &r.desc, 1), kInvalid);

    // The buffer control without the extra bits: D3D12's fixed rule.
    const D3D12_RESOURCE_DESC plain_buffer = buffer(300);
    expect_info("buffer 300 bytes, flags 0", query(device, "buffer 300 bytes, flags 0", 0, 1, &plain_buffer, 1),
                {k64, k64});

    // One invalid description makes the whole answer invalid.
    const D3D12_RESOURCE_DESC with_bad[2] = {main_desc, zero_width};
    expect_info("array with a zero-width texture", query(device, "array with a zero-width texture", 0, 2, with_bad, 2),
                kInvalid);
    if (device4) {
        D3D12_RESOURCE_ALLOCATION_INFO1 each[2] = {{0xdead, 0xbeef, 0xcafe}, {0xdead, 0xbeef, 0xcafe}};
        expect_info("array with a zero-width texture, GetResourceAllocationInfo1",
                    query1(device4, "array with a zero-width texture, GetResourceAllocationInfo1", 0, 2, with_bad, 2,
                           each),
                    kInvalid);
    }
}

}  // namespace

int main() {
    vktest::Device vk;
    const vktest::DeviceStatus status = vk.initialize();
    if (status == vktest::DeviceStatus::unavailable) {
        std::printf("SKIP: no usable Vulkan device\n");
        return 77;
    }
    if (status != vktest::DeviceStatus::ready) return 1;
    print_adapter(vk);

    bool oracle_available = true;
    {
        Ref<ID3D12Device> device;
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = vk.instance_proc;
        info.instance = vk.instance;
        info.physical = vk.physical;
        info.device = vk.device;
        info.queue = vk.queue;
        info.queue_family = vk.family;
        const HRESULT hr = rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&device.p));
        if (FAILED(hr) || !device.p) {
            std::printf("FAIL create_vulkan_device: 0x%08lx\n", static_cast<unsigned long>(hr));
            return 1;
        }
        Ref<ID3D12Device4> device4;
        const HRESULT qi = device->QueryInterface(IID_PPV_ARGS(&device4.p));
        expect_true(SUCCEEDED(qi) && device4.p, "QueryInterface ID3D12Device4", "GetResourceAllocationInfo1 reachable");

        oracle_available = run_oracle_cases(vk, device.p, device4.p);
        run_refusal_cases(device.p, device4.p);
    }

    const unsigned validation = vk.validation_errors();
    std::printf("allocation info: %u passed, %u failed, %u skipped; Vulkan validation errors %u%s\n", counts.passed,
                counts.failed, counts.skipped, validation, vk.validation ? "" : " (validation off)");
    if (counts.failed || validation) return 1;
    if (!oracle_available) {
        std::printf("SKIP: the main 3840x2160 oracle image is unavailable on this device; no oracle comparison ran\n");
        return 77;
    }
    return 0;
}
