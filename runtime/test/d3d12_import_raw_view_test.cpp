// Palmwave: RAW buffer views of imported host memory (Xbox device slot 133) addressed by a translated shader.
//
// A title VirtualAlloc range (the served shim, so the host-page lifetime provider can pin it) is filled with a
// background pattern, then imported as two slot-133 buffers: an input (64 KiB) and an output that allows unordered
// access (the next 64 KiB). After the import the CPU writes independent, non-uniform words into the original pages:
// the input view's 256 words and the 256 words the output view starts with. The test then creates, through the
// D3D12 API only, one shader-visible table of four views:
//   slot 0: RAW SRV of the imported input, 256 words from byte 256
//   slot 1: RAW SRV of an arena (default-heap) buffer, filled by a copy from an upload buffer in the same list
//   slot 2: RAW UAV of the imported output, 256 words from byte 512 (read, then written in place)
//   slot 3: RAW UAV of an arena (default-heap) buffer (written)
// and runs test/shaders/cs_import_raw.asm, hand-written RDNA2 translated by tools/rdna2spirv.py through the standard
// build (test/shaders/build.py), as a compute PSO. The shader loads the four V#s from the table the layer wrote and
// the translator adds the single heap_base the runtime pushes for every resource; nothing in the shader, the push
// constants or the descriptors is adjusted by the test. Both V# readers run on the imported input's V#: the vector
// MUBUF loads, and three scalar s_buffer_loads (an SGPR offset whose low two bits are dropped, the view's last dword,
// and one dword past NUM_RECORDS, which reads 0) whose combination is folded into every arena output word.
//
// Submission 1 holds the dispatch and nothing that writes a CPU-read-back buffer through a command the replay tracks:
// no UAV clear and no copy into READBACK (its one copy fills the DEFAULT arena input). The test checks that the queue
// recorded its shader-write -> host-read dependency (rt::d3d12::shader_host_read_barriers moved by one), waits for the
// fence, and compares every word of the 128 KiB range where the title's pages are, with no Map or copy: the imported
// output words with (a ^ b) + c, every other word (guards, input buffer, the words the clear writes later) with what
// the CPU last wrote. Only then does submission 2 run a ClearUnorderedAccessViewUint of a fifth RAW view of the
// imported output (64 words from byte 4096; it decodes the V# the layer wrote, d3d12_com.cpp) and copy the results to
// a READBACK buffer; it runs no shader and must not move the counter. After its fence the range is compared again
// with the cleared words, and the READBACK copies with the arena output rotr(a + seed, 7) ^ c ^ k, the imported output
// and the clear value. The scalar value k is also recovered from each arena output word and compared on its own. A
// scope guard waits for all queued work before any object it may use is released, on every exit. Validation messages
// are failures here and, through CTest's FAIL_REGULAR_EXPRESSION, also after device destruction.
// It also checks each V#: an imported view's 48-bit base field is the signed48-v1 encoding of its buffer's device
// address minus heap_base plus the view offset (src/imported_view_address.h), and heap_base + sext64(field) is the
// view's first byte; an arena view's is its console VA (an arena offset, below 4 GiB); and each resource's GPU VA is
// unchanged (the title's host address for the imported buffers).
//
// Placement. Where the driver puts the imports in device-address space is its choice, so the test reads the actual
// addresses and follows the contract's own representability conditions: a view that cannot be encoded is refused
// with its slot left as it was (checked), the shader proof does not run, and the test exits 77.
//   - Default mode takes whatever placement the driver gives and says which side of heap_base each import is on.
//   - `--below` is the negative-placement proof. Before the D3D12 device creates its arena, it makes real host-imported
//     Vulkan buffers (AddressHoles: two exact 64 KiB payloads and 73 MiB of distinct synthetic host allocations).
//     They are freed after the arena exists and before the imports, offering the driver free ranges below the arena.
//     It then requires the actual queried device address B of
//     BOTH imports to lie strictly below the actual heap_base, with the whole import below it. When the driver
//     placed either import elsewhere, nothing about the signed contract was exercised: the addresses are printed and
//     the test exits 77, never 0. The holes are never used by GPU work and nothing is copied, mirrored or adjusted.
// Exits 77 too without a Vulkan device, the bindless features, or VK_EXT_external_memory_host. Any other failure,
// and any validation-layer message, fails the test.
//
// Synthetic memory and a hand-written shader only; no game data.
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../shims/host_pins.h"
#include "../shims/shims.h"
#include "../src/gfx10_descriptors.h"
#include "../src/shader_library.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#ifndef PALMWAVE_TEST_IMPORT_RAW_DIR
#error "PALMWAVE_TEST_IMPORT_RAW_DIR must name the folder build.py wrote (spirv/index.json, fssm/cs_import_raw.fssm)"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slots 9/10: handle in RAX
using TitleVirtualAllocFn = void*(WINAPI*)(void*, SIZE_T, DWORD, DWORD);
using TitleVirtualFreeFn = BOOL(WINAPI*)(void*, SIZE_T, DWORD);

