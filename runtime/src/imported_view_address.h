#pragma once

// Palmwave: the V# base address of a RAW buffer view of an imported host buffer (Xbox device slot 133).
//
// The translator's buffer contract (tools/rdna2spirv.py: buffer_address, the s_load/s_buffer_load path and host_addr;
// docs/d3d12_vulkan.md section 3.3). A MUBUF access to byte `o` of a V# reads the host device address
//
//     heap_base + zext64(base48) + o        (64-bit arithmetic, mod 2^64)
//
// where base48 = word0 | word1[15:0] << 32 is zero-extended, heap_base is the GPU address arena's device address (the
// same push-constant value for every resource), and for a RAW view (stride 0, OOB_SELECT 3) `o` is a 32-bit offset
// that is accessed only when o + bytes <= NUM_RECORDS. s_buffer_load reads the same field, with its low two bits
// cleared (lo & ~3).
//
// A slot-133 buffer is its own VkBuffer at device address B, outside the arena. Its GPU VA (GetGPUVirtualAddress) is
// the title's host address, which identifies it in the CPU-side VA index, but heap_base + that VA is not B. Writing
//
//     base48 = (B - heap_base) + view_offset
//
// makes every access of the view land on B + view_offset + o, the view's own byte, exactly, when:
//
//   1. heap_base != 0 and B != 0 (there is an arena and the buffer has a device address);
//   2. the view [view_offset, view_offset + view_size) lies inside the buffer's width, and B + width does not pass 2^64;
//   3. heap_base < B, so B - heap_base does not wrap (B == heap_base would be the arena's own first byte);
//   4. (B - heap_base) + view_offset + view_size <= 2^47, and (B - heap_base) + view_offset < 2^47;
//   5. (B - heap_base) and the resource's GPU VA agree in their low two bits, so s_buffer_load's dword truncation
//      selects the same bytes the console's V# (base = GPU VA + view_offset) would.
//
// The bound in 4 is 2^47, not 2^48: every byte the view can reach then has address bit 47 clear, so the field means the
// same whether a consumer zero-extends it (this translator) or sign-extends it (the GFX10 canonical 48-bit VA), and
// a shader that adds an in-view offset to the base in SGPRs never carries into word1's STRIDE bits. Nothing is
// truncated or wrapped: a view that fails any condition is refused, and `out` is left as it was.
//
// What this does NOT cover: B <= heap_base (a negative difference has no zero-extended 48-bit encoding; it would need
// the translator to sign-extend bit 47, a contract change), structured and typed views, UAV counters, descriptors the
// title writes itself, and root or other raw VAs a shader reads that hold the title's host address.
//
// Pure arithmetic, so it is tested without a GPU (imported_view_address_test).

#include <cstdint>

namespace palmwave {

inline constexpr std::uint64_t kImportedViewLimit = std::uint64_t(1) << 47;

enum class ImportedViewAddress : std::uint8_t {
    ok,
    no_heap_base,       // 1: no arena, so the shader's heap_base names no buffer
    no_device_address,  // 1: the buffer has no device address
    outside_buffer,     // 2: the view's bytes are not inside the buffer
    buffer_wraps,       // 2: B + width passes 2^64
    below_heap_base,    // 3: B <= heap_base
    past_limit,         // 4: the view reaches 2^47 or beyond
    misaligned,         // 5: B - heap_base and the GPU VA differ in their low two bits
};

// The base48 for the view [offset, offset + size) of a buffer of `width` bytes at device address `device_address`,
// whose GPU VA is `gpu_va`, given the arena's `heap_base`. On `ok`, `out` is the value to write in the V#; on any
// other result `out` is unchanged.
inline ImportedViewAddress imported_view_address(std::uint64_t heap_base, std::uint64_t device_address,
                                                 std::uint64_t gpu_va, std::uint64_t width, std::uint64_t offset,
                                                 std::uint64_t size, std::uint64_t& out) {
    if (!heap_base) return ImportedViewAddress::no_heap_base;
    if (!device_address) return ImportedViewAddress::no_device_address;
    if (offset > width || size > width - offset) return ImportedViewAddress::outside_buffer;
    if (width > UINT64_MAX - device_address) return ImportedViewAddress::buffer_wraps;
    if (device_address <= heap_base) return ImportedViewAddress::below_heap_base;
    const std::uint64_t delta = device_address - heap_base;
    if (delta >= kImportedViewLimit || offset >= kImportedViewLimit - delta ||
        size > kImportedViewLimit - delta - offset)
        return ImportedViewAddress::past_limit;
    if ((delta ^ gpu_va) & 3) return ImportedViewAddress::misaligned;
    out = delta + offset;
    return ImportedViewAddress::ok;
}

// A one-line reason for a refusal, for the runtime's log.
inline const char* imported_view_refusal(ImportedViewAddress result) {
    switch (result) {
        case ImportedViewAddress::ok: return "representable";
        case ImportedViewAddress::no_heap_base: return "the device has no GPU address arena (no heap_base)";
        case ImportedViewAddress::no_device_address: return "the buffer has no device address";
        case ImportedViewAddress::outside_buffer: return "the view is not inside the buffer";
        case ImportedViewAddress::buffer_wraps: return "the buffer's device address range wraps 2^64";
        case ImportedViewAddress::below_heap_base:
            return "the buffer's device address is at or below heap_base (a negative V# base is not representable)";
        case ImportedViewAddress::past_limit: return "the view reaches 2^47 bytes past heap_base";
        case ImportedViewAddress::misaligned:
            return "the device address minus heap_base and the GPU VA differ in their low two bits";
    }
    return "unknown";
}

}  // namespace palmwave
