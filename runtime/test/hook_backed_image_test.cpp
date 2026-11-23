// Palmwave: the opt-in hook-backed texture experiment's CPU-side rules, with no GPU, driver or game data.
//   1. src/hook_backed_image.h: lease size (the image's real size, rounded only to the import alignment), the bind
//      alignment, address checks (null, misaligned, wrapping; never rounded), and the coherent import type set (the
//      measured RTX 5080 shapes as synthetic masks: OPTIMAL image types 0x3 vs host-pointer types 0xc is EMPTY; LINEAR
//      0x1f vs 0xc keeps only the HOST_VISIBLE | HOST_COHERENT type).
//   2. teardown_plan at every creation failure point, for both release contracts, and at a created texture's release:
//      nothing submitted -> image, memory, unpin at once and in that order, then a logical release only under
//      owned_allocation (at_final_release_only keeps the allocation); submitted or created -> one retired group, then
//      exactly one logical release (created) at once.
//   3. DeferredDeletionQueue's image group with recording Vulkan entry points: nothing before its ticket; then, in one
//      drain step, framebuffers, views, image, memory, and only then the unpin.
//   4. The served xmem pool (registry-resolved XMemAlloc/XMemFree) under pins, as the measured scope uses it: a 64 MiB
//      block starts on a 64 KiB boundary (the approved host alignment guarantee; small blocks keep 64 bytes), two slices
//      pin disjointly, a scoped free that ignores the interior slice and forwards the base leaves the block pending
//      while either pin lives (a probe pin in its tail is refused as unmanaged), the slices' bytes stay intact after the
//      logical releases, and the pins go only with the retired groups, after the scope was removed; no release runs
//      after the scope, and the core never frees an interior pointer.
#include "../src/hook_backed_image.h"
#include "../src/vulkan_backend.h"
#include "../shims/host_pins.h"
#include "../shims/shims.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace hb = palmwave::hook_backed_image;

namespace {

int g_failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); ++g_failures; } \
} while (0)

void test_policy() {
    std::uint64_t bytes = 1;
    CHECK(hb::lease_bytes(33177600, 4096, bytes) == hb::SizeStatus::ok && bytes == 33177600);  // already a multiple
    CHECK(hb::lease_bytes(33177601, 4096, bytes) == hb::SizeStatus::ok && bytes == 33181696);
    CHECK(hb::lease_bytes(0, 4096, bytes) == hb::SizeStatus::zero_size && bytes == 0);
    CHECK(hb::lease_bytes(100, 3000, bytes) == hb::SizeStatus::bad_host_alignment);
    CHECK(hb::lease_bytes(~std::uint64_t(0) - 5, 4096, bytes) == hb::SizeStatus::overflow && bytes == 0);

    CHECK(hb::bind_alignment(4096, 128) == 4096);
    CHECK(hb::bind_alignment(4096, 65536) == 65536);
    CHECK(hb::bind_alignment(4096, 0) == 4096);
    CHECK(hb::bind_alignment(4096, 96) == 0 && hb::bind_alignment(0, 128) == 0);

    CHECK(hb::check_address(0, 4096, 4096) == hb::AddressStatus::null);
    CHECK(hb::check_address(0x10040, 4096, 4096) == hb::AddressStatus::misaligned);  // a 64-byte block, never rounded
    CHECK(hb::check_address(0x10000, 4096, 4096) == hb::AddressStatus::ok);
    CHECK(hb::check_address(0x10000, 0, 4096) == hb::AddressStatus::wraps);
    CHECK(hb::check_address(~std::uintptr_t(0xFFF), 0x2000, 4096) == hb::AddressStatus::wraps);

    VkPhysicalDeviceMemoryProperties memory{};
    memory.memoryTypeCount = 5;
    memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    memory.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    memory.memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    memory.memoryTypes[3].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    memory.memoryTypes[4].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    CHECK(hb::coherent_import_types(0x3, 0xc, memory) == 0);     // OPTIMAL: refused before any import
    CHECK(hb::coherent_import_types(0x1f, 0xc, memory) == 0x4);  // LINEAR: type 2 only (3 is not coherent)
    CHECK(hb::coherent_import_types(0x1f, 0x1c, memory) == 0x14);
}

bool plan_is(const hb::Plan& plan, std::initializer_list<hb::Step> steps) {
    if (plan.count != steps.size()) return false;
    unsigned i = 0;
    for (const hb::Step s : steps)
        if (plan.steps[i++] != s) return false;
    return true;
}

