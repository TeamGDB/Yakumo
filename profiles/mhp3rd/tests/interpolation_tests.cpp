// Checks for frame interpolation's backend-independent part: matching draws
// between two frames, telling a cut from motion, and blending transforms.
// Needs no game data.
//
//   mhp3rd_interpolation_tests
#include "gpu/frame_interpolation.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace mhp3rd::gpu;
using namespace mhp3rd::gpu::interpolation;

namespace {

int failures = 0;

void check(bool condition, const char *what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++failures;
}

bool near(float a, float b, float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance; }

constexpr float kPi = 3.14159265358979f;
constexpr std::uint32_t kShown = 0x04000000u;

// A rotation about y by `degrees` followed by a translation.
Matrix transform(float degrees, float x, float y, float z) {
    const float c = std::cos(degrees * kPi / 180.0f);
    const float s = std::sin(degrees * kPi / 180.0f);
    Matrix m{};
    m[0] = c;
    m[2] = -s;
    m[5] = 1.0f;
    m[8] = s;
    m[10] = c;
    m[12] = x;
    m[13] = y;
    m[14] = z;
    m[15] = 1.0f;
    return m;
}

Matrix perspective() {
    Matrix m{};
    m[0] = 1.0f;
    m[5] = 1.8f;
    m[10] = -1.0f;
    m[11] = -1.0f;
    m[14] = -60.0f;
    return m;
}

// One 3D draw of mesh `mesh` placed at `x`, seen by a camera turned by
// `camera_degrees` and moved by `camera_x`. Like the game, the camera's
// rotation lives in the world matrix and the view only translates.
DrawSummary draw(std::uint32_t mesh, float x, float camera_degrees = 0.0f, float camera_x = 0.0f) {
    DrawSummary summary{};
    summary.vertex_address = 0x09000000u + mesh * 0x100u;
    summary.vertex_type = 0x11Cu;
    summary.texture_address = 0x09800000u + mesh * 0x40u;
    summary.count = 36u;
    summary.primitive = PrimitiveType::Triangles;
    summary.target = kShown;
    summary.perspective = true;
    summary.world = transform(camera_degrees, x, 0.0f, -500.0f);
    summary.view = transform(0.0f, -camera_x, 0.0f, 0.0f);
    summary.projection = perspective();
    return summary;
}

std::vector<DrawSummary> scene(std::uint32_t meshes, float camera_degrees = 0.0f, float camera_x = 0.0f) {
    std::vector<DrawSummary> frame;
    for (std::uint32_t i = 0; i < meshes; ++i) frame.push_back(draw(i, static_cast<float>(i) * 10.0f, camera_degrees, camera_x));
    mark_eligible(frame, kShown);
    return frame;
}

void blending() {
    const Matrix a = transform(10.0f, 100.0f, 20.0f, -300.0f);
    const Matrix b = transform(16.0f, 104.0f, 20.0f, -310.0f);
    const Matrix start = blend_affine(a, b, 0.0f);
    const Matrix end = blend_affine(a, b, 1.0f);
    bool same = true;
    for (std::size_t i = 0; i < 16u; ++i) same = same && near(start[i], a[i]) && near(end[i], b[i]);
    check(same, "blend_affine gives the ends at 0 and 1");
    const Matrix half = blend_affine(a, b, 0.5f);
    check(near(rotation_angle_degrees(a, half), 3.0f, 0.01f), "halfway turns half the angle");
    check(near(half[12], 102.0f) && near(half[14], -305.0f), "halfway moves half the distance");
    float length = 0.0f;
    for (std::size_t row = 0; row < 3u; ++row) length += half[row] * half[row];
    check(near(std::sqrt(length), 1.0f), "a blended rotation keeps its scale");
    check(near(rotation_angle_degrees(transform(10.0f, 0, 0, 0), transform(100.0f, 0, 0, 0)), 90.0f, 0.01f),
          "rotation_angle_degrees measures a quarter turn");
    Matrix ortho{};
    ortho[0] = ortho[5] = ortho[10] = ortho[15] = 1.0f;
    check(is_orthographic(ortho) && !is_orthographic(perspective()), "orthographic projections are told apart");
}

void matching() {
    const CutThresholds thresholds{};
    Matcher matcher;

    const std::vector<DrawSummary> still = scene(100u);
    const Matching &same = matcher.match(still, still, thresholds);
    check(same.matched == 100u && same.cut == nullptr, "a still scene matches every draw and is no cut");

    const Matching &walking = matcher.match(scene(100u, 0.0f, 0.0f), scene(100u, 1.5f, 8.0f), thresholds);
    check(walking.matched == 100u && walking.cut == nullptr, "a camera turning 1.5 degrees and moving 8 units blends");
    check(near(walking.camera_angle_degrees, 1.5f, 0.01f) && near(walking.camera_distance, 8.0f, 0.5f),
          "the camera's turn and move are measured in eye space");

    const Matching &turned = matcher.match(scene(100u), scene(100u, 40.0f), thresholds);
    check(turned.cut != nullptr && std::strcmp(turned.cut, "camera turned") == 0, "a 40 degree turn is a cut");

    const Matching &moved = matcher.match(scene(100u), scene(100u, 0.0f, 300.0f), thresholds);
    check(moved.cut != nullptr && std::strcmp(moved.cut, "camera moved") == 0, "a 300 unit jump is a cut");

    std::vector<DrawSummary> other = scene(100u);
    for (std::size_t i = 0; i < 60u; ++i) other[i].vertex_address += 0x00100000u;
    const Matching &replaced = matcher.match(scene(100u), other, thresholds);
    check(replaced.matched == 40u && replaced.cut != nullptr && std::strcmp(replaced.cut, "few draws match") == 0,
          "a frame with most draws new is a cut");

    // Instances of one mesh pair up in drawing order.
    std::vector<DrawSummary> older{draw(7u, 0.0f), draw(7u, 50.0f), draw(7u, 100.0f)};
    std::vector<DrawSummary> newer{draw(7u, 1.0f), draw(7u, 51.0f)};
    mark_eligible(older, kShown);
    mark_eligible(newer, kShown);
    const Matching &instances = matcher.match(older, newer, thresholds);
    check(instances.newer_of[0] == 0 && instances.newer_of[1] == 1 && instances.newer_of[2] == -1,
          "instances pair in order and an extra one stays unmatched");

    // Only perspective draws into the shown framebuffer take part.
    std::vector<DrawSummary> mixed{draw(1u, 0.0f), draw(2u, 0.0f), draw(3u, 0.0f)};
    mixed[1].target = 0x04100000u;  // render to texture
    mixed[2].perspective = false;   // 2D, orthographic or a clear
    mark_eligible(mixed, kShown);
    const Matching &eligible = matcher.match(mixed, mixed, thresholds);
    check(eligible.eligible_older == 1u && eligible.matched == 1u && eligible.newer_of[1] == -1 &&
              eligible.newer_of[2] == -1,
          "draws into other framebuffers and 2D draws are never matched");

    std::vector<DrawSummary> flat{draw(1u, 0.0f)};
    flat[0].perspective = false;
    mark_eligible(flat, kShown);
    const Matching &nothing = matcher.match(flat, flat, thresholds);
    check(nothing.cut != nullptr && std::strcmp(nothing.cut, "nothing to blend") == 0,
          "a frame without 3D draws, like a loading screen, is not blended");
}

} // namespace

int main() {
    blending();
    matching();
    std::printf("%s\n", failures == 0 ? "all passed" : "FAILED");
    return failures == 0 ? 0 : 1;
}
