// Palmwave: the V# base of a RAW view of an imported host buffer (src/imported_view_address.h), without a GPU.
// Synthetic addresses only; no game data. Each accepted case is encoded into a GFX10 V# (src/gfx10_descriptors.h),
// decoded back, and pushed through the translator's address formula, heap_base + sext64(base48) + o
// (tools/rdna2spirv.py, buffer_address and host_addr), for the view's first and last dword: the result must be the
// buffer's device address plus the view offset plus o. Each refused case must leave the output untouched.
#include "../src/imported_view_address.h"
#include "../src/gfx10_descriptors.h"

#include <cstdint>
#include <cstdio>

#define CHECK(condition) do {                                    \
    if (!(condition)) {                                          \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        return 1;                                                \
    }                                                            \
} while (0)

using namespace palmwave;

namespace {

constexpr std::uint64_t kSentinel = 0xD1CEDA7A5EED0001ull;
constexpr std::uint64_t kField48 = (std::uint64_t(1) << 48) - 1;

// What a translated shader reads for byte `o` of a V# holding `base` (the field is 48 bits, sign-extended).
std::uint64_t shader_address(std::uint64_t heap_base, const gfx10::BufferWords& words, std::uint64_t o) {
    gfx10::Buffer decoded{};
    if (!gfx10::decode(words, decoded)) return 0;
    const std::uint64_t address = decoded.address & kField48;
    const std::uint64_t extended = address & (std::uint64_t(1) << 47) ? address | ~kField48 : address;
    return heap_base + extended + o;
}

gfx10::BufferWords raw_view(std::uint64_t base, std::uint32_t bytes) {
    gfx10::Buffer buffer{};
    buffer.address = base;
    buffer.num_records = bytes;
    buffer.format = 0;  // an actual RAW V#, as ClearUAV recognizes with stride == 0
    buffer.oob_select = gfx10::OobSelect::raw;
    return gfx10::encode(buffer);
}

// A refusal: the expected result and `out` untouched.
bool refused(ImportedViewAddress want, std::uint64_t heap_base, std::uint64_t b, std::uint64_t va, std::uint64_t width,
             std::uint64_t offset, std::uint64_t size) {
    std::uint64_t out = kSentinel;
    const ImportedViewAddress got = imported_view_address(heap_base, b, va, width, offset, size, out);
    if (got == want && out == kSentinel) return true;
    std::fprintf(stderr, "  heap 0x%llx B 0x%llx width 0x%llx [0x%llx, +0x%llx): result %d (%s), out 0x%llx\n",
                 (unsigned long long)heap_base, (unsigned long long)b, (unsigned long long)width,
                 (unsigned long long)offset, (unsigned long long)size, int(got), imported_view_refusal(got),
                 (unsigned long long)out);
    return false;
}

int representable() {
    // A title host VA (what GetGPUVirtualAddress returns for a slot-133 buffer), an arena and an import above it.
    const std::uint64_t heap_base = 0x0000001234000000ull;
    const std::uint64_t b = 0x0000004567800000ull;
    const std::uint64_t title_va = 0x00007ff612340000ull;
    const std::uint64_t width = 0x20000, offset = 0x100, size = 0x400;
    std::uint64_t out = kSentinel;
    CHECK(imported_view_address(heap_base, b, title_va, width, offset, size, out) == ImportedViewAddress::ok);
    CHECK(out == b - heap_base + offset);
    const gfx10::BufferWords words = raw_view(out, std::uint32_t(size));
    CHECK(shader_address(heap_base, words, 0) == b + offset);
    CHECK(shader_address(heap_base, words, size - 4) == b + offset + size - 4);
    // The V# the backend wrote before: the title VA plus the offset. The shader adds heap_base to it and misses B.
    const gfx10::BufferWords old_words = raw_view(title_va + offset, std::uint32_t(size));
    CHECK(shader_address(heap_base, old_words, 0) != b + offset);

    // An arena resource in the same table keeps its console VA (an offset in the arena): one heap_base serves both.
    const std::uint64_t arena_va = 0x10000;
    CHECK(shader_address(heap_base, raw_view(arena_va, 0x400), 0) == heap_base + arena_va);
    return 0;
}

int bounds() {
    const std::uint64_t heap_base = 0x0000000100000000ull;
    const std::uint64_t va = 0x0000020000000000ull;
    // The view ends exactly at 2^47 past heap_base: accepted, and the 48-bit field holds it without truncation.
    {
        const std::uint64_t b = heap_base + kImportedViewLimit - 0x1000;
        std::uint64_t out = kSentinel;
        CHECK(imported_view_address(heap_base, b, va, 0x1000, 0, 0x1000, out) == ImportedViewAddress::ok);
        CHECK(out == kImportedViewLimit - 0x1000);
        const gfx10::BufferWords words = raw_view(out, 0x1000);
        gfx10::Buffer decoded{};
        CHECK(gfx10::decode(words, decoded) && decoded.address == out);
        CHECK(shader_address(heap_base, words, 0xFFC) == b + 0xFFC);
        // One byte more reaches bit 47.
        CHECK(refused(ImportedViewAddress::past_limit, heap_base, b, va, 0x1004, 0, 0x1004));
        // An empty view whose base would be 2^47 is refused too: the field must keep bit 47 clear.
        CHECK(refused(ImportedViewAddress::past_limit, heap_base, b, va, 0x1000, 0x1000, 0));
    }
    // A difference at 2^47 or anywhere in [2^47, 2^48) is refused; so is one that would carry past 48 bits.
    CHECK(refused(ImportedViewAddress::past_limit, heap_base, heap_base + kImportedViewLimit, va, 0x100, 0, 4));
    CHECK(refused(ImportedViewAddress::past_limit, heap_base, heap_base + (std::uint64_t(1) << 48) - 4, va, 8, 0, 8));
    CHECK(refused(ImportedViewAddress::past_limit, heap_base, heap_base + (std::uint64_t(1) << 52), va, 0x100, 0, 4));
    // The offset alone can carry the base past the bound.
    CHECK(refused(ImportedViewAddress::past_limit, heap_base, heap_base + kImportedViewLimit - 0x100, va, 0x1000,
                  0x200, 4));
    return 0;
}

int negative() {
    const std::uint64_t heap_base = 0x400000000ull, b = 0x20000000ull;
    const std::uint64_t va = 0x0000020000000000ull;
    const std::uint64_t offset = 0x100, size = 0x400;
    std::uint64_t out = kSentinel, base = kSentinel;
    CHECK(imported_view_address(heap_base, b, va, 0x10000, offset, size, out) == ImportedViewAddress::ok);
    CHECK(imported_view_address(heap_base, b, va, 0x10000, 0, 0, base) == ImportedViewAddress::ok);
    CHECK((out & (std::uint64_t(1) << 47)) != 0);
    CHECK(out <= kField48);
    CHECK(heap_base + out != b + offset);  // the previous unsigned shader contract is demonstrably wrong
    const auto words = raw_view(out, std::uint32_t(size));
    CHECK(shader_address(heap_base, words, 0) == b + offset);
    CHECK(shader_address(heap_base, words, size - 4) == b + offset + size - 4);
    CHECK(out >= base && out - base == offset);  // ClearUAV's exact encoded-base subtraction
    CHECK(words[1] >> 16 == 0);  // encoding did not contaminate STRIDE/control fields

    // Lowest signed value is accepted, one dword below it is not. Both are real positive device addresses.
    const std::uint64_t high_heap = kImportedViewLimit + 0x10000000;
    CHECK(imported_view_address(high_heap, 0x10000000, va, 0x1000, 0, 0x1000, out) == ImportedViewAddress::ok);
    CHECK(out == kImportedViewLimit);
    CHECK(shader_address(high_heap, raw_view(out, 0x1000), 0xFFC) == 0x10000FFC);
    CHECK(refused(ImportedViewAddress::past_limit, high_heap, 0x0FFFFFFC, va, 0x1000, 0, 4));
    // Ending at zero is safe; crossing zero would carry into descriptor STRIDE when the base is adjusted in SGPRs.
    CHECK(imported_view_address(heap_base, heap_base - 0x1000, va, 0x2000, 0, 0x1000, out) == ImportedViewAddress::ok);
    CHECK(refused(ImportedViewAddress::past_limit, heap_base, heap_base - 0x1000, va, 0x2000, 0, 0x1004));
    CHECK(refused(ImportedViewAddress::past_limit, heap_base, heap_base - 0x1000, va, 0x2000, 0x1000, 0));
    CHECK(refused(ImportedViewAddress::below_heap_base, heap_base, heap_base, va, 0x1000, 0, 4));
    CHECK(refused(ImportedViewAddress::misaligned, heap_base, b + 2, va, 0x1000, 0, 4));
    CHECK(imported_view_address(heap_base, b + 2, va + 2, 0x1000, 0, 4, out) == ImportedViewAddress::ok);
    CHECK(shader_address(heap_base, raw_view(out & ~3ull, 4), 0) == b);  // scalar base rounding
    return 0;
}

int malformed() {
    const std::uint64_t heap_base = 0x0000000100000000ull, b = 0x0000000200000000ull, va = 0x10000000000ull;
    CHECK(refused(ImportedViewAddress::no_heap_base, 0, b, va, 0x1000, 0, 4));
    CHECK(refused(ImportedViewAddress::no_device_address, heap_base, 0, va, 0x1000, 0, 4));
    CHECK(refused(ImportedViewAddress::outside_buffer, heap_base, b, va, 0x1000, 0x1004, 0));
    CHECK(refused(ImportedViewAddress::outside_buffer, heap_base, b, va, 0x1000, 0x800, 0x804));
    CHECK(refused(ImportedViewAddress::outside_buffer, heap_base, b, va, 0x1000, UINT64_MAX, 4));
    CHECK(refused(ImportedViewAddress::outside_buffer, heap_base, b, va, 0x1000, 4, UINT64_MAX));
    // B + width passes 2^64 (checked before the difference, so it cannot be mistaken for a small view).
    CHECK(refused(ImportedViewAddress::buffer_wraps, heap_base, UINT64_MAX - 0xFFF, va, 0x2000, 0, 4));
    // The low two bits: s_buffer_load clears them from the base, so B - heap_base must agree with the GPU VA there.
    CHECK(refused(ImportedViewAddress::misaligned, heap_base, b + 2, va, 0x1000, 0, 4));
    std::uint64_t out = kSentinel;
    CHECK(imported_view_address(heap_base, b + 2, va + 2, 0x1000, 0, 4, out) == ImportedViewAddress::ok);
    CHECK(out == b + 2 - heap_base);
    // Every reason has a message.
    for (int r = 0; r <= int(ImportedViewAddress::misaligned); ++r)
        CHECK(imported_view_refusal(ImportedViewAddress(r))[0] != 'u');
    return 0;
}

}  // namespace

int main() {
    const int failures = representable() + bounds() + negative() + malformed();
    if (failures) {
        std::fprintf(stderr, "imported_view_address_test: %d group(s) failed\n", failures);
        return 1;
    }
    std::printf("imported_view_address_test: representable views reach B + offset through the 48-bit V# field; "
                "negative and positive bases are exact; signed-range overflow, zero crossing, wrap, malformed and misaligned views are refused with the output "
                "untouched\n");
    return 0;
}
