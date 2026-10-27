// Which draws the renderer shades in 2x2 blocks; no GPU is needed.
#include "gpu/coarse_shading.hpp"

#include <iostream>

namespace {

using namespace mhp3rd::gpu;

int failures{};

void expect(bool condition, const char *what) {
    if (condition) return;
    ++failures;
    std::cout << "FAIL: " << what << '\n';
}

} // namespace

int main() {
    constexpr std::uint32_t tall = 1600u;
    expect(
        coarse_effect_draw(false, false, true, false, true, false, tall), "a translucent textured 3D effect is coarse");
    expect(coarse_effect_draw(false, false, true, false, true, false, kCoarseMinTargetHeight), "from x4 on");
    expect(!coarse_effect_draw(false, false, true, false, true, false, kCoarseMinTargetHeight - 1u), "not below x4");
    expect(!coarse_effect_draw(true, false, true, false, true, false, tall), "never the 2D interface");
    expect(!coarse_effect_draw(false, true, true, false, true, false, tall), "never a clear");
    expect(!coarse_effect_draw(false, false, false, false, true, false, tall), "never an opaque draw");
    expect(!coarse_effect_draw(false, false, true, true, true, false, tall), "never geometry that writes depth");
    expect(!coarse_effect_draw(false, false, true, false, false, false, tall), "never an untextured draw");
    expect(!coarse_effect_draw(false, false, true, false, true, true, tall),
        "never a draw that samples a rendered picture");

    // The key file: the raw shader and the coarse flag share a word.
    for (const bool raw : {false, true})
        for (const bool coarse : {false, true}) {
            const std::uint32_t word = shader_word(raw, coarse);
            expect(raw_shader(word) == raw && coarse_shader(word) == coarse, "the shader word reads back as written");
        }
    expect(
        shader_word(true, false) == 1u && shader_word(false, false) == 0u, "a key without coarse is written as before");
    expect(raw_shader(1u) && !coarse_shader(1u) && !raw_shader(0u) && !coarse_shader(0u),
        "a version 1 word holds no coarse flag");
    expect(readable_pipeline_keys(1u) && readable_pipeline_keys(kPipelineKeysVersion), "versions 1 and 2 are read");
    expect(kPipelineKeysVersion == 2u, "files with coarse keys are version 2, which older builds leave alone");
    expect(!readable_pipeline_keys(0u) && !readable_pipeline_keys(3u), "other versions are not read");
    expect(keeps_pipeline_key(false, false) && keeps_pipeline_key(false, true) && keeps_pipeline_key(true, true),
        "keys are kept where they can be made as recorded");
    expect(!keeps_pipeline_key(true, false), "a device without coarse shading drops coarse keys");
    std::cout << (failures == 0 ? "all passed" : "FAILED") << '\n';
    return failures == 0 ? 0 : 1;
}