void test_plans() {
    using S = hb::Step;
    constexpr auto final_only = hb::Contract::at_final_release_only;
    constexpr auto owned = hb::Contract::owned_allocation;
    hb::Progress p;
    CHECK(plan_is(hb::teardown_plan(p, false, final_only), {}));  // refused before anything
    p.image = true;
    CHECK(plan_is(hb::teardown_plan(p, false, final_only), {S::destroy_image}));
    p.leased = true;  // the provider answered, then the address check failed
    CHECK(plan_is(hb::teardown_plan(p, false, final_only), {S::destroy_image, S::keep_allocation}));
    CHECK(plan_is(hb::teardown_plan(p, false, owned), {S::destroy_image, S::logical_release}));
    p.pinned = true;  // the page check or host-pointer query failed
    CHECK(plan_is(hb::teardown_plan(p, false, owned), {S::destroy_image, S::unpin, S::logical_release}));
    p.memory = true;  // the bind failed
    CHECK(plan_is(hb::teardown_plan(p, false, owned),
                  {S::destroy_image, S::free_memory, S::unpin, S::logical_release}));
    CHECK(plan_is(hb::teardown_plan(p, false, final_only),
                  {S::destroy_image, S::free_memory, S::unpin, S::keep_allocation}));
    p.submitted = true;  // the first transition was handed to the queue, then failed
    CHECK(plan_is(hb::teardown_plan(p, false, owned), {S::retire_group, S::logical_release}));
    CHECK(plan_is(hb::teardown_plan(p, false, final_only), {S::retire_group, S::keep_allocation}));
    CHECK(plan_is(hb::teardown_plan(p, true, final_only), {S::retire_group, S::logical_release}));  // final Release
    CHECK(plan_is(hb::teardown_plan(p, true, owned), {S::retire_group, S::logical_release}));
}

// ---- Recording Vulkan entry points for the deferred-deletion queue ----
std::vector<std::string> g_calls;
VKAPI_ATTR void VKAPI_CALL rec_view(VkDevice, VkImageView v, const VkAllocationCallbacks*) {
    g_calls.push_back("view" + std::to_string(reinterpret_cast<std::uintptr_t>(v)));
}
VKAPI_ATTR void VKAPI_CALL rec_image(VkDevice, VkImage, const VkAllocationCallbacks*) { g_calls.push_back("image"); }
VKAPI_ATTR void VKAPI_CALL rec_memory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) {
    g_calls.push_back("memory");
}
VKAPI_ATTR void VKAPI_CALL rec_framebuffer(VkDevice, VkFramebuffer, const VkAllocationCallbacks*) {
    g_calls.push_back("framebuffer");
}

template<class T>
T handle(std::uintptr_t n) {
    return reinterpret_cast<T>(n);
}

void test_group_order() {
    palmwave::VulkanDeviceFunctions vk{};
    vk.vkDestroyImageView = rec_view;
    vk.vkDestroyImage = rec_image;
    vk.vkFreeMemory = rec_memory;
    vk.vkDestroyFramebuffer = rec_framebuffer;
    palmwave::DeferredDeletionQueue queue;
    auto* group = new palmwave::RetiredImageGroup;
    group->views = {handle<VkImageView>(1), handle<VkImageView>(2)};
    group->framebuffers = {handle<VkFramebuffer>(3)};
    group->image = handle<VkImage>(4);
    group->memory = handle<VkDeviceMemory>(5);
    group->after = [] { g_calls.push_back("unpin"); };
    queue.push(5, palmwave::DeferredDeletionQueue::Kind::image_group, reinterpret_cast<std::uint64_t>(group));
    queue.drain(4, handle<VkDevice>(9), vk);
    CHECK(g_calls.empty());  // nothing before every submission that may use it has retired
    queue.drain(5, handle<VkDevice>(9), vk);
    const std::vector<std::string> want = {"framebuffer", "view1", "view2", "image", "memory", "unpin"};
    CHECK(g_calls == want);
}

// ---- The pool under pins ----
using XMemAllocFn = void*(__stdcall*)(size_t, uint64_t);
using XMemFreeFn = void(__stdcall*)(void*, uint64_t);

struct ScopeModel {  // the aggregated scope facts, synthesised: base forwarded, interior ignored
    std::uint8_t* base = nullptr;
    bool free_active = false;
    unsigned frees = 0, interior_ignored = 0, forwarded = 0, after_scope = 0;
    XMemFreeFn previous = nullptr;
    void free(void* p, std::uint64_t attributes) {
        ++frees;
        if (!free_active) {
            ++after_scope;
            return;
        }
        if (p != base) {
            ++interior_ignored;
            return;
        }
        ++forwarded;
        previous(base, attributes);
    }
};

// One texture's lifetime as the layer runs it, with the Vulkan steps reduced to what the pages see.
struct Texture {
    std::uintptr_t begin = 0, end = 0;
    hb::Progress progress{true, true, true, true, true};
    bool released = false;
    bool group_pending = false;
};

