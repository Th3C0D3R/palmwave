// Palmwave: the pure policy of the opt-in hook-backed texture EXPERIMENT (a diagnostic, not a console ABI).
//
// Device slot 127's exact experimental texture tuple, with PALMWAVE_EXPERIMENTAL_XBOX_TEXTURE_HOOK_BACKING=1 as well as
// PALMWAVE_EXPERIMENTAL_XBOX_COMMITTED_TEXTURE=1, gets ONE linear VkImage whose memory is an import
// (VK_EXT_external_memory_host) of the pages a TexturePageProvider returned for it (d3d12_xs_device.h; the Vulkan layer's
// Resource::create_hook_backed_texture). This header holds the decisions that need no device, so the CPU test
// (hook_backed_image_test) drives every branch and production runs the same code:
//   - lease_bytes: how many bytes to ask for: the image's real VkMemoryRequirements::size, rounded up only to
//     minImportedHostPointerAlignment (an import's size must be a multiple of it). No tile padding, no budget-shaped size.
//   - bind_alignment / check_address: the answer must be non-null, aligned to both minImportedHostPointerAlignment (the
//     import starts there) and the image's own memory alignment (it is bound at offset 0), and must not wrap. A provider
//     that cannot return such an address is refused; the address is never rounded inside or outside its allocation.
//   - coherent_import_types: the memory types the image accepts, the host pointer accepts, and that are HOST_VISIBLE and
//     HOST_COHERENT (the title reads and writes the pages with no flush or invalidate). Empty: refuse, never fall back.
//   - teardown_plan: what releasing (or failing to create) the texture does, and in which order (below).
#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace palmwave::hook_backed_image {

inline constexpr bool power_of_two(std::uint64_t v) { return v && !(v & (v - 1)); }

enum class SizeStatus { ok, zero_size, bad_host_alignment, overflow };

// `required` is VkMemoryRequirements::size of the image as created; `host_alignment` minImportedHostPointerAlignment.
constexpr SizeStatus lease_bytes(std::uint64_t required, std::uint64_t host_alignment, std::uint64_t& out) {
    out = 0;
    if (!required) return SizeStatus::zero_size;
    if (!power_of_two(host_alignment)) return SizeStatus::bad_host_alignment;
    if (required > (std::numeric_limits<std::uint64_t>::max)() - (host_alignment - 1)) return SizeStatus::overflow;
    out = (required + host_alignment - 1) & ~(host_alignment - 1);
    if (out > std::uint64_t((std::numeric_limits<std::size_t>::max)())) {
        out = 0;
        return SizeStatus::overflow;
    }
    return SizeStatus::ok;
}

// The alignment the lease address needs: the larger of the two (both powers of two), or 0 when either is not one.
constexpr std::uint64_t bind_alignment(std::uint64_t host_alignment, std::uint64_t image_alignment) {
    if (!power_of_two(host_alignment) || (image_alignment && !power_of_two(image_alignment))) return 0;
    return image_alignment > host_alignment ? image_alignment : host_alignment;
}

enum class AddressStatus { ok, null, misaligned, wraps };

constexpr AddressStatus check_address(std::uintptr_t address, std::uint64_t bytes, std::uint64_t alignment) {
    if (!address) return AddressStatus::null;
    if (!power_of_two(alignment) || address & (alignment - 1)) return AddressStatus::misaligned;
    if (!bytes || bytes > std::uint64_t((std::numeric_limits<std::uintptr_t>::max)() - address))
        return AddressStatus::wraps;
    return AddressStatus::ok;
}

// The bit mask of memory types an import of the pages may use for this image.
inline std::uint32_t coherent_import_types(std::uint32_t image_types, std::uint32_t host_pointer_types,
                                           const VkPhysicalDeviceMemoryProperties& memory) {
    constexpr VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    std::uint32_t out = 0;
    for (std::uint32_t i = 0; i < memory.memoryTypeCount && i < 32; ++i)
        if ((image_types & host_pointer_types & (1u << i)) && (memory.memoryTypes[i].propertyFlags & coherent) == coherent)
            out |= 1u << i;
    return out;
}

// ---- Lifetime ----
//
// LOGICAL release (the provider's release, i.e. the title's free for the hook bridge) and PHYSICAL page lifetime (the pin
// plus the Vulkan objects on the pages) are separate:
//   - A texture that was created reaches its final COM Release: its image, views and imported memory go to the
//     context's deferred-deletion queue as ONE group, destroyed in order (framebuffers, views, image, memory) and only
//     then unpinned, after every submission that may use them has retired. The logical release is called right away,
//     synchronously, exactly once, with the captured routine and attribute word: an allocation scope that frees slices
//     expects it there. The pin keeps a shim-mediated free of the pages deferred (xmem) or refused until the unpin.
//   - A creation that fails before anything was submitted is undone at once, in order: image destroyed, memory freed,
//     pages unpinned; then the logical release ONLY when the provider's contract says a failed creation may release
//     (owned_allocation). Otherwise the allocation is kept with its allocator and reported (keep_allocation).
//   - A creation that fails after its first submission retires through the same group as a created texture.
enum class Contract { at_final_release_only, owned_allocation };

struct Progress {
    bool image = false;      // vkCreateImage succeeded
    bool leased = false;     // the provider's allocate returned an address
    bool pinned = false;     // host_pins::pin succeeded for the lease
    bool memory = false;     // vkAllocateMemory (the import) succeeded
    bool submitted = false;  // the first-use submission was handed to a queue (the GPU may use the image)
};

enum class Step : std::uint8_t {
    retire_group,     // views, image and memory to the deferred queue as one group; its last step unpins
    destroy_image,    // vkDestroyImage now (nothing was submitted)
    free_memory,      // vkFreeMemory now (nothing was submitted)
    unpin,            // host_pins::unpin now (nothing was submitted)
    logical_release,  // the provider's release, once
    keep_allocation,  // the allocation is not released: logged and counted
};

struct Plan {
    Step steps[5]{};
    unsigned count = 0;
    constexpr void add(Step s) { steps[count++] = s; }
};

// `created`: the resource was handed out (this is its final COM Release), not a failed creation.
constexpr Plan teardown_plan(const Progress& p, bool created, Contract contract) {
    Plan plan;
    if (created || p.submitted) {
        if (p.image || p.memory || p.pinned) plan.add(Step::retire_group);
    } else {
        if (p.image) plan.add(Step::destroy_image);
        if (p.memory) plan.add(Step::free_memory);
        if (p.pinned) plan.add(Step::unpin);
    }
    if (p.leased) plan.add(created || contract == Contract::owned_allocation ? Step::logical_release
                                                                            : Step::keep_allocation);
    return plan;
}

}  // namespace palmwave::hook_backed_image
