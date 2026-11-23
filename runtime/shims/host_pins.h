// Host page pins: ranges of this process's own memory that a GPU import holds for as long as it lives.
//
// VK_EXT_external_memory_host imports the caller's pages as VkDeviceMemory, and the Vulkan specification requires the
// imported range to stay valid and accessible for that memory's whole lifetime
// (https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryHostPointerInfoEXT.html). The Xbox slot-133
// path (CreatePlacedResourceX, d3d12_vulkan_device.cpp) pins the range it imports here before it validates it, and
// unpins it only after the GPU work that may use it has retired and the memory is freed.
//
// A pin is a logical reservation in this registry, not an OS page lock. It protects a range only against the title
// shims that consult it, so a pin is granted only for a range those shims own end to end. The provider (installed by
// xmem.cpp) accepts a range only when all of it lies in ONE of:
//   - a live allocation the title made through the served VirtualAlloc or XMemVirtualAlloc shims;
//   - 64 KiB physical pages the xmem shim mapped into one of its windows;
//   - a live XMemAlloc / XMemAllocDefault block (the whole range inside the block).
// Anything else (a raw host VirtualAlloc, a section view, a stack, the CRT heap) is PinResult::unmanaged, and with no
// provider linked every pin is PinResult::no_provider: neither claims a lifetime nothing enforces.
//
// While a pin lives the shims refuse to invalidate it with ERROR_BUSY under the same lock (xmem.cpp):
// XMemMapPhysicalPages / XMemMapPhysicalPagesScatter (remap or unmap), XMemFreePhysicalPages (of a page mapped inside a
// pin), an explicit-address XMemVirtualAlloc or title VirtualAlloc over a pin, and the title's VirtualFree,
// VirtualProtect, UnmapViewOfFile and UnmapViewOfFileEx over a pin (a whole-allocation release or view unmap is tested
// over its whole extent). XMemFree / XMemFreeDefault (no result) keep the block, and its ownership, until the last pin
// over it is gone. Memory changed behind the shims (a raw host or NT call, another process) is outside this contract.
//
// Lock order: xmem's physical-page lock, then its block lock, then this registry's lock. The provider takes all three,
// so ownership, aliasing and the overlap check are one decision, and every refusal above holds this registry's lock
// across the host call it guards.
//
// Header-only so the static Vulkan library and the loader's xmem shim share one registry without a new link
// dependency: registry() is an inline function with one static instance per program.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <new>

namespace rt::host_pins {

enum class PinResult {
    pinned,         // recorded; unpin() with the same range releases it
    invalid,        // an empty or wrapping range
    overlaps,       // part of the range is already pinned (by another import)
    aliases,        // a physical page of the range already backs another pin at another address
    unmanaged,      // the range is not wholly inside one allocation the title shims own (see above)
    no_provider,    // no lifetime provider is linked: nothing would enforce the pin
    out_of_memory,  // recording the pin failed; nothing was recorded
};

using PinProvider = PinResult (*)(std::uintptr_t begin, std::uintptr_t end);
using UnpinHook = void (*)();

struct Registry {
    std::mutex mutex;
    std::map<std::uintptr_t, std::uintptr_t> pins;  // begin -> end; the ranges are disjoint
    std::atomic<PinProvider> provider{nullptr};
    std::atomic<UnpinHook> after_unpin{nullptr};
};

inline Registry& registry() {
    static Registry instance;
    return instance;
}

// With registry().mutex held: true when [begin, end) overlaps any pin.
inline bool overlaps_locked(std::uintptr_t begin, std::uintptr_t end) {
    if (begin >= end) return false;
    const auto& pins = registry().pins;
    auto it = pins.upper_bound(begin);
    if (it != pins.begin() && std::prev(it)->second > begin) return true;
    return it != pins.end() && it->first < end;
}

// With registry().mutex held: records the pin after the overlap check. For the provider only, after its ownership
// check; an allocation failure records nothing.
inline PinResult insert_locked(std::uintptr_t begin, std::uintptr_t end) {
    if (begin >= end) return PinResult::invalid;
    if (overlaps_locked(begin, end)) return PinResult::overlaps;
    try {
        registry().pins.emplace(begin, end);
    } catch (const std::bad_alloc&) {
        return PinResult::out_of_memory;
    }
    return PinResult::pinned;
}

// Only through the provider: with none linked the range is refused rather than recorded unenforced.
inline PinResult pin(std::uintptr_t begin, std::uintptr_t end) {
    if (begin >= end) return PinResult::invalid;
    const PinProvider provider = registry().provider.load(std::memory_order_acquire);
    if (!provider) return PinResult::no_provider;
    return provider(begin, end);
}

// False when no pin has exactly this range. The hook (xmem's deferred frees) runs with no lock held.
inline bool unpin(std::uintptr_t begin, std::uintptr_t end) {
    {
        std::lock_guard lock(registry().mutex);
        auto& pins = registry().pins;
        const auto it = pins.find(begin);
        if (it == pins.end() || it->second != end) return false;
        pins.erase(it);
    }
    if (const UnpinHook hook = registry().after_unpin.load(std::memory_order_acquire)) hook();
    return true;
}

inline bool is_pinned(std::uintptr_t begin, std::uintptr_t end) {
    std::lock_guard lock(registry().mutex);
    return overlaps_locked(begin, end);
}

inline std::size_t pin_count() {
    std::lock_guard lock(registry().mutex);
    return registry().pins.size();
}

}  // namespace rt::host_pins
