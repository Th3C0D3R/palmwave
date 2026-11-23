// The strict PresentX wire check (shims/xbox_present_wire.h) on hand-built structs: CPU only, no GPU, no game data.
// Checks the 0x48 plane and 0x20 parameter layouts byte by byte, the accepted subset, one refusal per unsupported
// field, and that the caller's bytes are never written.
#include "../shims/xbox_present_wire.h"

#include <cstdio>
#include <cstring>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "xbox_present_wire line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

using rt::d3d12::check_present_wire;
using rt::d3d12::XboxPresentPlaneWire;
using rt::d3d12::XboxPresentParametersWire;

std::uint64_t word(const void* p) { return reinterpret_cast<std::uint64_t>(p); }

template <class T>
void put(unsigned char* bytes, std::size_t offset, T value) { std::memcpy(bytes + offset, &value, sizeof(value)); }

}  // namespace

int main() {
    // A fake resource pointer: the check never dereferences the resource, it only reads ppResources[0].
    auto* const resource = reinterpret_cast<ID3D12Resource*>(std::uintptr_t{0x1000});
    ID3D12Resource* resources[1] = {resource};

    // The plane as raw bytes at the public-source offsets (not through the struct), with poisoned padding.
    alignas(8) unsigned char raw[0x48];
    std::memset(raw, 0, sizeof(raw));
    put<std::uint64_t>(raw, 0x00, 7);               // Token
    put<std::uint32_t>(raw, 0x08, 1);               // ResourceCount
    put<std::uint32_t>(raw, 0x0C, 0xA5A5A5A5u);     // padding: not read
    put<std::uint64_t>(raw, 0x10, word(resources)); // ppResources
    put<std::uint32_t>(raw, 0x34, 0xA5A5A5A5u);
    put<std::uint32_t>(raw, 0x44, 0xA5A5A5A5u);
    unsigned char before[sizeof(raw)];
    std::memcpy(before, raw, sizeof(raw));

    auto check = check_present_wire(1, word(raw), 0);
    EXPECT(check.hr == S_OK && !check.request.blank && check.request.token == 7 && check.request.resource == resource &&
           !check.request.has_source_rect);
    EXPECT(std::memcmp(before, raw, sizeof(raw)) == 0);  // nothing written
    // The plane count is a UINT: the upper half of the register word is not part of it.
    EXPECT(check_present_wire(0xFFFFFFFF00000001ull, word(raw), 0).hr == S_OK);

    // A source rectangle (validated for emptiness here, against the texture in the bridge).
    const D3D12_RECT rect{2, 3, 10, 9};
    put<std::uint64_t>(raw, 0x18, word(&rect));
    check = check_present_wire(1, word(raw), 0);
    EXPECT(check.hr == S_OK && check.request.has_source_rect && check.request.source_rect.left == 2 &&
           check.request.source_rect.bottom == 9);
    const D3D12_RECT empty{4, 4, 4, 8}, negative{-1, 0, 4, 4};
    put<std::uint64_t>(raw, 0x18, word(&empty));
    EXPECT(check_present_wire(1, word(raw), 0).hr == E_INVALIDARG);
    put<std::uint64_t>(raw, 0x18, word(&negative));
    EXPECT(check_present_wire(1, word(raw), 0).hr == E_INVALIDARG);
    put<std::uint64_t>(raw, 0x18, 0);

    // Each unsupported field alone is refused; restoring it accepts the plane again.
    struct Field { std::size_t offset; std::size_t size; std::uint64_t value; HRESULT expected; };
    const float placement[4] = {0, 0, 1, 1};
    const Field fields[] = {
        {0x00, 8, 0, E_NOTIMPL},                    // TOKEN_NULL
        {0x08, 4, 0, E_INVALIDARG},                 // ResourceCount 0
        {0x08, 4, 2, E_NOTIMPL},                    // ResourceCount 2
        {0x10, 8, 0, E_INVALIDARG},                 // ppResources null
        {0x20, 8, word(placement), E_NOTIMPL},      // pDestPlacementBase (even the whole output)
        {0x28, 4, 12, E_NOTIMPL},                   // ColorSpace HDR10
        {0x28, 4, 1, E_NOTIMPL},                    // ColorSpace scRGB
        {0x2C, 4, 1, E_NOTIMPL},                    // ScaleFilter
        {0x30, 4, 1, E_NOTIMPL},                    // ExtendedDescCount
        {0x38, 8, word(raw), E_NOTIMPL},            // pExtendedDescs
        {0x40, 4, 1, E_NOTIMPL},                    // Flags
        {0x40, 4, 0x80000000u, E_NOTIMPL},
    };
    for (const Field& f : fields) {
        unsigned char saved[8];
        std::memcpy(saved, raw + f.offset, f.size);
        std::memcpy(raw + f.offset, &f.value, f.size);
        const auto refused = check_present_wire(1, word(raw), 0);
        if (refused.hr != f.expected) {
            std::fprintf(stderr, "xbox_present_wire: field +0x%zx = 0x%llx gave 0x%08lx\n", f.offset,
                         static_cast<unsigned long long>(f.value), static_cast<unsigned long>(refused.hr));
            return 1;
        }
        EXPECT(refused.reason && *refused.reason);
        std::memcpy(raw + f.offset, saved, f.size);
    }
    EXPECT(check_present_wire(1, word(raw), 0).hr == S_OK);

    // ppResources[0] null.
    ID3D12Resource* none[1] = {nullptr};
    put<std::uint64_t>(raw, 0x10, word(none));
    EXPECT(check_present_wire(1, word(raw), 0).hr == E_INVALIDARG);
    put<std::uint64_t>(raw, 0x10, word(resources));

    // Plane counts: 0 is a blank frame (planes not read, even when non-null), 2 is not in the subset, 3 is invalid.
    check = check_present_wire(0, 0, 0);
    EXPECT(check.hr == S_OK && check.request.blank);
    EXPECT(check_present_wire(0, word(raw), 0).hr == S_OK);
    EXPECT(check_present_wire(2, word(raw), 0).hr == E_NOTIMPL);
    EXPECT(check_present_wire(3, word(raw), 0).hr == E_INVALIDARG);
    EXPECT(check_present_wire(1, 0, 0).hr == E_INVALIDARG);

    // Parameters: all-zero accepted (padding poisoned), every non-zero field refused, -0.0f included.
    alignas(8) unsigned char params[0x20];
    std::memset(params, 0, sizeof(params));
    put<std::uint32_t>(params, 0x0C, 0xA5A5A5A5u);
    put<std::uint32_t>(params, 0x1C, 0xA5A5A5A5u);
    EXPECT(check_present_wire(1, word(raw), word(params)).hr == S_OK);
    EXPECT(check_present_wire(0, 0, word(params)).hr == S_OK);
    const Field param_fields[] = {
        {0x00, 4, 0x42C80000u, E_NOTIMPL},  // 100.0f
        {0x00, 4, 0x80000000u, E_NOTIMPL},  // -0.0f
        {0x04, 4, 1, E_NOTIMPL},            // ViewCount
        {0x08, 4, 1, E_NOTIMPL},            // ExtendedDescCount
        {0x10, 8, word(raw), E_NOTIMPL},    // pExtendedDescs
        {0x18, 4, 1, E_NOTIMPL},            // Flags
    };
    for (const Field& f : param_fields) {
        unsigned char saved[8];
        std::memcpy(saved, params + f.offset, f.size);
        std::memcpy(params + f.offset, &f.value, f.size);
        EXPECT(check_present_wire(1, word(raw), word(params)).hr == f.expected);
        EXPECT(check_present_wire(0, 0, word(params)).hr == f.expected);  // a blank frame checks them too
        std::memcpy(params + f.offset, saved, f.size);
    }

    static_assert(sizeof(XboxPresentPlaneWire) == 0x48 && sizeof(XboxPresentParametersWire) == 0x20);
    std::printf("xbox_present_wire: layout, accepted subset and refusals ok\n");
    return 0;
}
