// Synthetic opacity proofs and blend/alpha-test equivalence; no renderer or
// game assets are needed for these contracts.
#include "gpu/opaque_alpha.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

using namespace mhp3rd::gpu;

int failures{};

void expect(bool condition, const char *what) {
    if (condition) return;
    ++failures;
    std::cout << "FAIL: " << what << '\n';
}

void test_pixels() {
    std::array<std::uint32_t, 5> pixels{0xFF000000u, 0xFFFFFFFFu, 0xFF123456u, 0xFFABCDEFu, 0xFF00FFFFu};
    expect(opaque_pixels(pixels.data(), pixels.size()), "all opaque texels, regardless of RGB");
    expect(opaque_pixels(pixels.data(), 1u), "single opaque texel");
    expect(!opaque_pixels(nullptr, pixels.size()), "missing pixels are unknown");
    expect(!opaque_pixels(nullptr, 0u), "missing empty pixels are unknown");
    expect(!opaque_pixels(pixels.data(), 0u), "empty image is unknown");
    for (std::size_t i = 0u; i < pixels.size(); ++i) {
        const std::uint32_t before = pixels[i];
        for (const std::uint32_t alpha : {0u, 1u, 128u, 254u}) {
            pixels[i] = (before & 0xFFFFFFu) | alpha << 24u;
            expect(!opaque_pixels(pixels.data(), pixels.size()), "any changed alpha invalidates image opacity");
        }
        pixels[i] = before;
        expect(opaque_pixels(pixels.data(), pixels.size()), "restored image can be proved opaque again");
    }
}

void test_vertex_alpha() {
    // Independently evaluate the vertex shader's selected alpha for two
    // vertices. An unknown vertex proof is represented by a nonopaque vertex.
    for (const bool lit : {false, true})
        for (const bool has_color : {false, true})
            for (const bool vertices_opaque : {false, true})
                for (std::uint32_t update = 0u; update < 8u; ++update)
                    for (const std::uint32_t ambient : {0u, 128u, 254u, 255u})
                        for (const std::uint32_t material : {0u, 254u, 255u}) {
                            const std::array<double, 2> vertices{1.0, vertices_opaque ? 1.0 : 254.0 / 255.0};
                            bool expected = true;
                            for (const double vertex : vertices) {
                                const double selected = has_color && update % 2u != 0u ? vertex : material / 255.0;
                                const double alpha = lit ? ambient / 255.0 * selected : vertex;
                                expected &= alpha == 1.0;
                            }
                            expect(opaque_vertex_alpha(lit, vertices_opaque, has_color, update, ambient,
                                       material << 24u | 0x123456u) == expected,
                                "lit/unlit alpha proof matches independent selected-material calculation");
                        }
    expect(!opaque_vertex_alpha(true, true, false, 0u, 256u, 0xFFFFFFFFu), "invalid ambient is unknown");
    expect(!opaque_vertex_alpha(true, true, false, 0u, std::numeric_limits<std::uint32_t>::max(), 0xFFFFFFFFu),
        "oversized ambient is unknown");
    expect(opaque_vertex_alpha(true, false, true, 2u, 255u, 0xFF000000u),
        "diffuse-only material update cannot change ambient alpha");
    expect(!opaque_vertex_alpha(true, false, true, 1u, 255u, 0xFFFFFFFFu),
        "ambient material update selects nonopaque vertices");
}

