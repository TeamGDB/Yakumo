// Renderer helpers that need no GPU: alpha-test elision must preserve every
// fragment, and index lists must name, position by position, the vertex the
// triangle expansion would have written there.

#include "gpu/triangle_indices.hpp"
#include "gpu/alpha_test.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

namespace {

using mhp3rd::gpu::PrimitiveType;

int failures{};

void expect(bool condition, const char *what) {
    if (condition) return;
    ++failures;
    std::cout << "FAIL: " << what << "\n";
}

// The renderer's expansion as it was written before the index lists, kept
// here as the reference: which decoded vertex lands at each output position.
std::vector<std::uint16_t> reference(
    PrimitiveType primitive, std::size_t count, const std::vector<std::uint16_t> &indices, std::size_t vertex_count) {
    std::vector<std::uint16_t> out;
    const auto vertex_at = [&](std::size_t index) -> std::uint16_t {
        if (!indices.empty()) {
            const std::size_t mapped = indices[index];
            return static_cast<std::uint16_t>(std::min(mapped, vertex_count - 1u));
        }
        return static_cast<std::uint16_t>(std::min(index, vertex_count - 1u));
    };
    switch (primitive) {
    case PrimitiveType::Triangles:
        for (std::size_t i = 0; i + 2u < count; i += 3u) {
            out.push_back(vertex_at(i));
            out.push_back(vertex_at(i + 1u));
            out.push_back(vertex_at(i + 2u));
        }
        break;
    case PrimitiveType::TriangleStrip:
        for (std::size_t i = 0; i + 2u < count; ++i) {
            const bool odd = (i & 1u) != 0u;
            out.push_back(vertex_at(i));
            out.push_back(vertex_at(odd ? i + 2u : i + 1u));
            out.push_back(vertex_at(odd ? i + 1u : i + 2u));
        }
        break;
    case PrimitiveType::TriangleFan:
        for (std::size_t i = 1u; i + 1u < count; ++i) {
            out.push_back(vertex_at(0));
            out.push_back(vertex_at(i));
            out.push_back(vertex_at(i + 1u));
        }
        break;
    default:
        break;
    }
    return out;
}

void test_matches_expansion() {
    std::mt19937 random(92u);
    std::vector<std::uint16_t> out;
    for (const PrimitiveType primitive :
        {PrimitiveType::Triangles, PrimitiveType::TriangleStrip, PrimitiveType::TriangleFan}) {
        for (std::size_t count = 0; count < 40u; ++count) {
            for (int indexed = 0; indexed < 2; ++indexed) {
                for (int trial = 0; trial < 8; ++trial) {
                    std::size_t vertex_count = count;
                    std::vector<std::uint16_t> indices;
                    if (indexed != 0) {
                        // Indices past the decoded vertices are clamped, as
                        // the expansion clamps them.
                        vertex_count = 1u + random() % 24u;
                        for (std::size_t i = 0; i < count; ++i)
                            indices.push_back(static_cast<std::uint16_t>(random() % (vertex_count + 4u)));
                    }
                    if (vertex_count == 0u) continue;
                    expect(mhp3rd::gpu::triangle_indices(primitive, count, indices, vertex_count, out),
                        "triangle primitives are supported");
                    expect(
                        out == reference(primitive, count, indices, vertex_count), "index list matches the expansion");
                    expect(out.size() % 3u == 0u, "whole triangles");
                }
            }
        }
    }
}

void test_large_mesh() {
    // A strip over the whole 16-bit index range.
    const std::size_t count = 65536u;
    std::vector<std::uint16_t> out;
    expect(mhp3rd::gpu::triangle_indices(PrimitiveType::TriangleStrip, count, {}, count, out), "strip supported");
    expect(out == reference(PrimitiveType::TriangleStrip, count, {}, count), "large strip matches the expansion");
    expect(out.size() == (count - 2u) * 3u, "large strip triangle count");
}

void test_other_primitives() {
    std::vector<std::uint16_t> out{1u, 2u, 3u};
    for (const PrimitiveType primitive :
        {PrimitiveType::Points, PrimitiveType::Lines, PrimitiveType::LineStrip, PrimitiveType::Sprites}) {
        expect(!mhp3rd::gpu::triangle_indices(primitive, 4u, {}, 4u, out), "other primitives are refused");
        expect(out.empty(), "refused primitives leave no indices");
    }
}

// An independent scalar evaluation of the fragment shader's comparison,
// including its equality tolerance. Fog and specular never change alpha.
bool shader_accepts_alpha(float alpha, std::uint32_t function, std::uint32_t reference) {
    const float threshold = static_cast<float>(reference) / 255.0f;
    switch (function) {
    case 0u:
        return true;
    case 1u:
        return false;
    case 2u:
        return std::abs(alpha - threshold) < 0.002f;
    case 3u:
        return std::abs(alpha - threshold) >= 0.002f;
    case 4u:
        return alpha < threshold;
    case 5u:
        return alpha <= threshold;
    case 6u:
        return alpha > threshold;
    case 7u:
        return alpha >= threshold;
    default:
        return true;
    }
}

void test_alpha_test_boundaries() {
    using mhp3rd::gpu::requires_alpha_test;
    // Endpoint comparisons alone can pass for every normalized alpha.
    const std::array<std::uint32_t, 6> references{0u, 1u, 128u, 254u, 255u, 256u};
    const std::array<std::array<bool, 6>, 8> required{{
        {false, false, false, false, false, false},
        {true, true, true, true, true, true},
        {true, true, true, true, true, true},
        {true, true, true, true, true, true},
        {true, true, true, true, true, true},
        {true, true, true, true, false, true},
        {true, true, true, true, true, true},
        {false, true, true, true, true, true},
    }};
    for (std::uint32_t function = 0u; function < required.size(); ++function) {
        for (std::size_t index = 0; index < references.size(); ++index) {
            const std::uint32_t reference = references[index];
            expect(requires_alpha_test(true, function, reference, 0xFFu) == required[function][index],
                "only always-passing full-mask alpha comparisons omit the test");
            for (const std::uint32_t mask : {0u, 0x7Fu, 0xFFu}) {
                expect(!requires_alpha_test(false, function, reference, mask),
                    "disabled alpha testing never needs a discard-capable shader");
                if (mask != 0xFFu)
                    expect(requires_alpha_test(true, function, reference, mask) == (function != 0u),
                        "masked comparisons retain the existing shader path");
            }
        }
    }
    for (const std::uint32_t function : {8u, std::numeric_limits<std::uint32_t>::max()}) {
        for (const std::uint32_t reference : references) {
            for (const std::uint32_t mask : {0u, 0x7Fu, 0xFFu}) {
                expect(requires_alpha_test(true, function, reference, mask),
                    "unknown enabled comparisons remain conservative");
                expect(!requires_alpha_test(false, function, reference, mask),
                    "disabled testing also ignores unknown comparison values");
            }
        }
    }
    expect(requires_alpha_test(true, 5u, 255u, 256u) && requires_alpha_test(true, 7u, 0u, 256u),
        "out-of-range masks cannot enable alpha-test elision");
}

void test_alpha_test_elision_preserves_fragments() {
    // Exercise every byte reference and alpha, then values between byte steps
    // and immediately inside the normalized endpoints used by the proof.
    const std::array<float, 5> continuous{0.0f, std::nextafter(0.0f, 1.0f), 0.5f, std::nextafter(1.0f, 0.0f), 1.0f};
    unsigned bypassed = 0u;
    for (std::uint32_t function = 0u; function < 8u; ++function) {
        for (std::uint32_t reference = 0u; reference <= 255u; ++reference) {
            if (mhp3rd::gpu::requires_alpha_test(true, function, reference, 0xFFu)) continue;
            ++bypassed;
            for (std::uint32_t alpha = 0u; alpha <= 255u; ++alpha)
                expect(shader_accepts_alpha(static_cast<float>(alpha) / 255.0f, function, reference),
                    "omitting the alpha test preserves every normalized byte alpha");
            for (const float alpha : continuous)
                expect(shader_accepts_alpha(alpha, function, reference),
                    "omitting the alpha test preserves interpolated and endpoint alpha");
        }
    }
    expect(bypassed == 258u, "elision exercises all always comparisons and both safe endpoints");
}

} // namespace

int main() {
    test_matches_expansion();
    test_large_mesh();
    test_other_primitives();
    test_alpha_test_boundaries();
    test_alpha_test_elision_preserves_fragments();
    std::cout << (failures ? "FAIL" : "PASS") << ": render helpers (" << failures << " failures)\n";
    return failures ? 1 : 0;
}
