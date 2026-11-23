// The strict console PresentX wire check used by the presentation bridge (xbox_present_bridge.h). Header-only, no
// Vulkan: a CPU test checks it alone (runtime/test/xbox_present_wire_test.cpp).
//
// PROVENANCE (public sources; independently written here, no code copied):
//   - D3D12XBOX_PRESENT_PLANE_PARAMETERS field order and types: hajimehoshi/ebiten (Apache-2.0),
//     internal/graphicsdriver/directx/d3d12_windows.go at d438df689b19544c82543ad522b36be86a5a60b7, lines 866-878;
//     the token is a uint64 (line 461) and TOKEN_NULL is 0 (line 464). Natural x64 alignment of those fields gives
//     the offsets below and a 0x48-byte struct.
//   - D3D12XBOX_PRESENT_PARAMETERS: the same file, lines 846-853 (float, uint32, uint32, pointer, flags: 0x20 bytes).
//   - Use pattern (zeroed struct; Token, ResourceCount, ppResources; pSrcViewRects only for HDR): Microsoft
//     directxtk12modelviewer DeviceResourcesGXDK.cpp (MIT) at 37e6552a5e479b6f128f3f412fccfcae2447a318, lines 420-440.
//     Lines 55-58 and 224-225 explicitly use PresentX(0, nullptr, nullptr) to present a blank screen before resource
//     cleanup. Black is the existing backend's representation of that blank; the sample does not prescribe RGB.
//     Blank presents are counted separately and do not establish a first frame from a title resource.
//   - FiveStar (GPL-3.0-or-later) at 18e225d2cbd526ba710beb69b7884d6c758d9291, PresentPlane.cs, describes the same
//     72-byte plane with the same offsets through +0x28, and PresentX(count, planes, parameters).
// This is a public-source contract. It is NOT a measurement of the local retail title's planes: no local plane
// has been confirmed. OpenScarlett's plane struct (inline rectangles) disagrees with all three and is not used.
//
// THE ACCEPTED SUBSET (anything else is refused before any presenter call; nothing is masked, coerced or dropped):
//   plane_count 0  a blank frame (the samples' shutdown PresentX(0, nullptr, nullptr)); planes is not read.
//   plane_count 1  Token != 0; ResourceCount == 1; ppResources and ppResources[0] non-null; pSrcViewRects null or a
//                  rectangle with 0 <= left < right and 0 <= top < bottom (the bridge also checks it fits the texture);
//                  pDestPlacementBase null; ColorSpace 0 (DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, SDR); ScaleFilter
//                  0; ExtendedDescCount 0 and pExtendedDescs null; Flags 0. Padding (+0x0C, +0x34, +0x44) is not read.
//   params         null, or every field zero: ImmediateThresholdPercent bits 0 (+0.0f), ViewCount 0,
//                  ExtendedDescCount 0, pExtendedDescs null, Flags 0. The padding at +0x0C/+0x1C is not read.
//   plane_count 2  refused (E_NOTIMPL): the HDR/GameDVR pair is not in the subset. Above 2: E_INVALIDARG.
// The caller's memory is copied, never written. The plane count is the UINT of the prototype (upper half ignored).
#pragma once

#include <windows.h>
#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rt::d3d12 {

struct XboxPresentPlaneWire {
    std::uint64_t Token;              // +0x00 D3D12XBOX_FRAME_PIPELINE_TOKEN
    std::uint32_t ResourceCount;      // +0x08
    std::uint32_t padding0;           // +0x0C (not read)
    std::uint64_t ppResources;        // +0x10 ID3D12Resource* const*
    std::uint64_t pSrcViewRects;      // +0x18 const D3D12_RECT*
    std::uint64_t pDestPlacementBase; // +0x20 const D3D12XBOX_VIEW_RECT*
    std::int32_t ColorSpace;          // +0x28 DXGI_COLOR_SPACE_TYPE
    std::int32_t ScaleFilter;         // +0x2C
    std::uint32_t ExtendedDescCount;  // +0x30
    std::uint32_t padding1;           // +0x34 (not read)
    std::uint64_t pExtendedDescs;     // +0x38
    std::uint32_t Flags;              // +0x40
    std::uint32_t padding2;           // +0x44 (not read)
};
static_assert(sizeof(XboxPresentPlaneWire) == 0x48);
static_assert(offsetof(XboxPresentPlaneWire, ResourceCount) == 0x08);
static_assert(offsetof(XboxPresentPlaneWire, ppResources) == 0x10);
static_assert(offsetof(XboxPresentPlaneWire, pSrcViewRects) == 0x18);
static_assert(offsetof(XboxPresentPlaneWire, pDestPlacementBase) == 0x20);
static_assert(offsetof(XboxPresentPlaneWire, ColorSpace) == 0x28);
static_assert(offsetof(XboxPresentPlaneWire, ScaleFilter) == 0x2C);
static_assert(offsetof(XboxPresentPlaneWire, ExtendedDescCount) == 0x30);
static_assert(offsetof(XboxPresentPlaneWire, pExtendedDescs) == 0x38);
static_assert(offsetof(XboxPresentPlaneWire, Flags) == 0x40);