namespace {

constexpr SIZE_T k64 = 0x10000;
constexpr UINT kWords = 256;                              // 4 groups of 64
constexpr UINT64 kInFirst = 64, kOutFirst = 128;          // the views' FirstElement (words)
constexpr UINT64 kClearFirst = 1024, kClearWords = 64;    // the cleared view, in the output buffer
constexpr std::uint32_t kSeed = 0x00c0ffee, kClear = 0xc1ea0133u, kSentinelByte = 0xa5;
constexpr UINT64 kViewBytes = kWords * 4;
constexpr UINT64 kReadArena = 0, kReadOut = 1024, kReadClear = 2048, kReadbackBytes = 4096;
constexpr std::uint64_t kLimit = std::uint64_t(1) << 47;
constexpr std::uint64_t kField48 = (std::uint64_t(1) << 48) - 1;
// The scalar reads of cs_import_raw.asm, as view words: SGPR offset 0x1f3 & ~3 = byte 0x1f0, and immediate 0x3fc.
constexpr UINT kScalarWordA = 0x1f0 / 4, kScalarWordB = 0x3fc / 4;

std::uint32_t rotr(std::uint32_t v, unsigned n) { return (v >> n) | (v << (32 - n)); }
std::uint32_t rotl(std::uint32_t v, unsigned n) { return n ? (v << n) | (v >> (32 - n)) : v; }
std::uint32_t background(std::size_t i) { return std::uint32_t(i) * 0x01000193u ^ 0xa5a5a5a5u; }
std::uint32_t in_word(UINT g) { return (g + 1) * 0x9e3779b1u ^ (g << 11) ^ 0x5bd1e995u; }
std::uint32_t arena_word(UINT g) { return rotl(g * 0x85ebca6bu + 0xc2b2ae35u, g & 31); }
std::uint32_t out_init(UINT g) { return ~(g * 0x27d4eb2fu) ^ 0x165667b1u; }
std::uint32_t out_want(UINT g) { return (in_word(g) ^ arena_word(g)) + out_init(g); }
// k = (k0 + k1) ^ k2: the view's words 124 and 255 through s_buffer_load, and 0 for the read past NUM_RECORDS.
std::uint32_t scalar_word() { return (in_word(kScalarWordA) + in_word(kScalarWordB)) ^ 0u; }
std::uint32_t arena_vector_part(UINT g) { return rotr(in_word(g) + kSeed, 7) ^ out_init(g); }
std::uint32_t arena_want(UINT g) { return arena_vector_part(g) ^ scalar_word(); }

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The test's own sign extension of a 48-bit V# base field (bit 47 repeated into bits 63..48).
std::uint64_t sext48(std::uint64_t field) {
    field &= kField48;
    return field & kLimit ? field | ~kField48 : field;
}

// The test's own reading of the signed48-v1 representability conditions (src/imported_view_address.h), from the
// addresses the driver gave: whether the layer must write a V# for the view [offset, offset + size) of a `width`-byte
// import at device address b with GPU VA va, or refuse it.
bool representable(std::uint64_t heap_base, std::uint64_t b, std::uint64_t va, std::uint64_t width,
                   std::uint64_t offset, std::uint64_t size) {
    if (!heap_base || !b || b == heap_base || offset > width || size > width - offset || width > UINT64_MAX - b)
        return false;
    if (((b - heap_base) ^ va) & 3) return false;  // scalar-load dword rounding must match the console's
    if (b > heap_base) {  // positive: every byte of the view below +2^47
        const std::uint64_t delta = b - heap_base;
        return delta < kLimit && offset < kLimit - delta && size <= kLimit - delta - offset;
    }
    // Negative: the view starts at most 2^47 below heap_base and ends at or below it (never crosses zero).
    const std::uint64_t distance = heap_base - b;
    return distance <= kLimit && offset < distance && size <= distance - offset;
}

// The value a V# names the view's first byte by, relative to heap_base, as a 64-bit two's-complement number.
std::uint64_t relative(std::uint64_t heap_base, std::uint64_t b, std::uint64_t offset) {
    return b - heap_base + offset;  // mod 2^64
}

std::array<std::uint32_t, 4> words_at(std::uint64_t cpu) {
    std::array<std::uint32_t, 4> w{};
    std::memcpy(w.data(), reinterpret_cast<const void*>(cpu), sizeof(w));
    return w;
}

bool holds_sentinel(std::uint64_t cpu, UINT bytes) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(cpu);
    for (UINT i = 0; i < bytes; ++i)
        if (p[i] != kSentinelByte) return false;
    return true;
}

// Signals the next fence value on `queue` and waits for it (20 s at most). False on any failure or timeout.
bool wait_for(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64& value) {
    const UINT64 target = ++value;
    if (FAILED(queue->Signal(fence, target))) return false;
    const ULONGLONG started = GetTickCount64();
    for (;;) {
        const UINT64 completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) return false;  // D3D12 device-removed sentinel
        if (completed >= target) return true;
        if (GetTickCount64() - started >= 20000) return false;
        Sleep(1);
    }  // no event remains armed after a timeout; QueueIdle still retires work before resource release
}

// On every exit, including a failed queue Signal or fence wait, retire submitted work before any referenced
// resource is released. The test still uses D3D12 fences for both observations; this Vulkan wait is cleanup only.
struct QueueIdle {
    VkDevice device;
    PFN_vkDeviceWaitIdle wait;
    ~QueueIdle() { if (device && wait) wait(device); }
};

// --below: real HOST_ALLOCATION imports before the arena, released before the runtime imports. Ordinary allocations
// of the same memory type did not yield reusable addresses on the measured RTX 5080. The first two reservations
// import the exact two 64 KiB payloads later used by the runtime; the others have distinct synthetic host allocations.
// This only shapes the driver's free address ranges: no requested/captured device address is supplied, and the test
// below still queries both real runtime-buffer addresses and skips unless their whole ranges are below heap_base.
// Vulkan ownership: VkBuffer -> VkDeviceMemory -> owned VirtualAlloc pages, in that destruction order. None is ever
// submitted to GPU work. Borrowed test pages outlive these imports. Public requirements: VkImportMemoryHostPointerInfoEXT
// (VUIDs 01747/01749/01750) and VkExternalMemoryBufferCreateInfo; no platform allocator behavior is assumed.
class AddressHoles {
public:
    explicit AddressHoles(const vktest::Device& vk) : vk_(vk) {}
    AddressHoles(const AddressHoles&) = delete;
    AddressHoles& operator=(const AddressHoles&) = delete;
    ~AddressHoles() { release(); }