void run_plan(Texture& t, bool created, ScopeModel& scope, std::uint64_t attributes) {
    const hb::Plan plan = hb::teardown_plan(t.progress, created, hb::Contract::at_final_release_only);
    for (unsigned i = 0; i < plan.count; ++i) {
        switch (plan.steps[i]) {
        case hb::Step::retire_group: t.group_pending = true; break;  // drained later
        case hb::Step::logical_release:
            if (!t.released) {
                t.released = true;
                scope.free(reinterpret_cast<void*>(t.begin), attributes);
            }
            break;
        default: CHECK(false); break;
        }
    }
}

void retire(Texture& t) {  // the group's last step, after its framebuffers, views, image and memory
    CHECK(t.group_pending);
    t.group_pending = false;
    CHECK(rt::host_pins::unpin(t.begin, t.end));
}

void test_pool() {
    const auto alloc = reinterpret_cast<XMemAllocFn>(rt::shims::find("xmem.dll", "XMemAlloc"));
    const auto release = reinterpret_cast<XMemFreeFn>(rt::shims::find("xmem.dll", "XMemFree"));
    CHECK(alloc && release && rt::host_pins::registry().provider.load());
    if (!alloc || !release || !rt::host_pins::registry().provider.load()) return;
    constexpr std::uint64_t kAttributes = 0x00808000, kBudget = 64ull << 20, kSlice = 33226752;  // align_up(33177600, 64 KiB)
    void* small = alloc(100, 0);
    CHECK(small && reinterpret_cast<std::uintptr_t>(small) % 64 == 0);
    release(small, 0);
    ScopeModel scope;
    scope.previous = release;
    scope.base = static_cast<std::uint8_t*>(alloc(kBudget, kAttributes));
    CHECK(scope.base && reinterpret_cast<std::uintptr_t>(scope.base) % 0x10000 == 0);
    if (!scope.base) return;
    const std::size_t pins0 = rt::host_pins::pin_count();
    Texture a, b;
    a.begin = reinterpret_cast<std::uintptr_t>(scope.base);
    a.end = a.begin + 33177600;
    b.begin = a.begin + kSlice;
    b.end = b.begin + 33177600;
    CHECK(rt::host_pins::pin(a.begin, a.end) == rt::host_pins::PinResult::pinned);
    CHECK(rt::host_pins::pin(b.begin, b.end) == rt::host_pins::PinResult::pinned);
    std::memset(reinterpret_cast<void*>(a.begin), 0xA1, 4096);
    std::memset(reinterpret_cast<void*>(b.begin), 0xB2, 4096);
    const std::uintptr_t tail = a.begin + 2 * kSlice;
    CHECK(tail + 0x1000 <= a.begin + kBudget);
    CHECK(rt::host_pins::pin(tail, tail + 0x1000) == rt::host_pins::PinResult::pinned);  // the pool is live
    CHECK(rt::host_pins::unpin(tail, tail + 0x1000));

    // Reverse COM release inside the free scope: B (interior, ignored), then A (the base, forwarded once).
    scope.free_active = true;
    run_plan(b, true, scope, kAttributes);
    run_plan(a, true, scope, kAttributes);
    CHECK(scope.frees == 2 && scope.interior_ignored == 1 && scope.forwarded == 1);
    CHECK(rt::host_pins::pin(tail, tail + 0x1000) == rt::host_pins::PinResult::unmanaged);  // the free is pending
    CHECK(rt::host_pins::is_pinned(a.begin, a.end) && rt::host_pins::is_pinned(b.begin, b.end));
    const auto* pa = reinterpret_cast<const std::uint8_t*>(a.begin);
    const auto* pb = reinterpret_cast<const std::uint8_t*>(b.begin);
    CHECK(pa[0] == 0xA1 && pa[4095] == 0xA1 && pb[0] == 0xB2 && pb[4095] == 0xB2);  // still the slices' bytes
    scope.free_active = false;  // the scope goes before the GPU retires

    retire(a);  // A's group first: B's pin still holds the pool
    CHECK(pb[0] == 0xB2 && pb[4095] == 0xB2);
    CHECK(rt::host_pins::is_pinned(b.begin, b.end));
    retire(b);  // the last pin: xmem really frees the pool now
    CHECK(rt::host_pins::pin_count() == pins0);
    CHECK(scope.after_scope == 0 && scope.frees == 2 && scope.forwarded == 1);
}

}  // namespace

int main() {
    test_policy();
    test_plans();
    test_group_order();
    test_pool();
    if (g_failures) {
        std::fprintf(stderr, "hook_backed_image_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("hook_backed_image_test: PASS (policy, teardown plans, grouped retirement order, two slices of one "
                "pool under pins with a scoped free)\n");
    return 0;
}
