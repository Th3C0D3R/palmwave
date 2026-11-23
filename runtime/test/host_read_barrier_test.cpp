// Palmwave: the device -> host dependency that ends a submission (VulkanQueue::host_read_barrier and its use in
// VulkanQueue::execute), with CPU mocks: no GPU, driver or game data.
//
// A shader can write a buffer the CPU reads back (a CUSTOM write-back heap, or a slot-133 import of the title's pages)
// through any V# or raw VA, so no replay command names that write. The CPU may then read it after the fence only if the
// submission made the shader's writes available to the host: shader write -> HOST / HOST_READ (the Vulkan
// synchronization examples, "CPU read back of data written by a compute shader"). This test checks:
//   1. host_read_barrier on every queue kind: none without a reason; a tracked readback write as before; shader writes
//      adding every shader stage the queue supports with MEMORY_WRITE; never a stage outside queue_supported_stages;
//   2. VulkanContext's count of such buffers: a READBACK binding (no CPU-written mapping) is not one, re-registering an
//      ID with or without the mapping and removing it keep the count;
//   3. VulkanQueue::execute of a list with one Dispatch, on mocked entry points: no host barrier with no such buffer
//      (or only a READBACK one) registered, exactly one shader-stage -> HOST / HOST_READ barrier with one registered
//      (and the diagnostic counter moved by one), none for a list that runs no shader, none again once it is removed.
#include "../src/vulkan_backend.h"

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace {

using namespace palmwave;
using Kind = VulkanQueue::QueueKind;

template<class T>
T handle(std::uintptr_t n) {
    return reinterpret_cast<T>(n);
}

void expect(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

constexpr VkPipelineStageFlags2 kShaderStages =
    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

void test_decision() {
    for (const Kind kind : {Kind::direct, Kind::compute, Kind::copy}) {
        const VkPipelineStageFlags2 allowed = VulkanQueue::queue_supported_stages(kind);
        const auto within = [&](const VulkanQueue::HostReadBarrier& b) { return (b.stages & ~allowed) == 0; };

        auto b = VulkanQueue::host_read_barrier(kind, false, 0, false);
        expect(b.stages == 0 && b.access == 0, "no reason, no barrier");

        // A tracked copy into a readback buffer: unchanged behaviour.
        b = VulkanQueue::host_read_barrier(kind, true, VK_PIPELINE_STAGE_2_TRANSFER_BIT, false);
        expect(b.stages == VK_PIPELINE_STAGE_2_TRANSFER_BIT && b.access == VK_ACCESS_2_TRANSFER_WRITE_BIT,
               "copy readback: transfer write");
        // A tracked compute-stage readback write on a queue without that stage falls back to transfer (as before).
        b = VulkanQueue::host_read_barrier(kind, true, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, false);
        expect(within(b) && b.stages, "tracked readback stays within the queue's stages");
        if (kind == Kind::copy)
            expect(b.stages == VK_PIPELINE_STAGE_2_TRANSFER_BIT && b.access == VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   "copy queue: transfer fallback");

        // Shader writes to a buffer the CPU reads back.
        b = VulkanQueue::host_read_barrier(kind, false, 0, true);
        expect(within(b), "shader writes stay within the queue's stages");
        if (kind == Kind::copy) {
            expect(b.stages == 0, "a copy queue runs no shader: no shader-stage barrier");
        } else {
            expect(b.stages == (kShaderStages & allowed) && b.access == VK_ACCESS_2_MEMORY_WRITE_BIT,
                   "every supported shader stage, memory write");
            expect(b.stages & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, "compute shader writes covered");
            if (kind == Kind::direct) expect(b.stages & VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, "graphics covered");
        }
        // Both reasons: the union, and a copy-only access no longer applies.
        b = VulkanQueue::host_read_barrier(kind, true, VK_PIPELINE_STAGE_2_TRANSFER_BIT, true);
        expect(within(b) && (b.stages & VK_PIPELINE_STAGE_2_TRANSFER_BIT), "union keeps the transfer source");
        expect(kind == Kind::copy ? b.access == VK_ACCESS_2_TRANSFER_WRITE_BIT
                                  : b.access == VK_ACCESS_2_MEMORY_WRITE_BIT,
               "union access");
    }
}

// The mocked device: only the entry points a queue that dispatches needs.
VulkanDeviceFunctions driver;
unsigned host_barriers = 0;
VkPipelineStageFlags2 host_src_stages = 0;
VkAccessFlags2 host_src_access = 0;
unsigned dispatches = 0;

PFN_vkVoidFunction VKAPI_CALL get(VkDevice, const char* name) {
#define MOCK_DEVICE_FUNCTION(n)                               \
    if (std::string_view(name) == #n) {                        \
        return reinterpret_cast<PFN_vkVoidFunction>(driver.n); \
    }
    PALMWAVE_VK_DEVICE_FUNCTIONS(MOCK_DEVICE_FUNCTION)
#undef MOCK_DEVICE_FUNCTION
    return nullptr;
}

void install_driver() {
    driver.vkCreateCommandPool = +[](VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*,
                                     VkCommandPool* p) {
        *p = handle<VkCommandPool>(1);
        return VK_SUCCESS;
    };
    driver.vkDestroyCommandPool = +[](VkDevice, VkCommandPool, const VkAllocationCallbacks*) {};
    driver.vkCreateSemaphore = +[](VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*,
                                   VkSemaphore* s) {
        *s = handle<VkSemaphore>(1);
        return VK_SUCCESS;
    };
    driver.vkDestroySemaphore = +[](VkDevice, VkSemaphore, const VkAllocationCallbacks*) {};
    driver.vkGetSemaphoreCounterValue = +[](VkDevice, VkSemaphore, std::uint64_t* v) {
        *v = UINT64_MAX;
        return VK_SUCCESS;
    };
    driver.vkQueueWaitIdle = +[](VkQueue) { return VK_SUCCESS; };
    driver.vkAllocateCommandBuffers = +[](VkDevice, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* b) {
        expect(info->commandBufferCount == 1, "one command buffer");
        *b = handle<VkCommandBuffer>(1);
        return VK_SUCCESS;
    };
    driver.vkResetCommandBuffer = +[](VkCommandBuffer, VkCommandBufferResetFlags) { return VK_SUCCESS; };
    driver.vkBeginCommandBuffer = +[](VkCommandBuffer, const VkCommandBufferBeginInfo*) { return VK_SUCCESS; };
    driver.vkEndCommandBuffer = +[](VkCommandBuffer) { return VK_SUCCESS; };
    driver.vkQueueSubmit = +[](VkQueue, unsigned, const VkSubmitInfo*, VkFence) { return VK_SUCCESS; };
    driver.vkCmdBindPipeline = +[](VkCommandBuffer, VkPipelineBindPoint point, VkPipeline) {
        expect(point == VK_PIPELINE_BIND_POINT_COMPUTE, "compute bind point");
    };
    driver.vkCmdPushConstants = +[](VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, unsigned, unsigned,
                                    const void*) {};
    driver.vkCmdDispatch = +[](VkCommandBuffer, unsigned, unsigned, unsigned) { ++dispatches; };
    // With vkCmdPipelineBarrier2 present the host-read barrier uses it; a legacy one to HOST would escape the count.
    driver.vkCmdPipelineBarrier = +[](VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags dst,
                                      VkDependencyFlags, unsigned, const VkMemoryBarrier*, unsigned,
                                      const VkBufferMemoryBarrier*, unsigned, const VkImageMemoryBarrier*) {
        expect(!(dst & VK_PIPELINE_STAGE_HOST_BIT), "host-read barriers go through vkCmdPipelineBarrier2 here");
    };
    driver.vkCmdPipelineBarrier2 = +[](VkCommandBuffer, const VkDependencyInfo* info) {
        for (unsigned i = 0; i < info->memoryBarrierCount; ++i) {
            const VkMemoryBarrier2& m = info->pMemoryBarriers[i];
            if (!(m.dstStageMask & VK_PIPELINE_STAGE_2_HOST_BIT)) continue;
            expect(m.dstAccessMask == VK_ACCESS_2_HOST_READ_BIT, "host read access");
            ++host_barriers;
            host_src_stages |= m.srcStageMask;
            host_src_access |= m.srcAccessMask;
        }
    };
}

void reset_observed() {
    host_barriers = 0;
    host_src_stages = 0;
    host_src_access = 0;
    dispatches = 0;
}

void test_queue() {
    install_driver();
    VulkanContext context(get, handle<VkDevice>(1), true);
    constexpr std::uintptr_t pipeline_id = 0x77;
    context.add_pipeline(pipeline_id, ComputePipeline{handle<VkPipeline>(7), handle<VkPipelineLayout>(8), 0,
                                                      VK_NULL_HANDLE});
    static std::uint8_t cpu_pages[256];
    ResourceBinding readback{};  // a READBACK heap buffer: GPU-written, CPU-read, no CPU-written mapping
    readback.buffer = handle<VkBuffer>(10);
    readback.size = 256;
    readback.host_readable = true;
    ResourceBinding shared = readback;  // a CUSTOM write-back heap or a slot-133 import
    shared.buffer = handle<VkBuffer>(11);
    shared.host = cpu_pages;

    context.add_resource(10, readback);
    expect(context.host_shared_buffer_count() == 0, "a READBACK buffer is not a shared one");
    context.add_resource(11, shared);
    expect(context.host_shared_buffer_count() == 1, "a shared buffer is counted");
    context.add_resource(11, readback);  // re-registered without the mapping
    expect(context.host_shared_buffer_count() == 0, "re-registration drops the count");
    context.add_resource(11, shared);
    context.add_resource(11, shared);    // and with it, twice
    expect(context.host_shared_buffer_count() == 1, "re-registration keeps one count");
    context.remove_resource(11);
    expect(context.host_shared_buffer_count() == 0, "removal drops the count");

    CommandList dispatch_list, idle_list;
    Dispatch dispatch{};
    dispatch.pipeline = pipeline_id;
    dispatch.x = dispatch.y = dispatch.z = 1;
    expect(dispatch_list.dispatch(dispatch) && dispatch_list.close(), "dispatch list");
    expect(idle_list.close(), "a list that runs no shader");
    const CommandList* with_shader = &dispatch_list;
    const CommandList* without_shader = &idle_list;

    for (const Kind kind : {Kind::direct, Kind::compute}) {
        VulkanQueue queue(context, handle<VkQueue>(1), 0, kind);
        expect(queue.ok(), "mock queue");
        const VkPipelineStageFlags2 want = kShaderStages & VulkanQueue::queue_supported_stages(kind);

        // Only a READBACK buffer registered: a dispatch needs no host barrier (the existing optimization).
        reset_observed();
        expect(queue.execute({&with_shader, 1}) == ReplayResult::success, "dispatch without a shared buffer");
        expect(dispatches == 1 && host_barriers == 0 && queue.shader_host_read_barriers() == 0,
               "no shared buffer: no host barrier");

        context.add_resource(11, shared);
        reset_observed();
        expect(queue.execute({&with_shader, 1}) == ReplayResult::success, "dispatch with a shared buffer");
        expect(dispatches == 1 && host_barriers == 1, "one host-read barrier after the dispatch");
        expect(host_src_stages == want && host_src_access == VK_ACCESS_2_MEMORY_WRITE_BIT,
               "from every shader stage of the queue, memory write");
        expect(queue.shader_host_read_barriers() == 1, "the diagnostic counter moved");

        // No shader in the submission: nothing to make available, so nothing is recorded.
        reset_observed();
        expect(queue.execute({&without_shader, 1}) == ReplayResult::success, "list without a shader");
        expect(host_barriers == 0 && queue.shader_host_read_barriers() == 1, "no shader: no host barrier");

        context.remove_resource(11);
        reset_observed();
        expect(queue.execute({&with_shader, 1}) == ReplayResult::success, "dispatch after removal");
        expect(dispatches == 1 && host_barriers == 0 && queue.shader_host_read_barriers() == 1,
               "removed: no host barrier");
    }
    {
        // A copy queue runs no shader; with a shared buffer registered it records no shader-stage barrier.
        VulkanQueue queue(context, handle<VkQueue>(1), 0, Kind::copy);
        expect(queue.ok(), "mock copy queue");
        context.add_resource(11, shared);
        reset_observed();
        expect(queue.execute({&without_shader, 1}) == ReplayResult::success, "copy queue list");
        expect(host_barriers == 0 && queue.shader_host_read_barriers() == 0, "copy queue: none");
        context.remove_resource(11);
    }
}

}  // namespace

int main() {
    struct TestCase {
        const char* name;
        void (*run)();
    };
    const TestCase cases[] = {
        {"host-read barrier decision per queue kind", test_decision},
        {"shared-buffer count and dispatch submissions", test_queue},
    };
    unsigned failures = 0;
    for (const auto& test : cases) {
        try {
            test.run();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: %s: %s\n", test.name, error.what());
            ++failures;
        }
    }
    if (failures) return 1;
    std::puts("host_read_barrier_test: shader submissions end with a shader-write -> host-read dependency exactly while "
              "a CPU-read-back buffer is registered, within each queue's stages; readback and copy paths unchanged");
    return 0;
}