void test_fragment_alpha() {
    for (const bool textured : {false, true})
        for (const bool vertex_opaque : {false, true})
            for (const bool texture_opaque : {false, true})
                for (std::uint32_t function = 0u; function <= 4u; ++function) {
                    const double vertex = vertex_opaque ? 1.0 : 128.0 / 255.0;
                    const double texture = texture_opaque ? 1.0 : 254.0 / 255.0;
                    double alpha = vertex;
                    if (textured) {
                        if (function == 0u || function == 2u)
                            alpha = vertex * texture;
                        else if (function >= 3u)
                            alpha = texture;
                    }
                    expect(opaque_fragment_alpha(textured, function, vertex_opaque, texture_opaque) == (alpha == 1.0),
                        "recognized texture functions match independently evaluated fragment alpha");
                }
    for (const std::uint32_t function : {5u, 7u, 8u, std::numeric_limits<std::uint32_t>::max()}) {
        expect(!opaque_fragment_alpha(true, function, true, true), "unknown texture function stays unknown");
        expect(opaque_fragment_alpha(false, function, true, false), "disabled texture ignores its function");
        expect(!opaque_fragment_alpha(false, function, false, true), "disabled texture keeps nonopaque vertex alpha");
    }
}

// Independently evaluate the documented comparison using double arithmetic
// and an explicit alpha input. The helper must preserve the existing .002
// equality window, rather than treating EQUAL as an arbitrary integer test.
bool shader_predicate(std::uint32_t function, double alpha, std::uint32_t reference) {
    const double normalized = reference / 255.0;
    switch (function) {
    case 0u:
        return true;
    case 1u:
        return false;
    case 2u:
        return std::abs(alpha - normalized) < 0.002;
    case 3u:
        return std::abs(alpha - normalized) >= 0.002;
    case 4u:
        return alpha < normalized;
    case 5u:
        return alpha <= normalized;
    case 6u:
        return alpha > normalized;
    case 7u:
        return alpha >= normalized;
    default:
        return true;
    }
}

void test_alpha_predicates() {
    for (std::uint32_t function = 0u; function < 8u; ++function)
        for (std::uint32_t reference = 0u; reference < 256u; ++reference) {
            expect(
                opaque_alpha_test_passes(true, function, reference, 255u) == shader_predicate(function, 1.0, reference),
                "all eight alpha comparisons and byte references match alpha-one shader predicate");
            expect(opaque_alpha_test_passes(false, function, reference, 0u), "disabled alpha tests pass");
            for (const std::uint32_t mask : {0u, 127u, 254u, 256u})
                expect(!opaque_alpha_test_passes(true, function, reference, mask), "partial/invalid masks retained");
        }
    for (const std::uint32_t reference : {256u, std::numeric_limits<std::uint32_t>::max()})
        for (std::uint32_t function = 0u; function < 8u; ++function)
            expect(!opaque_alpha_test_passes(true, function, reference, 255u), "invalid references retained");
    for (const std::uint32_t function : {8u, std::numeric_limits<std::uint32_t>::max()}) {
        expect(!opaque_alpha_test_passes(true, function, 255u, 255u), "unknown alpha comparisons retained");
        expect(opaque_alpha_test_passes(false, function, std::numeric_limits<std::uint32_t>::max(), 0u),
            "disabled alpha test ignores invalid state");
    }
    expect(opaque_alpha_test_passes(true, 2u, 255u, 255u), "EQUAL passes at the opaque endpoint");
    expect(!opaque_alpha_test_passes(true, 2u, 254u, 255u), "adjacent byte lies outside EQUAL tolerance");
    expect(!opaque_alpha_test_passes(true, 3u, 255u, 255u), "NOT EQUAL rejects the opaque endpoint");
    expect(opaque_alpha_test_passes(true, 3u, 254u, 255u), "NOT EQUAL passes at adjacent byte reference");
    expect(!opaque_alpha_test_passes(true, 7u, 256u, 255u), "GEQUAL out-of-byte reference is not assumed to pass");
    expect(!opaque_alpha_test_passes(true, 6u, 256u, 255u), "GREATER out-of-byte reference is not assumed to pass");
}

