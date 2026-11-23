// PALMWAVE_EXPERIMENTAL_XBOX_HOST_IMPORT: one explicitly opt-in DIAGNOSTIC EXPERIMENT. Nothing here is a measured console
// ABI or an established meaning of any console bit; see docs/d3d12_vulkan.md section 4.2, "Experimental host import".
//
// The variable must be exactly "1". Unset, empty, "0" or any other value leaves the strict behaviour unchanged. It is
// read once per process image (a function-local static) and never re-read, so it cannot change while mappings or
// imports exist; xmem.cpp reads it during static initialisation, before any title mapping.
//
// With it set, and only then, two things change:
//   - Xbox device slot 133 (d3d12_xs_device.cpp): the ONE bounded call shape seen in public upstream observations (a
//     plain row-major BUFFER, Alignment 0, flags 0x00020004, third word 0xFFFFFFFF, no clear value) is handed to the
//     existing host-memory importer as a host description with flags ALLOW_UNORDERED_ACCESS (0x4) and the state word
//     COMMON (0). Dropping 0x20000 and reading 0xFFFFFFFF as "no state" are HYPOTHESES. Every such attempt is logged
//     with the original and host values and recorded on the D3D12 trace with an "experimental" outcome.
//   - xmem physical-page views (xmem.cpp): a view created for a window whose protection asks for PAGE_WRITECOMBINE or
//     PAGE_NOCACHE is created as a normal cached view (the modifier is removed from the view protection only; the
//     window's recorded protection is kept). This follows upstream #7492 (hcristosm, 954fcb10, GPL-3.0) and is not an
//     emulation of console cache control.
#pragma once

#include <windows.h>

namespace rt::experimental_host_import {

inline constexpr char kVariable[] = "PALMWAVE_EXPERIMENTAL_XBOX_HOST_IMPORT";

// True only for the exact value "1". GetEnvironmentVariableA returns the length without the terminator when the value
// fits (1 for "1") and the size it needs when it does not, so "10" or "1 " are not "1".
inline bool read_enabled() {
    char value[4] = {};
    const DWORD length = GetEnvironmentVariableA(kVariable, value, sizeof(value));
    return length == 1 && value[0] == '1';
}

// Read once, then fixed for the life of the process image.
inline bool enabled() {
    static const bool on = read_enabled();
    return on;
}

// The cache modifiers the experiment removes from an xmem physical-page view's protection. Everything else, the
// access bits (read-only stays read-only) and PAGE_GUARD included, is kept as it is.
inline constexpr DWORD kCacheModifiers = PAGE_WRITECOMBINE | PAGE_NOCACHE;

inline constexpr DWORD host_cache_view_protect(DWORD window_protect) {
    return window_protect & ~kCacheModifiers;
}

// Test hook, defined in xmem.cpp (only programs that link it may call it): the protection the xmem shim recorded for the
// physical-page window holding `address` (the title's requested protection after host_protect, unchanged by the
// experiment). False when no window holds it.
bool xmem_window_protection(const void* address, DWORD* protect);

}  // namespace rt::experimental_host_import
