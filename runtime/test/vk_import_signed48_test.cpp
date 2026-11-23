// Native Vulkan oracle for the normalized signed48-v1 V# contract, separate from the D3D12 import adapter.
// Two real HOST_ALLOCATION buffers are created first, then a real mapped arena. Their queried BDAs must put both
// whole imports below the arena, or this exits 77. The unchanged translated cs_import_raw shader gets exactly that
// arena BDA in its standard heap_base push constants. V# bases are the actual BDA differences, not invented addresses.
// Original imported pages and the entire arena are compared after a shader-write -> HOST_READ barrier and a fence.
// This proves translated scalar/vector loads and stores at negative placement, not D3D12 descriptors, ClearUAV,
// allocation ordering in the runtime, or console hardware semantics. No game data, shadows or address compensation.
#include "vk_test_device.h"
#include "../src/shader_library.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#ifndef PALMWAVE_TEST_IMPORT_RAW_DIR
#error "PALMWAVE_TEST_IMPORT_RAW_DIR must name the standard translated cs_import_raw fixture"
#endif
#define EXPECT(x) do { if (!(x)) { std::fprintf(stderr, "vk_import_signed48 line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {
constexpr VkDeviceSize kImportBytes = 0x10000, kArenaBytes = 0x40000;
constexpr std::uint32_t kWords = 256, kInputFirst = 64, kOutputFirst = 128;
constexpr std::uint32_t kTable = 0x10000, kArenaInput = 0x20000, kArenaOutput = 0x30000;
constexpr std::uint32_t kSeed = 0x00c0ffee;
constexpr std::uint64_t kSign = 1ull << 47, kMask48 = (1ull << 48) - 1;
std::uint32_t rotl(std::uint32_t v, unsigned n) { return n ? (v << n) | (v >> (32 - n)) : v; }
std::uint32_t rotr7(std::uint32_t v) { return (v >> 7) | (v << 25); }
std::uint32_t background(std::size_t i) { return std::uint32_t(i) * 0x01000193u ^ 0xa5a5a5a5u; }
std::uint32_t input_word(unsigned g) { return (g + 1) * 0x9e3779b1u ^ (g << 11) ^ 0x5bd1e995u; }
std::uint32_t arena_word(unsigned g) { return rotl(g * 0x85ebca6bu + 0xc2b2ae35u, g & 31); }
std::uint32_t output_word(unsigned g) { return ~(g * 0x27d4eb2fu) ^ 0x165667b1u; }
std::uint64_t sign_extend(std::uint64_t field) { return (field & kSign) ? field | ~kMask48 : field; }

