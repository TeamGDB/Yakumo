#pragma once

#include <cstdint>

namespace mhp3rd::gpu {

// The fragment shader receives normalized alpha in [0, 1]: vertex colors
// and textures are UNORM, lighting clamps its color, and texturing preserves
// or multiplies alpha. Only these comparisons pass for every such value.
// Keep masked and unknown comparisons on the existing shader path.
[[nodiscard]] inline bool requires_alpha_test(
    bool enabled, std::uint32_t function, std::uint32_t reference, std::uint32_t mask) {
    if (!enabled || function == 0u) return false;
    if (mask == 0xFFu && ((function == 5u && reference == 255u) || (function == 7u && reference == 0u))) return false;
    return true;
}

} // namespace mhp3rd::gpu
