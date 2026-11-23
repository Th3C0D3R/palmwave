#pragma once

// Palmwave normalized V# ABI, signed48-v1: API-created RAW views of imported host buffers.
// A translated V# access is heap_base + sext64(base48) + byte_offset, modulo 2^64.
// The two V# readers sign-extend before offsets; full-width s_load/root/global pointers are unchanged.
// This is the runtime's normalized contract, not a claim about unmeasured console address behavior.
//
// For a real VkBuffer at B, encode B - heap_base + view_offset only after proving its signed 48-bit
// representability. A negative view stays wholly below zero; a positive view stays below +2^47.
// Thus adding an in-view offset to the encoded descriptor never carries into word1's STRIDE bits.
// All bytes must lie in the actual buffer, B + width must not wrap, and the displacement must preserve
// the original CPU GPUVA's low two bits for scalar-load dword rounding. No address is repaired by truncation.
//
// GetGPUVirtualAddress and the VA registry retain the original CPU VA. ClearUAV subtracts the encoded
// zero-offset base from the encoded view base: the no-cross-zero rule makes this exactly the byte offset
// for either sign, without signed C++ overflow. Invalid cases leave out untouched.
// Typed/structured views, counters, CPU-authored descriptors and CPU raw/root pointers remain out of scope.
// Newly signed descriptors require newly translated signed48-v1 shaders; old unsigned modules are incompatible.

#include <cstdint>

namespace palmwave {

inline constexpr std::uint64_t kImportedViewLimit = std::uint64_t(1) << 47;

enum class ImportedViewAddress : std::uint8_t {
    ok,
    no_heap_base,       // 1: no arena, so the shader's heap_base names no buffer
    no_device_address,  // 1: the buffer has no device address
    outside_buffer,     // 2: the view's bytes are not inside the buffer
    buffer_wraps,       // 2: B + width passes 2^64
    below_heap_base,    // B == heap_base: reserved arena origin
    past_limit,         // outside signed 48 bits, or a negative encoded view carries through zero
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
    if (device_address == heap_base) return ImportedViewAddress::below_heap_base;
    std::uint64_t encoded = 0;
    if (device_address < heap_base) {
        const std::uint64_t distance = heap_base - device_address;
        if (distance > kImportedViewLimit || offset >= distance || size > distance - offset)
            return ImportedViewAddress::past_limit;
        // distance - offset is in [1, 2^47]. This constructs the exact two's-complement field;
        // it never discards significant address bits or relies on signed arithmetic overflow.
        encoded = (std::uint64_t(1) << 48) - (distance - offset);
    } else {
        const std::uint64_t delta = device_address - heap_base;
        if (delta >= kImportedViewLimit || offset >= kImportedViewLimit - delta ||
            size > kImportedViewLimit - delta - offset)
            return ImportedViewAddress::past_limit;
        encoded = delta + offset;
    }
    if (((device_address - heap_base) ^ gpu_va) & 3) return ImportedViewAddress::misaligned;
    out = encoded;
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
            return "the buffer's device address equals the reserved arena origin";
        case ImportedViewAddress::past_limit: return "the view exceeds signed 48-bit range or carries its encoded base through zero";
        case ImportedViewAddress::misaligned:
            return "the device address minus heap_base and the GPU VA differ in their low two bits";
    }
    return "unknown";
}

}  // namespace palmwave