void test_interpolated_alpha() {
    const float rounded = std::nextafter(1.0f, 0.0f);
    expect(!shader_predicate(7u, rounded, 255u), "a rounded opaque varying can fail the strict upper endpoint");
    expect(!opaque_alpha_is_exact(false, false, 0u), "untextured perspective alpha retains its original path");
    for (std::uint32_t function : {0u, 1u, 2u}) {
        expect(!opaque_alpha_is_exact(false, true, function),
            "modulate, decal and blend depend on the perspective-interpolated vertex alpha");
        expect(opaque_alpha_is_exact(true, true, function), "through alpha does not use a perspective division");
    }
    for (std::uint32_t function : {3u, 4u}) {
        expect(opaque_alpha_is_exact(false, true, function), "replace and add take alpha from the opaque sample");
        expect(!opaque_alpha_is_exact(false, false, function), "disabled texturing still uses the varying alpha");
    }
    for (std::uint32_t function : {5u, 8u, std::numeric_limits<std::uint32_t>::max()})
        expect(!opaque_alpha_is_exact(false, true, function), "unknown perspective texture functions remain guarded");
    expect(opaque_alpha_is_exact(true, false, 0u), "untextured through alpha can be exact");
}

std::array<double, 4> alpha_blend(const std::array<double, 4> &source, const std::array<double, 4> &destination) {
    // Vulkan uses SRC_ALPHA / ONE_MINUS_SRC_ALPHA for RGB, and ONE / ZERO
    // for alpha. Compare all four channels, including arbitrary destination alpha.
    std::array<double, 4> out{};
    for (std::size_t i = 0u; i < 3u; ++i) out[i] = source[i] * source[3] + destination[i] * (1.0 - source[3]);
    out[3] = source[3];
    return out;
}

void test_blend_equivalence() {
    for (const double red : {0.0, 1.0 / 255.0, 128.0 / 255.0, 1.0})
        for (const double green : {0.0, 254.0 / 255.0, 1.0})
            for (const double destination_alpha : {0.0, 128.0 / 255.0, 1.0}) {
                const std::array<double, 4> source{red, green, 1.0 - red, 1.0};
                const std::array<double, 4> destination{1.0 - red, 1.0 - green, red, destination_alpha};
                expect(alpha_blend(source, destination) == source, "opaque alpha blend matches unblended RGBA");
            }
    expect(redundant_opaque_blend(true, 2u, 3u, 0u), "enabled SRC_ALPHA inverse ADD is redundant when opaque");
    expect(!redundant_opaque_blend(false, 2u, 3u, 0u), "disabled blend needs no optimization");
    for (std::uint32_t source = 0u; source < 20u; ++source)
        for (std::uint32_t destination = 0u; destination < 20u; ++destination)
            for (std::uint32_t equation = 0u; equation < 8u; ++equation)
                if (source != 2u || destination != 3u || equation != 0u)
                    expect(!redundant_opaque_blend(true, source, destination, equation), "other blend states retained");
    const std::uint32_t invalid = std::numeric_limits<std::uint32_t>::max();
    expect(!redundant_opaque_blend(true, invalid, 3u, 0u), "invalid source factor retained");
    expect(!redundant_opaque_blend(true, 2u, invalid, 0u), "invalid destination factor retained");
    expect(!redundant_opaque_blend(true, 2u, 3u, invalid), "invalid equation retained");
    for (const double alpha : {0.0, 128.0 / 255.0, 254.0 / 255.0, 1.0 - 1e-6}) {
        const std::array<double, 4> source{1.0, 0.0, 0.0, alpha};
        const std::array<double, 4> destination{0.0, 1.0, 1.0, 0.5};
        expect(alpha_blend(source, destination) != source, "nonopaque alpha blend cannot generally be disabled");
    }
}

} // namespace

int main() {
    test_pixels();
    test_vertex_alpha();
    test_fragment_alpha();
    test_alpha_predicates();
    test_interpolated_alpha();
    test_blend_equivalence();
    if (failures != 0) return 1;
    std::cout << "opaque alpha tests passed\n";
    return 0;
}