struct XboxPresentParametersWire {
    float ImmediateThresholdPercent;  // +0x00
    std::uint32_t ViewCount;          // +0x04
    std::uint32_t ExtendedDescCount;  // +0x08
    std::uint32_t padding0;           // +0x0C (not read)
    std::uint64_t pExtendedDescs;     // +0x10
    std::uint32_t Flags;              // +0x18
    std::uint32_t padding1;           // +0x1C (not read)
};
static_assert(sizeof(XboxPresentParametersWire) == 0x20);
static_assert(offsetof(XboxPresentParametersWire, ViewCount) == 0x04);
static_assert(offsetof(XboxPresentParametersWire, ExtendedDescCount) == 0x08);
static_assert(offsetof(XboxPresentParametersWire, pExtendedDescs) == 0x10);
static_assert(offsetof(XboxPresentParametersWire, Flags) == 0x18);

// What a request that passed the check asks for.
struct XboxPresentWireRequest {
    bool blank = false;                 // plane_count 0
    std::uint64_t token = 0;
    ID3D12Resource* resource = nullptr; // ppResources[0], no reference added
    bool has_source_rect = false;
    D3D12_RECT source_rect{};
};

struct XboxPresentWireCheck {
    HRESULT hr = E_FAIL;        // S_OK: `request` is filled; otherwise the refusal to return, nothing presented
    const char* reason = "";    // fixed text for the log line
    XboxPresentWireRequest request;
};

namespace present_wire_detail {
inline XboxPresentWireCheck refuse(HRESULT hr, const char* reason) {
    XboxPresentWireCheck check;
    check.hr = hr;
    check.reason = reason;
    return check;
}
}  // namespace present_wire_detail

inline XboxPresentWireCheck check_present_wire(std::uint64_t plane_count_word, std::uint64_t planes_word,
                                               std::uint64_t params_word) {
    using present_wire_detail::refuse;
    const auto plane_count = static_cast<std::uint32_t>(plane_count_word);
    if (params_word) {
        XboxPresentParametersWire params;
        std::memcpy(&params, reinterpret_cast<const void*>(params_word), sizeof(params));
        std::uint32_t threshold_bits = 0;
        std::memcpy(&threshold_bits, &params.ImmediateThresholdPercent, sizeof(threshold_bits));
        if (threshold_bits != 0) return refuse(E_NOTIMPL, "params ImmediateThresholdPercent is not 0");
        if (params.ViewCount != 0) return refuse(E_NOTIMPL, "params ViewCount is not 0");
        if (params.ExtendedDescCount != 0 || params.pExtendedDescs != 0)
            return refuse(E_NOTIMPL, "params extended descriptors are not supported");
        if (params.Flags != 0) return refuse(E_NOTIMPL, "params Flags is not 0");
    }
    if (plane_count == 0) {
        XboxPresentWireCheck check;
        check.hr = S_OK;
        check.reason = "blank";
        check.request.blank = true;
        return check;
    }
    if (plane_count > 2) return refuse(E_INVALIDARG, "more than 2 planes");
    if (plane_count != 1) return refuse(E_NOTIMPL, "2 planes (HDR/GameDVR pair) are not in the supported subset");
    if (!planes_word) return refuse(E_INVALIDARG, "plane array is null");

    XboxPresentPlaneWire plane;
    std::memcpy(&plane, reinterpret_cast<const void*>(planes_word), sizeof(plane));
    if (plane.Token == 0) return refuse(E_NOTIMPL, "plane Token is TOKEN_NULL (behaviour not established)");
    if (plane.ResourceCount != 1)
        return refuse(plane.ResourceCount == 0 ? E_INVALIDARG : E_NOTIMPL, "plane ResourceCount is not 1");
    if (!plane.ppResources) return refuse(E_INVALIDARG, "plane ppResources is null");
    if (plane.pDestPlacementBase) return refuse(E_NOTIMPL, "plane pDestPlacementBase is not null");
    if (plane.ColorSpace != 0) return refuse(E_NOTIMPL, "plane ColorSpace is not 0 (RGB_FULL_G22_NONE_P709)");
    if (plane.ScaleFilter != 0) return refuse(E_NOTIMPL, "plane ScaleFilter is not 0");
    if (plane.ExtendedDescCount != 0 || plane.pExtendedDescs != 0)
        return refuse(E_NOTIMPL, "plane extended descriptors are not supported");
    if (plane.Flags != 0) return refuse(E_NOTIMPL, "plane Flags is not 0");

    ID3D12Resource* resource = nullptr;
    std::memcpy(&resource, reinterpret_cast<const void*>(plane.ppResources), sizeof(resource));
    if (!resource) return refuse(E_INVALIDARG, "plane ppResources[0] is null");

    XboxPresentWireCheck check;
    if (plane.pSrcViewRects) {
        D3D12_RECT rect;
        std::memcpy(&rect, reinterpret_cast<const void*>(plane.pSrcViewRects), sizeof(rect));
        if (rect.left < 0 || rect.top < 0 || rect.left >= rect.right || rect.top >= rect.bottom)
            return refuse(E_INVALIDARG, "plane source rectangle is empty or negative");
        check.request.has_source_rect = true;
        check.request.source_rect = rect;
    }
    check.hr = S_OK;
    check.reason = "single plane";
    check.request.token = plane.Token;
    check.request.resource = resource;
    return check;
}

}  // namespace rt::d3d12