// Owned host memory stays alive until the imported VkDeviceMemory is destroyed. The caller's Work guard waits
// for GPU retirement before this destructor. Public Vulkan import requirements: VUIDs 01747, 01749, 01750,
// https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryHostPointerInfoEXT.html .
struct ImportedBuffer {
    const vktest::Device& vk;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::uint32_t* pages = nullptr;
    std::uint32_t type = UINT32_MAX;
    explicit ImportedBuffer(const vktest::Device& d) : vk(d) {}
    ImportedBuffer(const ImportedBuffer&) = delete;
    ImportedBuffer& operator=(const ImportedBuffer&) = delete;
    ~ImportedBuffer() {
        if (buffer) vk.fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(vk.device, buffer, nullptr);
        if (memory) vk.fn<PFN_vkFreeMemory>("vkFreeMemory")(vk.device, memory, nullptr);
        if (pages) VirtualFree(pages, 0, MEM_RELEASE);
    }
    bool create() {
        constexpr VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VkPhysicalDeviceExternalBufferInfo query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
        query.usage = usage;
        query.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkExternalBufferProperties ext{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        vk.instance_fn<PFN_vkGetPhysicalDeviceExternalBufferProperties>("vkGetPhysicalDeviceExternalBufferProperties")(
            vk.physical, &query, &ext);
        if (!(ext.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
            !(ext.externalMemoryProperties.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT))
            return false;
        VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
        info.size = kImportBytes;
        info.usage = usage;
        if (vk.fn<PFN_vkCreateBuffer>("vkCreateBuffer")(vk.device, &info, nullptr, &buffer) != VK_SUCCESS) return false;
        VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated};
        VkBufferMemoryRequirementsInfo2 which{VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
        which.buffer = buffer;
        vk.fn<PFN_vkGetBufferMemoryRequirements2>("vkGetBufferMemoryRequirements2")(vk.device, &which, &req);
        const auto alignment = vk.host_pointer_alignment;
        if (!req.memoryRequirements.size || req.memoryRequirements.size > UINT64_MAX - alignment + 1) return false;
        const auto size = (req.memoryRequirements.size + alignment - 1) & ~(alignment - 1);
        pages = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, SIZE_T(size), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!pages || reinterpret_cast<std::uintptr_t>(pages) % alignment) return false;
        VkMemoryHostPointerPropertiesEXT host{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (vk.fn<PFN_vkGetMemoryHostPointerPropertiesEXT>("vkGetMemoryHostPointerPropertiesEXT")(
                vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, pages, &host) != VK_SUCCESS) return false;
        VkPhysicalDeviceMemoryProperties props{};
        vk.instance_fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(vk.physical, &props);
        constexpr auto coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((host.memoryTypeBits & req.memoryRequirements.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags & coherent) == coherent) { type = i; break; }
        if (type == UINT32_MAX) return false;
        VkMemoryDedicatedAllocateInfo dedicate{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicate.buffer = buffer;
        VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        import.pHostPointer = pages;
        if (dedicated.requiresDedicatedAllocation ||
            (ext.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT))
            import.pNext = &dedicate;
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &import};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
        allocate.allocationSize = size;
        allocate.memoryTypeIndex = type;
        if (vk.fn<PFN_vkAllocateMemory>("vkAllocateMemory")(vk.device, &allocate, nullptr, &memory) != VK_SUCCESS) return false;
        return vk.fn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(vk.device, buffer, memory, 0) == VK_SUCCESS;
    }
};

// Declared after all buffers: retirement happens before any buffer, import or CPU page can disappear, even on failure.
struct Work {
    const vktest::Device& vk;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    explicit Work(const vktest::Device& d) : vk(d) {}
    ~Work() {
        vk.fn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(vk.device);
        if (fence) vk.fn<PFN_vkDestroyFence>("vkDestroyFence")(vk.device, fence, nullptr);
        if (pool) vk.fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(vk.device, pool, nullptr);
        if (pipeline) vk.fn<PFN_vkDestroyPipeline>("vkDestroyPipeline")(vk.device, pipeline, nullptr);
        if (layout) vk.fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(vk.device, layout, nullptr);
        if (module) vk.fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(vk.device, module, nullptr);
    }
};

// Independent byte-layout fixture: stride 0, NUM_RECORDS in bytes, identity swizzle, RAW bounds, SRV format20/UAV0.
std::array<std::uint32_t, 4> raw(std::uint64_t base, std::uint32_t format) {
    return {std::uint32_t(base), std::uint32_t(base >> 32) & 0xffffu, kWords * 4,
            0x30000facu | (format << 12)};
}
unsigned compare(const std::uint32_t* actual, const std::vector<std::uint32_t>& wanted, const char* label) {
    unsigned errors = 0;
    for (std::size_t i = 0; i < wanted.size(); ++i)
        if (actual[i] != wanted[i]) {
            if (errors++ < 6) std::fprintf(stderr, "%s word %zu: got %08x expected %08x\n", label, i, actual[i], wanted[i]);
        }
    return errors;
}

int exercise(vktest::Device& vk) {
    palmwave::ShaderLibrary library;
    const std::filesystem::path dir = PALMWAVE_TEST_IMPORT_RAW_DIR;
    EXPECT(library.load(dir / "spirv"));
    std::ifstream f(dir / "fssm" / "cs_import_raw.fssm", std::ios::binary);
    const std::vector<std::uint8_t> fssm{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    EXPECT(fssm.size() >= 0x150);
    const auto* entry = library.find("cs", palmwave::sha256_hex(fssm));
    EXPECT(entry && entry->vsharp_addressing == palmwave::kVsharpAddressing);
    EXPECT(entry->binding_count == 0 && !entry->uses_code_base && entry->push_constant_bytes == 144);
    EXPECT((entry->local_size == std::array<std::uint32_t, 3>{64, 1, 1}));
    const auto code = library.spirv(*entry);
    EXPECT(!code.empty());
    const auto subgroup = palmwave::decide_subgroup_size(*entry, vk.subgroup);
    if (subgroup.outcome == palmwave::SubgroupOutcome::unavailable ||
        subgroup.outcome == palmwave::SubgroupOutcome::unknown_device) {
        std::puts("SKIP: translated fixture's subgroup size is unavailable"); return 77;
    }

    ImportedBuffer input(vk), output(vk);
    EXPECT(input.create() && output.create());  // genuine imports BEFORE allocating the arena
    void* mapped = nullptr;
    const VkBuffer arena = vk.buffer(kArenaBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &mapped);
    EXPECT(arena && mapped);
    const std::uint64_t base = vk.device_address(arena), in = vk.device_address(input.buffer), out = vk.device_address(output.buffer);
    EXPECT(base && in && out);
    std::printf("native signed48: actual heap_base=0x%llx, input BDA=0x%llx (type%u), output BDA=0x%llx (type%u)\n",
                (unsigned long long)base, (unsigned long long)in, input.type, (unsigned long long)out, output.type);
    if (!(in < base && kImportBytes <= base - in && base - in <= kSign &&
          out < base && kImportBytes <= base - out && base - out <= kSign)) {
        std::puts("SKIP: both real imports are not wholly below the real arena in the signed48 range"); return 77;
    }
    EXPECT((base & 3) == 0 && (in & 3) == 0 && (out & 3) == 0);
    auto* const arena_words = static_cast<std::uint32_t*>(mapped);
    std::vector<std::uint32_t> wanted_in(kImportBytes / 4), wanted_out(kImportBytes / 4), wanted_arena(kArenaBytes / 4);
    // Writes happen AFTER all import/allocation calls. These exact original pages are read again after the fence.
    for (std::size_t i = 0; i < wanted_in.size(); ++i) {
        input.pages[i] = wanted_in[i] = background(i);
        output.pages[i] = wanted_out[i] = background(i + wanted_in.size());
    }
    for (std::size_t i = 0; i < wanted_arena.size(); ++i) arena_words[i] = wanted_arena[i] = background(i + 2 * wanted_in.size());
    for (unsigned g = 0; g < kWords; ++g) {
        input.pages[kInputFirst + g] = wanted_in[kInputFirst + g] = input_word(g);
        output.pages[kOutputFirst + g] = output_word(g);
        wanted_out[kOutputFirst + g] = (input_word(g) ^ arena_word(g)) + output_word(g);
        arena_words[kArenaInput / 4 + g] = wanted_arena[kArenaInput / 4 + g] = arena_word(g);
        wanted_arena[kArenaOutput / 4 + g] = rotr7(input_word(g) + kSeed) ^ output_word(g) ^
                                               (input_word(124) + input_word(255));
    }
    const std::array<std::array<std::uint32_t, 4>, 4> descriptors{
        raw(in - base + kInputFirst * 4, 20), raw(kArenaInput, 20),
        raw(out - base + kOutputFirst * 4, 0), raw(kArenaOutput, 0)};
    for (unsigned i = 0; i < descriptors.size(); ++i) {
        std::memcpy(arena_words + kTable / 4 + i * 8, descriptors[i].data(), 16);
        std::memcpy(wanted_arena.data() + kTable / 4 + i * 8, descriptors[i].data(), 16);
    }
    for (unsigned i : {0u, 2u}) {
        const auto& d = descriptors[i];
        const std::uint64_t field = (std::uint64_t(d[1] & 0xffffu) << 32) | d[0];
        const std::uint64_t actual = i == 0 ? in + kInputFirst * 4 : out + kOutputFirst * 4;
        EXPECT((field & kSign) && base + sign_extend(field) == actual);
        EXPECT(base + sign_extend(field) + kWords * 4 - 4 == actual + kWords * 4 - 4);
    }
    EXPECT(descriptors[1][0] == kArenaInput && descriptors[1][1] == 0);
    EXPECT(descriptors[3][0] == kArenaOutput && descriptors[3][1] == 0);
    std::array<std::uint32_t, 36> pc{};
    pc[4] = kSeed; pc[8] = kTable;
    pc[32] = std::uint32_t(base); pc[33] = std::uint32_t(base >> 32);  // actual arena BDA, standard runtime contract

    Work work(vk);
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = code.size() * sizeof(code[0]); sm.pCode = code.data();
    EXPECT(vk.fn<PFN_vkCreateShaderModule>("vkCreateShaderModule")(vk.device, &sm, nullptr, &work.module) == VK_SUCCESS);
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, UINT32_C(144)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &range;
    EXPECT(vk.fn<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(vk.device, &pl, nullptr, &work.layout) == VK_SUCCESS);
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sg{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    sg.requiredSubgroupSize = subgroup.size;
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cp.stage.module = work.module; cp.stage.pName = "main";
    cp.stage.flags = subgroup.flags; cp.stage.pNext = subgroup.size ? &sg : nullptr; cp.layout = work.layout;
    EXPECT(vk.fn<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(vk.device, VK_NULL_HANDLE, 1, &cp, nullptr, &work.pipeline) == VK_SUCCESS);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool.queueFamilyIndex = vk.family;
    EXPECT(vk.fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(vk.device, &pool, nullptr, &work.pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = work.pool; alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    EXPECT(vk.fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(vk.device, &alloc, &cmd) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT(vk.fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(cmd, &begin) == VK_SUCCESS);
    VkMemoryBarrier acquire{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    acquire.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; acquire.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    const auto barrier = vk.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    barrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &acquire, 0, nullptr, 0, nullptr);
    vk.fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, work.pipeline);
    vk.fn<PFN_vkCmdPushConstants>("vkCmdPushConstants")(cmd, work.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc.data());
    vk.fn<PFN_vkCmdDispatch>("vkCmdDispatch")(cmd, kWords / 64, 1, 1);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
    EXPECT(vk.fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(cmd) == VK_SUCCESS);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    EXPECT(vk.fn<PFN_vkCreateFence>("vkCreateFence")(vk.device, &fi, nullptr, &work.fence) == VK_SUCCESS);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    EXPECT(vk.fn<PFN_vkQueueSubmit>("vkQueueSubmit")(vk.queue, 1, &submit, work.fence) == VK_SUCCESS);
    EXPECT(vk.fn<PFN_vkWaitForFences>("vkWaitForFences")(vk.device, 1, &work.fence, VK_TRUE, UINT64_C(20000000000)) == VK_SUCCESS);
    EXPECT(compare(input.pages, wanted_in, "original imported input") == 0);
    EXPECT(compare(output.pages, wanted_out, "original imported output") == 0);
    EXPECT(compare(arena_words, wanted_arena, "whole arena including guards/descriptors") == 0);
    const std::uint32_t scalar = input_word(124) + input_word(255);  // scalar load at 0x400 must add zero
    for (unsigned g = 0; g < kWords; ++g)
        EXPECT((arena_words[kArenaOutput / 4 + g] ^ rotr7(input_word(g) + kSeed) ^ output_word(g)) == scalar);
    std::printf("vk_import_signed48: 98304 original-memory words exact, 256 scalar checks; negative vector/scalar imports "
                "and positive arena controls, standard shader and actual heap_base; validation errors %u\n", vk.validation_errors());
    return 0;
}
}  // namespace
int main() {
    int result = 1;
    {
        vktest::Device vk; vk.request_buffer_device_address = true;
        const auto status = vk.initialize();
        if (status == vktest::DeviceStatus::unavailable) return 77;
        if (status != vktest::DeviceStatus::ready) return 1;
        if (!vk.buffer_device_address || !vk.enabled.shaderInt64 || !vk.host_pointer_alignment || vk.host_pointer_alignment > kImportBytes) {
            std::puts("SKIP: buffer device address, shaderInt64 or aligned host import unavailable"); return 77;
        }
        result = exercise(vk);
    }  // validation includes all resource and device destruction
    if (vktest::validation_error_count()) return 1;
    return result;
}
