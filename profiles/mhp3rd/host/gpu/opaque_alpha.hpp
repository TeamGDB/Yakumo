#pragma once

#include <cstddef>
#include <cstdint>

namespace mhp3rd::gpu {

// A proof about the complete decoded image, including texels outside this
// draw's UV range. Empty, missing or partially transparent images stay unknown.
inline bool opaque_pixels(const std::uint32_t *data, std::size_t count) {
    if (data == nullptr || count == 0u) return false;
    for (std::size_t i = 0u; i < count; ++i)
        if ((data[i] >> 24u) != 255u) return false;
    return true;
}

// ge.vert uses global ambient alpha times the selected ambient material alpha
// when lit, and the converted vertex alpha otherwise. The caller supplies a
// proof for every converted vertex used by the draw.
inline bool opaque_vertex_alpha(bool lit, bool vertices_opaque, bool has_vertex_color, std::uint32_t material_update,
    std::uint32_t ambient_alpha, std::uint32_t material_color) {
    if (!lit) return vertices_opaque;
    if (ambient_alpha != 255u) return false;
    return has_vertex_color && (material_update & 1u) != 0u ? vertices_opaque : (material_color >> 24u) == 255u;
}

// Alpha follows ge.frag: modulate/blend multiply both alphas, decal keeps the
// vertex alpha, and replace/add currently take the sampled alpha. Unknown
// enabled texture functions are deliberately not optimized.
inline bool opaque_fragment_alpha(
    bool texture_enabled, std::uint32_t function, bool vertex_opaque, bool texture_opaque) {
    if (!texture_enabled) return vertex_opaque;
    switch (function) {
    case 0u:
    case 2u:
        return vertex_opaque && texture_opaque;
    case 1u:
        return vertex_opaque;
    case 3u:
    case 4u:
        return texture_opaque;
    default:
        return false;
    }
}

// Opaque inputs alone do not prove an exact fragment alpha: perspective
// interpolation can round a constant vertex alpha below one. Keep its blend
// and comparison path. Through draws avoid that division; replace/add take
// alpha directly from the sampled opaque image instead of the varying color.
inline bool opaque_alpha_is_exact(bool through, bool texture_enabled, std::uint32_t function) {
    return through || (texture_enabled && (function == 3u || function == 4u));
}

// Whether ge.frag's existing predicate passes at alpha exactly one. Preserve
// enabled tests with invalid state or a partial mask, including ALWAYS.
inline bool opaque_alpha_test_passes(
    bool enabled, std::uint32_t function, std::uint32_t reference, std::uint32_t mask) {
    if (!enabled) return true;
    if (function > 7u || reference > 255u || mask != 255u) return false;
    const float normalized_reference = static_cast<float>(reference) / 255.0f;
    const float distance = 1.0f - normalized_reference;
    switch (function) {
    case 0u:
        return true;
    case 1u:
        return false;
    case 2u:
        return distance < 0.002f;
    case 3u:
        return distance >= 0.002f;
    case 4u:
        return 1.0f < normalized_reference;
    case 5u:
        return 1.0f <= normalized_reference;
    case 6u:
        return 1.0f > normalized_reference;
    default: // Validated above: function 7, GEQUAL.
        return 1.0f >= normalized_reference;
    }
}

// With proven source alpha one, this RGB blend is source * 1 + destination * 0.
// Vulkan already uses ONE/ZERO/ADD for alpha. Other factors/equations stay intact.
inline bool redundant_opaque_blend(
    bool enabled, std::uint32_t source, std::uint32_t destination, std::uint32_t equation) {
    return enabled && source == 2u && destination == 3u && equation == 0u;
}

} // namespace mhp3rd::gpu