    // Two exact 64 KiB payloads, then the original 73 MiB across several size classes. All are actual host imports.
    static constexpr VkDeviceSize kSizes[] = {0x10000, 0x10000, 0x20000, 0x20000, 0x20000, 0x20000, 0x20000,
                                              0x20000, 0x20000, 0x20000, 0x200000, 0x200000, 0x200000, 0x200000,
                                              0x4000000};

    bool reserve(void* host_pages) {
        const auto host_properties =
            vk_.fn<PFN_vkGetMemoryHostPointerPropertiesEXT>("vkGetMemoryHostPointerPropertiesEXT");
        const auto external_properties = vk_.instance_fn<PFN_vkGetPhysicalDeviceExternalBufferProperties>(
            "vkGetPhysicalDeviceExternalBufferProperties");
        if (!host_properties || !external_properties) return false;
        constexpr VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VkPhysicalDeviceExternalBufferInfo query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
        query.usage = usage;
        query.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkExternalBufferProperties support{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        external_properties(vk_.physical, &query, &support);
        const auto& ext = support.externalMemoryProperties;
        if (!(ext.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
            !(ext.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT) ||
            (ext.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT)) return false;
        VkPhysicalDeviceMemoryProperties memory{};
        vk_.instance_fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(vk_.physical,
                                                                                                        &memory);
        const auto create_buffer = vk_.fn<PFN_vkCreateBuffer>("vkCreateBuffer");
        const auto requirements_of = vk_.fn<PFN_vkGetBufferMemoryRequirements2>("vkGetBufferMemoryRequirements2");
        const auto allocate = vk_.fn<PFN_vkAllocateMemory>("vkAllocateMemory");
        const auto bind = vk_.fn<PFN_vkBindBufferMemory>("vkBindBufferMemory");
        held_.reserve(std::size(kSizes));
        for (std::size_t n = 0; n < std::size(kSizes); ++n) {
            const VkDeviceSize size = kSizes[n];
            held_.push_back({});
            Hole& hole = held_.back();  // every later failure is cleaned by release()
            VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
            external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
            info.size = size;
            info.usage = usage;
            if (create_buffer(vk_.device, &info, nullptr, &hole.buffer) != VK_SUCCESS) return false;
            VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
            VkMemoryRequirements2 requirements2{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated};
            VkBufferMemoryRequirementsInfo2 which{VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
            which.buffer = hole.buffer;
            requirements_of(vk_.device, &which, &requirements2);
            const auto& requirements = requirements2.memoryRequirements;
            if (dedicated.requiresDedicatedAllocation || !requirements.size) return false;
            const VkDeviceSize alignment = vk_.host_pointer_alignment;
            if (requirements.size > UINT64_MAX - (alignment - 1)) return false;
            const VkDeviceSize allocated = (requirements.size + alignment - 1) & ~(alignment - 1);
            if (n < 2) {
                if (allocated > k64) return false;
                hole.pages = static_cast<std::uint8_t*>(host_pages) + n * k64;
            } else {
                hole.pages = VirtualAlloc(nullptr, SIZE_T(allocated), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                hole.owned_pages = true;
            }
            if (!hole.pages || reinterpret_cast<std::uintptr_t>(hole.pages) % alignment) return false;
            VkMemoryHostPointerPropertiesEXT host{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
            if (host_properties(vk_.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                hole.pages, &host) != VK_SUCCESS) return false;
            const std::uint32_t usable = requirements.memoryTypeBits & host.memoryTypeBits;
            constexpr VkMemoryPropertyFlags coherent =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            std::uint32_t type = UINT32_MAX;
            for (std::uint32_t i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; ++i)
                if ((usable & (1u << i)) && (memory.memoryTypes[i].propertyFlags & coherent) == coherent) type = i;
            if (type == UINT32_MAX) return false;
            VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
            import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            import.pHostPointer = hole.pages;
            VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &import};
            flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
            allocation.allocationSize = allocated;
            allocation.memoryTypeIndex = type;
            const VkResult result = allocate(vk_.device, &allocation, nullptr, &hole.backing);
            if (result != VK_SUCCESS) {
                std::fprintf(stderr, "below mode: %llu-byte host-import hole failed, VkResult %d\n",
                             static_cast<unsigned long long>(allocated), int(result));
                return false;
            }
            if (bind(vk_.device, hole.buffer, hole.backing, 0) != VK_SUCCESS) return false;
            const std::uint64_t address = vk_.device_address(hole.buffer);
            if (!address || size > UINT64_MAX - address) return false;
            std::printf("below mode: host-import hole %zu, %llu bytes, type %u, BDA [0x%llx, 0x%llx)%s\n",
                        n, (unsigned long long)size, unsigned(type), (unsigned long long)address,
                        (unsigned long long)(address + size), n < 2 ? " (future test payload)" : "");
            lowest = std::min(lowest, address);
            highest = std::max(highest, address + size);
            bytes += size;
            memory_type = type;
        }
        return true;
    }

    void release() {
        if (held_.empty()) return;
        const auto destroy_buffer = vk_.fn<PFN_vkDestroyBuffer>("vkDestroyBuffer");
        const auto free_memory = vk_.fn<PFN_vkFreeMemory>("vkFreeMemory");
        for (auto& hole : held_) {
            if (hole.buffer) destroy_buffer(vk_.device, hole.buffer, nullptr);
            if (hole.backing) free_memory(vk_.device, hole.backing, nullptr);
            if (hole.owned_pages && hole.pages) VirtualFree(hole.pages, 0, MEM_RELEASE);
        }
        held_.clear();
    }

    std::uint64_t lowest = UINT64_MAX, highest = 0, bytes = 0;
    std::uint32_t memory_type = UINT32_MAX;

private:
    struct Hole {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory backing = VK_NULL_HANDLE;
        void* pages = nullptr;
        bool owned_pages = false;
    };
    const vktest::Device& vk_;
    std::vector<Hole> held_;
};

// Every word of the title's range against `want`; the mismatches (the first six printed).
unsigned compare_pages(const std::uint32_t* pages, const std::vector<std::uint32_t>& want, const char* when,
                       unsigned& compared) {
    unsigned bad = 0;
    const std::size_t half = want.size() / 2;
    for (std::size_t i = 0; i < want.size(); ++i, ++compared) {
        if (pages[i] == want[i]) continue;
        if (bad++ < 6)
            std::fprintf(stderr, "%s: title page word %zu (%s buffer): got %08x want %08x\n", when, i % half,
                         i < half ? "input" : "output", pages[i], want[i]);
    }
    if (bad) std::fprintf(stderr, "%s: %u of %zu words differ\n", when, bad, want.size());
    return bad;
}

// A RAW V# of `bytes` bytes whose 48-bit base field encodes `base` (a 64-bit two's-complement value relative to
// heap_base): the field is base's low 48 bits and sign-extends back to it, no stride, NUM_RECORDS, OOB_SELECT 3 (raw),
// the expected FORMAT (SRV: GFX10_FORMAT_32_UINT=20; untyped RAW UAV: 0), and a buffer TYPE.
// Both are existing encodings; raw operations use byte bounds rather than the typed-format field.
bool raw_vsharp(const std::array<std::uint32_t, 4>& w, std::uint64_t base, std::uint64_t bytes, std::uint32_t format) {
    palmwave::gfx10::Buffer decoded{};
    return palmwave::gfx10::decode(w, decoded) && decoded.address == (base & kField48) &&
           sext48(decoded.address) == base && decoded.format == format && decoded.stride == 0 && decoded.num_records == bytes &&
           decoded.oob_select == palmwave::gfx10::OobSelect::raw;
}

// What a translated shader reaches for byte `o` of the view in slot `w`: heap_base + sext64(field) + o, mod 2^64.
std::uint64_t shader_reaches(std::uint64_t heap_base, const std::array<std::uint32_t, 4>& w, std::uint64_t o) {
    palmwave::gfx10::Buffer decoded{};
    if (!palmwave::gfx10::decode(w, decoded)) return 0;
    return heap_base + sext48(decoded.address) + o;
}

// A resource's device address is relative to its own arena. A second D3D12 device on the same Vulkan device must
// refuse both imported views before touching its descriptor slot, rather than encode against the wrong heap_base.
bool check_foreign_view_refusals(const rt::d3d12::VulkanDeviceInfo& info, ID3D12Resource* input,
                                  ID3D12Resource* output) {
    Ref<ID3D12Device> foreign;
    if (FAILED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&foreign.p)))) return false;
    const D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                           D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    Ref<ID3D12DescriptorHeap> heap;
    if (FAILED(foreign->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap.p)))) return false;
    const auto handle = heap->GetCPUDescriptorHandleForHeapStart();
    const UINT step = foreign->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!handle.ptr || step != 32) return false;
    std::memset(reinterpret_cast<void*>(handle.ptr), kSentinelByte, step);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer = {kInFirst, kWords, 0, D3D12_BUFFER_SRV_FLAG_RAW};
    foreign->CreateShaderResourceView(input, &srv, handle);
    if (!holds_sentinel(handle.ptr, step)) return false;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer = {kOutFirst, kWords, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW};
    foreign->CreateUnorderedAccessView(output, nullptr, &uav, handle);
    if (!holds_sentinel(handle.ptr, step)) return false;
    std::printf("foreign-device imported RAW SRV/UAV: both refused, all 32 descriptor bytes unchanged\n");
    return true;
}

