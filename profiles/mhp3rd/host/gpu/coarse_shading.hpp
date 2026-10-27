#pragma once

#include <cstdint>

namespace mhp3rd::gpu {

// The internal resolution from which translucent effects are shaded in 2x2
// blocks: four times the PSP's 272 lines.
inline constexpr std::uint32_t kCoarseMinTargetHeight = 4u * 272u;

// Whether a draw is a translucent 3D effect that may be shaded once per 2x2
// pixels (VK_KHR_fragment_shading_rate). Smoke, snow, breath and dust are
// textured, blended quads that leave the depth buffer alone, and their
// textures are PSP-sized images stretched over large parts of the screen, so
// shading every pixel of them costs the GPU without showing more. Near the
// camera they cover the screen many times over: a Barioth's breath drew 12-16
// screens of them into each picture at 2560x1600. Blending, the depth test
// and coverage stay per pixel; only the colour is computed per block.
// Opaque and depth-writing geometry, the 2D interface, clears, untextured
// draws and low internal resolutions keep shading every pixel, and so do
// draws that sample a picture the renderer drew (a haze over the screen):
// that texture has the internal resolution, which blocks would halve.
inline bool coarse_effect_draw(bool through, bool clear, bool blend, bool depth_write, bool textured,
    bool rendered_texture, std::uint32_t target_height) {
    return !through && !clear && blend && !depth_write && textured && !rendered_texture &&
        target_height >= kCoarseMinTargetHeight;
}

// pipeline_keys.bin keeps the raw vertex shader in bit 0 of a key's last
// word and the coarse flag in bit 1. Version 1 files, written before coarse
// shading, hold 0 or 1 there and are still read; older builds leave version
// 2 files alone instead of reading a coarse key as a raw one.
inline constexpr std::uint32_t kPipelineKeysVersion = 2u;

inline bool readable_pipeline_keys(std::uint32_t version) {
    return version == 1u || version == kPipelineKeysVersion;
}

inline std::uint32_t shader_word(bool raw, bool coarse) {
    return (raw ? 1u : 0u) | (coarse ? 2u : 0u);
}

inline bool raw_shader(std::uint32_t word) {
    return (word & 1u) != 0u;
}

inline bool coarse_shader(std::uint32_t word) {
    return (word & 2u) != 0u;
}

// A device without coarse shading neither prewarms nor keeps coarse keys:
// their pipelines would be the per-pixel ones of the keys without the flag.
inline bool keeps_pipeline_key(bool coarse_key, bool coarse_effects) {
    return !coarse_key || coarse_effects;
}

} // namespace mhp3rd::gpu