const char* side(std::uint64_t heap_base, std::uint64_t b) {
    return b < heap_base ? "below heap_base (negative signed48 base)" : "above heap_base (positive base)";
}

}  // namespace

int main(int argc, char** argv) {
    bool below_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--below") == 0) {
            below_mode = true;
        } else {
            std::fprintf(stderr, "usage: d3d12_import_raw_view_test [--below]\n");
            return 2;
        }
    }
    vktest::Device vk;
    vk.request_buffer_device_address = true;
    const auto status = vk.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    if (status != vktest::DeviceStatus::ready) return 1;
    if (!vk.bindless) {
        std::printf("SKIP: the device lacks the descriptor indexing features of the bindless set 0\n");
        return 77;
    }
    if (!vk.host_pointer_alignment) {
        std::printf("SKIP: the Vulkan device has no VK_EXT_external_memory_host\n");
        return 77;
    }
    if (vk.host_pointer_alignment > k64 || !vk.buffer_device_address) {
        std::printf("SKIP: minImportedHostPointerAlignment %llu is above 64 KiB, or no bufferDeviceAddress\n",
                    static_cast<unsigned long long>(vk.host_pointer_alignment));
        return 77;
    }
    const auto title_alloc = reinterpret_cast<TitleVirtualAllocFn>(rt::shims::find("kernel32.dll", "VirtualAlloc"));
    const auto title_free = reinterpret_cast<TitleVirtualFreeFn>(rt::shims::find("kernel32.dll", "VirtualFree"));
    if (!title_alloc || !title_free) {
        std::fprintf(stderr, "the title-memory shims are not registered in this executable\n");
        return 1;  // a build defect, not a missing capability
    }
    if (!rt::host_pins::registry().provider.load()) {
        std::fprintf(stderr, "no host-page lifetime provider is linked\n");
        return 1;
    }

    const std::filesystem::path root_dir = PALMWAVE_TEST_IMPORT_RAW_DIR;
    palmwave::ShaderLibrary library;
    EXPECT(library.load((root_dir / "spirv").string()));
    const auto program = read_file(root_dir / "fssm" / "cs_import_raw.fssm");
    EXPECT(program.size() >= 0x150);
    const palmwave::ShaderEntry* translated = library.find("cs", palmwave::sha256_hex(program));
    EXPECT(translated != nullptr);
    // The fixture was translated by this revision for the runtime's V# contract; a stale module is refused by the
    // pipeline (src/shader_library.h), and the test must not be run against one.
    EXPECT(translated->vsharp_addressing == palmwave::kVsharpAddressing);
    const auto container = shader_container(program);

    // The title's pages: 128 KiB through the served VirtualAlloc, every word a background value before any import.
    auto* const pages = static_cast<std::uint32_t*>(title_alloc(nullptr, 2 * k64, MEM_RESERVE | MEM_COMMIT,
                                                                PAGE_READWRITE));
    EXPECT(pages != nullptr);
    constexpr std::size_t kPageWords = 2 * k64 / 4, kOutBase = k64 / 4;
    for (std::size_t i = 0; i < kPageWords; ++i) pages[i] = background(i);
    std::vector<std::uint32_t> cpu_wrote(pages, pages + kPageWords);  // what the CPU last wrote in each word

    // --below: the holes exist before the D3D12 device (and its arena) does. Declared after `vk`, so destroyed first.
    AddressHoles holes(vk);
    if (below_mode) EXPECT(holes.reserve(pages));

    int skipped = 0;
    unsigned compared = 0, scalar_checks = 0;
    std::uint64_t heap_base = 0, b_in = 0, b_out = 0;
    {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = vk.instance_proc;
        info.instance = vk.instance;
        info.physical = vk.physical;
        info.device = vk.device;
        info.queue = vk.queue;
        info.queue_family = vk.family;
        info.buffer_device_address = true;
        info.bindless = vk.bindless;
        info.bindless_partially_bound = vk.partially_bound;
        info.image_view_min_lod = vk.image_view_min_lod;
        info.host_pointer_alignment = vk.host_pointer_alignment;
        info.shader_dir = (root_dir / "spirv").string();
        ID3D12Device* inner = nullptr;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        Ref<ID3D12Device> device;
        if (FAILED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                     reinterpret_cast<void**>(&device.p)))) {
            inner->Release();
            return 1;
        }
        rt::d3d12::set_device_outer(inner, device.p);
        EXPECT(rt::d3d12::bindless_set(device.p) != VK_NULL_HANDLE);
        heap_base = rt::d3d12::device_heap_base(device.p);
        EXPECT(heap_base != 0);
        if (below_mode) {
            std::printf("below mode: %llu bytes of host-imported holes in memory type %u spanned [0x%llx, 0x%llx) before the arena; "
                        "heap_base 0x%llx; the holes are freed now, before the imports\n",
                        (unsigned long long)holes.bytes, unsigned(holes.memory_type), (unsigned long long)holes.lowest,
                        (unsigned long long)holes.highest, (unsigned long long)heap_base);
            holes.release();
            // A measured allocator experiment only: let the driver retire all device work before the new imports.
            // No hole was submitted, and idle does not promise address reuse; the queried BDA check below decides.
            EXPECT(vk.fn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(vk.device) == VK_SUCCESS);
        }

        // The two imports, on the title's pages; their GPU VAs stay the title's addresses.
        Ref<ID3D12Resource> imported_in, imported_out;
        const auto in_desc = buffer_desc(k64);
        const auto out_desc = buffer_desc(k64, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        EXPECT(SUCCEEDED(rt::d3d12::create_buffer_at_host_address(device.p, pages, in_desc, 0, __uuidof(ID3D12Resource),
                                                                  reinterpret_cast<void**>(&imported_in.p))));
        EXPECT(SUCCEEDED(rt::d3d12::create_buffer_at_host_address(device.p, pages + kOutBase, out_desc, 0,
                                                                  __uuidof(ID3D12Resource),
                                                                  reinterpret_cast<void**>(&imported_out.p))));
        const std::uint64_t va_in = imported_in->GetGPUVirtualAddress(), va_out = imported_out->GetGPUVirtualAddress();
        EXPECT(va_in == reinterpret_cast<std::uint64_t>(pages));
        EXPECT(va_out == reinterpret_cast<std::uint64_t>(pages + kOutBase));
        b_in = vk.device_address(rt::d3d12::vulkan_buffer(imported_in.p));
        b_out = vk.device_address(rt::d3d12::vulkan_buffer(imported_out.p));
        EXPECT(b_in && b_out);
        EXPECT(check_foreign_view_refusals(info, imported_in.p, imported_out.p));

        // After the import: the CPU writes the views' words into the original pages.
        for (UINT g = 0; g < kWords; ++g) {
            pages[kInFirst + g] = cpu_wrote[kInFirst + g] = in_word(g);
            pages[kOutBase + kOutFirst + g] = cpu_wrote[kOutBase + kOutFirst + g] = out_init(g);
        }

        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        Ref<ID3D12Resource> upload, arena_in, arena_out, readback;
        const auto committed = [&](Ref<ID3D12Resource>& out, const D3D12_HEAP_PROPERTIES& heap, UINT64 bytes,
                                   D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
            const auto desc = buffer_desc(bytes, flags);
            return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                             IID_PPV_ARGS(&out.p)));
        };
        EXPECT(committed(upload, upload_heap, kViewBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ));
        EXPECT(committed(arena_in, default_heap, kViewBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST));
        EXPECT(committed(arena_out, default_heap, kViewBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        EXPECT(committed(readback, readback_heap, kReadbackBytes, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_COPY_DEST));
        const std::uint64_t va_arena_in = arena_in->GetGPUVirtualAddress();
        const std::uint64_t va_arena_out = arena_out->GetGPUVirtualAddress();
        EXPECT(va_arena_in && va_arena_in < (1ull << 32) && va_arena_out && va_arena_out < (1ull << 32));
        {
            std::uint32_t* mapped = nullptr;
            EXPECT(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))));
            for (UINT g = 0; g < kWords; ++g) mapped[g] = arena_word(g);
            upload->Unmap(0, nullptr);
        }

        // The shader-visible heap: the table (slots 0-3) and the clear view's GPU slot (4). A CPU-only heap holds the
        // clear view's CPU descriptor. Every imported view's slot starts as a sentinel, so a refusal shows.
        const D3D12_DESCRIPTOR_HEAP_DESC visible_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5,
                                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        const D3D12_DESCRIPTOR_HEAP_DESC cpu_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                                  D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        Ref<ID3D12DescriptorHeap> heap, cpu_heap;
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&visible_desc, IID_PPV_ARGS(&heap.p))));
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&cpu_desc, IID_PPV_ARGS(&cpu_heap.p))));
        const std::uint64_t cpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 9))(heap.p);
        const std::uint64_t gpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 10))(heap.p);
        const std::uint64_t clear_cpu = reinterpret_cast<XboxHeapHandleFn>(slot(cpu_heap.p, 9))(cpu_heap.p);
        const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        EXPECT(cpu && clear_cpu && gpu >= 0x10000 && gpu < (1ull << 32) && step == 32);
        const auto at = [&](UINT i) { return cpu + std::uint64_t(i) * step; };
        for (const std::uint64_t s : {at(0), at(2), at(4), clear_cpu})
            std::memset(reinterpret_cast<void*>(s), kSentinelByte, step);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer = {kInFirst, kWords, 0, D3D12_BUFFER_SRV_FLAG_RAW};
        device->CreateShaderResourceView(imported_in.p, &srv, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(at(0))});
        srv.Buffer.FirstElement = 0;
        device->CreateShaderResourceView(arena_in.p, &srv, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(at(1))});
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer = {kOutFirst, kWords, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW};
        device->CreateUnorderedAccessView(imported_out.p, nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(at(2))});
        uav.Buffer.FirstElement = 0;
        device->CreateUnorderedAccessView(arena_out.p, nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(at(3))});
        uav.Buffer.FirstElement = kClearFirst;
        uav.Buffer.NumElements = UINT(kClearWords);
        device->CreateUnorderedAccessView(imported_out.p, nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(at(4))});
        device->CreateUnorderedAccessView(imported_out.p, nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(clear_cpu)});

        // The arena views keep their console VAs: the contract every other resource relies on is unchanged.
        EXPECT(raw_vsharp(words_at(at(1)), va_arena_in, kViewBytes, 20));
        EXPECT(raw_vsharp(words_at(at(3)), va_arena_out, kViewBytes, 0));
        std::uint64_t arena_base = 0;
        EXPECT(rt::d3d12::raw_buffer_view_base(arena_out.p, arena_base) && arena_base == va_arena_out);

        const bool fits_in = representable(heap_base, b_in, va_in, k64, kInFirst * 4, kViewBytes);
        const bool fits_out = representable(heap_base, b_out, va_out, k64, kOutFirst * 4, kViewBytes) &&
                              representable(heap_base, b_out, va_out, k64, kClearFirst * 4, kClearWords * 4) &&
                              representable(heap_base, b_out, va_out, k64, 0, 0);
        std::printf("heap_base 0x%llx; imported input: title VA 0x%llx, device address 0x%llx, %s (%s); imported "
                    "output: title VA 0x%llx, device address 0x%llx, %s (%s)\n",
                    (unsigned long long)heap_base, (unsigned long long)va_in, (unsigned long long)b_in,
                    side(heap_base, b_in), fits_in ? "representable" : "NOT representable", (unsigned long long)va_out,
                    (unsigned long long)b_out, side(heap_base, b_out),
                    fits_out ? "representable" : "NOT representable");
        // Each imported slot holds the V# the representability says, or is untouched.
        if (fits_in) {
            EXPECT(raw_vsharp(words_at(at(0)), relative(heap_base, b_in, kInFirst * 4), kViewBytes, 20));
            // Both of the view's ends, the way the translator computes them, land on the import's own bytes.
            EXPECT(shader_reaches(heap_base, words_at(at(0)), 0) == b_in + kInFirst * 4);
            EXPECT(shader_reaches(heap_base, words_at(at(0)), kViewBytes - 4) == b_in + kInFirst * 4 + kViewBytes - 4);
        } else {
            EXPECT(holds_sentinel(at(0), step));
        }
        if (fits_out) {
            EXPECT(raw_vsharp(words_at(at(2)), relative(heap_base, b_out, kOutFirst * 4), kViewBytes, 0));
            EXPECT(raw_vsharp(words_at(at(4)), relative(heap_base, b_out, kClearFirst * 4), kClearWords * 4, 0));
            EXPECT(raw_vsharp(words_at(clear_cpu), relative(heap_base, b_out, kClearFirst * 4), kClearWords * 4, 0));
            // The UAV clear decodes the view by this zero-offset base (the 48-bit field of the buffer's first byte).
            std::uint64_t out_base = 0;
            EXPECT(rt::d3d12::raw_buffer_view_base(imported_out.p, out_base) &&
                   out_base == (relative(heap_base, b_out, 0) & kField48));
            EXPECT(shader_reaches(heap_base, words_at(at(2)), 0) == b_out + kOutFirst * 4);
            EXPECT(shader_reaches(heap_base, words_at(at(2)), kViewBytes - 4) == b_out + kOutFirst * 4 + kViewBytes - 4);
            EXPECT(shader_reaches(heap_base, words_at(clear_cpu), 0) == b_out + kClearFirst * 4);
        } else {
            EXPECT(holds_sentinel(at(2), step) && holds_sentinel(at(4), step) && holds_sentinel(clear_cpu, step));
        }
        // The GPU VAs did not move.
        EXPECT(imported_in->GetGPUVirtualAddress() == va_in && imported_out->GetGPUVirtualAddress() == va_out);

        // --below needs both imports wholly below the actual heap_base: then every imported view (vector and scalar
        // input reads, the read-modify-write output, the clear) has a negative signed48 base.
        const bool both_below = b_in < heap_base && k64 <= heap_base - b_in && b_out < heap_base &&
                                k64 <= heap_base - b_out;
        if (below_mode && both_below && fits_in && fits_out) {
            // The encoded fields are genuinely negative: bit 47 (word1 bit 15) set in every imported view.
            for (const std::uint64_t s : {at(0), at(2), at(4), clear_cpu}) EXPECT((words_at(s)[1] & 0x8000u) != 0);
        }

        if (!fits_in || !fits_out) {
            std::printf("SKIP: on this device an imported buffer's device address is not representable as a signed48 "
                        "V# base (more than 2^47 from heap_base, at it, or a view crossing it); the layer refused "
                        "those views and left their slots as they were (checked). The shader proof did not run.\n");
            skipped = 1;
        } else if (below_mode && !both_below) {
            std::printf("SKIP (negative placement unavailable): --below needs both imports strictly below heap_base "
                        "0x%llx, but the driver placed the input at 0x%llx and the output at 0x%llx after the "
                        "address holes were freed. Nothing about negative V# bases was exercised; this is not a "
                        "pass.\n",
                        (unsigned long long)heap_base, (unsigned long long)b_in, (unsigned long long)b_out);
            skipped = 1;
        } else {
            // The root signature: [0] four 32-bit constants (s4..s7), [1] a table of two SRVs and two UAVs (s8, its
            // 32-bit VA): the console compute layout (D3D-448); the fixture is built with PALMWAVE_USER_DATA_SHIFT=4.
            const D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
                                                      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 2}};
            D3D12_ROOT_PARAMETER params[2] = {};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[0].Constants = {0, 0, 4};
            params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1].DescriptorTable = {2, ranges};
            params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
            root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
            root_desc.Desc_1_0 = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
            const auto root_blob = serialize_root_signature(root_desc);
            EXPECT(!root_blob.empty());
            Ref<ID3D12RootSignature> root;
            EXPECT(SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(),
                                                         IID_PPV_ARGS(&root.p))));
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
            pso_desc.pRootSignature = root.p;
            pso_desc.CS = {container.data(), container.size()};
            Ref<ID3D12PipelineState> pso;
            EXPECT(SUCCEEDED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso.p))));

            const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
            Ref<ID3D12CommandQueue> queue;
            Ref<ID3D12CommandAllocator> allocator;
            Ref<ID3D12GraphicsCommandList> list;
            Ref<ID3D12Fence> fence;
            EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
            EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&allocator.p))));
            EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                                       IID_PPV_ARGS(&list.p))));
            EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
            // From here every exit, a failed EXPECT included, first waits for all the work given to the queue, so no
            // resource, heap, pipeline, list or import that work may use is released under it. Declared after them
            // all, it is destroyed before them.
            UINT64 fence_value = 0;
            const auto wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(vk.device_proc(vk.device, "vkDeviceWaitIdle"));
            EXPECT(wait_idle != nullptr);
            QueueIdle idle{vk.device, wait_idle};

            constexpr auto common = D3D12_RESOURCE_STATE_COMMON;
            constexpr auto uav_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            constexpr auto srv_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            constexpr auto copy_source = D3D12_RESOURCE_STATE_COPY_SOURCE;

            // Submission 1: the translated shader and nothing that writes a CPU-read-back buffer through a command
            // the replay tracks (no UAV clear, no copy into READBACK; the one copy fills a DEFAULT arena buffer). The
            // CPU then reads the shader's words where the title's pages are, so only the runtime's own shader-write
            // -> host-read dependency can make them visible (VulkanQueue::host_read_barrier).
            list->CopyBufferRegion(arena_in.p, 0, upload.p, 0, kViewBytes);
            const D3D12_RESOURCE_BARRIER acquire[3] = {
                transition(arena_in.p, D3D12_RESOURCE_STATE_COPY_DEST, srv_state),
                transition(imported_in.p, common, srv_state), transition(imported_out.p, common, uav_state)};
            list->ResourceBarrier(3, acquire);
            ID3D12DescriptorHeap* heaps[] = {heap.p};
            list->SetDescriptorHeaps(1, heaps);
            list->SetComputeRootSignature(root.p);
            list->SetPipelineState(pso.p);
            const std::uint32_t constants[4] = {kSeed, 0, 0, 0};
            list->SetComputeRoot32BitConstants(0, 4, constants, 0);
            list->SetComputeRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{gpu});
            list->Dispatch(kWords / 64, 1, 1);
            EXPECT(SUCCEEDED(list->Close()));
            ID3D12CommandList* lists[] = {list.p};
            const std::uint64_t host_barriers_before = rt::d3d12::shader_host_read_barriers(queue.p);
            queue->ExecuteCommandLists(1, lists);
            EXPECT(rt::d3d12::last_execute_result(queue.p) == palmwave::ReplayResult::success);
            // The runtime recorded the dependency itself: a driver whose host memory happens to be coherent without it
            // cannot make this test pass with the dependency missing.
            EXPECT(rt::d3d12::shader_host_read_barriers(queue.p) == host_barriers_before + 1);
            EXPECT(wait_for(queue.p, fence.p, fence_value));

            // The whole 128 KiB range at once, before any other GPU work: the shader's words in the output view, and
            // every other word (both guard regions, the input buffer, the words the clear will write later) exactly
            // what the CPU last wrote.
            std::vector<std::uint32_t> want = cpu_wrote;
            for (UINT g = 0; g < kWords; ++g) want[kOutBase + kOutFirst + g] = out_want(g);
            unsigned bad = compare_pages(pages, want, "after the shader-only submission", compared);
            EXPECT(bad == 0);

            // Submission 2, only after that: the UAV clear (it decodes the V# the layer wrote) and the READBACK copies.
            // It runs no shader, so it adds no shader-write -> host-read dependency; its tracked transfer writes to
            // the import and to READBACK get the existing one.
            EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, nullptr)));
            list->SetDescriptorHeaps(1, heaps);
            const UINT clear_values[4] = {kClear, kClear, kClear, kClear};
            list->ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE{gpu + 4ull * step},
                                               D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(clear_cpu)}, imported_out.p,
                                               clear_values, 0, nullptr);
            const D3D12_RESOURCE_BARRIER to_copy[2] = {transition(imported_out.p, uav_state, copy_source),
                                                       transition(arena_out.p, uav_state, copy_source)};
            list->ResourceBarrier(2, to_copy);
            list->CopyBufferRegion(readback.p, kReadArena, arena_out.p, 0, kViewBytes);
            list->CopyBufferRegion(readback.p, kReadOut, imported_out.p, kOutFirst * 4, kViewBytes);
            list->CopyBufferRegion(readback.p, kReadClear, imported_out.p, kClearFirst * 4, kClearWords * 4);
            const D3D12_RESOURCE_BARRIER release[3] = {transition(imported_out.p, copy_source, common),
                                                       transition(imported_in.p, srv_state, common),
                                                       transition(arena_out.p, copy_source, uav_state)};
            list->ResourceBarrier(3, release);
            EXPECT(SUCCEEDED(list->Close()));
            queue->ExecuteCommandLists(1, lists);
            EXPECT(rt::d3d12::last_execute_result(queue.p) == palmwave::ReplayResult::success);
            EXPECT(rt::d3d12::shader_host_read_barriers(queue.p) == host_barriers_before + 1);
            EXPECT(wait_for(queue.p, fence.p, fence_value));

            for (UINT64 i = 0; i < kClearWords; ++i) want[kOutBase + kClearFirst + i] = kClear;
            bad = compare_pages(pages, want, "after the clear and copy submission", compared);
            void* mapped = nullptr;
            EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
            const auto* got = static_cast<const std::uint32_t*>(mapped);
            for (UINT g = 0; g < kWords; ++g, compared += 2) {
                if (got[kReadArena / 4 + g] != arena_want(g) && bad++ < 12)
                    std::fprintf(stderr, "arena output word %u: got %08x want %08x\n", g, got[kReadArena / 4 + g],
                                 arena_want(g));
                if (got[kReadOut / 4 + g] != out_want(g) && bad++ < 12)
                    std::fprintf(stderr, "readback of the imported output word %u: got %08x want %08x\n", g,
                                 got[kReadOut / 4 + g], out_want(g));
            }
            for (UINT64 i = 0; i < kClearWords; ++i, ++compared)
                if (got[kReadClear / 4 + i] != kClear && bad++ < 12)
                    std::fprintf(stderr, "readback of the cleared word %llu: got %08x\n", (unsigned long long)i,
                                 got[kReadClear / 4 + i]);
            // The scalar path on its own: every group's s_buffer_load result, recovered from its arena output words.
            for (UINT g = 0; g < kWords; ++g, ++scalar_checks) {
                const std::uint32_t k = got[kReadArena / 4 + g] ^ arena_vector_part(g);
                if (k != scalar_word() && bad++ < 12)
                    std::fprintf(stderr, "scalar s_buffer_load result seen by word %u: got %08x want %08x "
                                         "(words %u and %u of the imported view, and 0 past NUM_RECORDS)\n",
                                 g, k, scalar_word(), kScalarWordA, kScalarWordB);
            }
            readback->Unmap(0, nullptr);
            EXPECT(bad == 0);
        }
        imported_in.reset();
        imported_out.reset();
        EXPECT(rt::d3d12::host_import_count(device.p) == 0);
    }
    holes.release();
    EXPECT(rt::host_pins::pin_count() == 0);
    EXPECT(title_free(pages, 0, MEM_RELEASE));
    if (vk.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", vk.validation_errors());
        return 1;
    }
    if (skipped) return 77;
    const bool negative = b_in < heap_base && b_out < heap_base;
    std::printf("imported RAW views%s: a translated shader read a RAW SRV through both V# readers (vector MUBUF and "
                "scalar s_buffer_load) and read-modify-wrote a RAW UAV of two slot-133 imports through the signed48-v1 "
                "V#s the layer wrote (heap_base 0x%llx; input %s, output %s), next to arena views in the same table. "
                "The CPU read the shader's words on the title's pages after a shader-only submission that ended with "
                "the runtime's own shader-write -> host-read dependency; a second submission's UAV clear decoded the "
                "same V#. %u words matched, every other word of the 128 KiB range is what the CPU wrote, the READBACK "
                "copies agree, and %u scalar-path checks matched (error 0)\n",
                below_mode ? " (--below, negative placement measured)" : "", (unsigned long long)heap_base,
                side(heap_base, b_in), side(heap_base, b_out), compared, scalar_checks);
    if (below_mode && !negative) return 1;  // unreachable by construction; never report a positive run as --below
    return 0;
}
