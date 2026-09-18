#include "frame_interpolation.hpp"

#include <algorithm>
#include <cmath>

namespace mhp3rd::gpu::interpolation {
namespace {

constexpr float kPi = 3.14159265358979f;

float length3(const float *v) noexcept { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

} // namespace

DrawSummary summarize(const DrawCall &call) {
    DrawSummary draw{};
    draw.vertex_address = call.vertex_address;
    draw.index_address = call.index_address;
    draw.vertex_type = call.vertex_type;
    draw.texture_address = call.texture.enabled ? call.texture.address : 0u;
    draw.count = call.primitive_count;
    draw.primitive = call.primitive;
    draw.target = call.target.color_address;
    draw.perspective = !call.through && !call.clear_mode && !is_orthographic(call.projection);
    // Vertex type bits 9..10 give the weight format; ge_state.cpp's
    // decode_vertices skins exactly the transformed vertices that have one.
    draw.skinned = ((call.vertex_type >> 9u) & 3u) != 0u && !call.through;
    draw.world = call.world;
    draw.view = call.view;
    draw.projection = call.projection;
    return draw;
}

void mark_eligible(std::vector<DrawSummary> &draws, std::uint32_t displayed) noexcept {
    for (DrawSummary &draw : draws) draw.eligible = draw.perspective && draw.target == displayed;
}

bool is_orthographic(const Matrix &projection) noexcept {
    // A perspective projection copies -z into w: row 3 is (0, 0, -1, 0).
    return std::fabs(projection[11]) < 1e-6f;
}

std::size_t Matcher::KeyHash::operator()(const Key &key) const noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&](std::uint32_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    mix(key.vertex_address);
    mix(key.index_address);
    mix(key.vertex_type);
    mix(key.texture_address);
    mix(key.count);
    mix(key.primitive);
    return static_cast<std::size_t>(hash);
}

Matcher::Key Matcher::key_of(const DrawSummary &draw) noexcept {
    return Key{draw.vertex_address, draw.index_address, draw.vertex_type, draw.texture_address, draw.count,
               static_cast<std::uint8_t>(draw.primitive)};
}

const Matching &Matcher::match(const std::vector<DrawSummary> &older, const std::vector<DrawSummary> &newer,
                               const CutThresholds &thresholds) {
    Matching &out = result_;
    out = Matching{};
    out.newer_of.assign(older.size(), -1);

    for (auto &[key, slot] : slots_) {
        slot.newer.clear();
        slot.used = 0u;
    }
    for (std::size_t i = 0; i < newer.size(); ++i) {
        if (!newer[i].eligible) continue;
        ++out.eligible_newer;
        slots_[key_of(newer[i])].newer.push_back(static_cast<std::int32_t>(i));
    }
    for (std::size_t i = 0; i < older.size(); ++i) {
        if (!older[i].eligible) continue;
        ++out.eligible_older;
        const auto found = slots_.find(key_of(older[i]));
        if (found == slots_.end()) continue;
        Slot &slot = found->second;
        if (slot.used >= slot.newer.size()) {
            ++slot.used;
            continue;
        }
        out.newer_of[i] = slot.newer[slot.used++];
        ++out.matched;
    }
    // Keys neither frame used lately would pile up over a long session.
    if (slots_.size() > 8192u) slots_.clear();

    // The camera is the view matrix most of the newer frame's matched draws
    // use; a frame seldom has more than a few.
    struct Candidate {
        const Matrix *view;
        std::size_t older_index;
        std::uint32_t uses;
    };
    std::vector<Candidate> views;
    for (std::size_t i = 0; i < older.size(); ++i) {
        const std::int32_t partner = out.newer_of[i];
        if (partner < 0) continue;
        const Matrix &view = newer[static_cast<std::size_t>(partner)].view;
        auto found = std::find_if(views.begin(), views.end(), [&](const Candidate &c) { return *c.view == view; });
        if (found != views.end()) ++found->uses;
        else if (views.size() < 16u) views.push_back({&view, i, 1u});
    }
    if (!views.empty()) {
        const Candidate &camera = *std::max_element(
            views.begin(), views.end(), [](const Candidate &a, const Candidate &b) { return a.uses < b.uses; });
        const Matrix &before = older[camera.older_index].view;
        const std::array<float, 3> from = camera_position(before);
        const std::array<float, 3> to = camera_position(*camera.view);
        const float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
        out.camera_found = true;
        out.camera_distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        out.camera_angle_degrees = rotation_angle_degrees(before, *camera.view);
    }

    if (out.eligible_newer == 0u || out.matched == 0u) {
        out.cut = "nothing to blend";
    } else if (static_cast<float>(out.matched) <
               thresholds.min_matched_fraction * static_cast<float>(std::max(out.eligible_newer, out.eligible_older))) {
        out.cut = "few draws match";
    } else if (out.camera_found && out.camera_angle_degrees > thresholds.max_camera_angle_degrees) {
        out.cut = "camera turned";
    } else if (out.camera_found && out.camera_distance > thresholds.max_camera_distance) {
        out.cut = "camera moved";
    }
    return out;
}

Matrix blend_affine(const Matrix &a, const Matrix &b, float t) noexcept {
    Matrix out = blend_linear(a, b, t);
    for (std::uint32_t column = 0; column < 3u; ++column) {
        float *axis = out.data() + column * 4u;
        const float length = length3(axis);
        if (length < 1e-12f) continue;
        const float wanted = length3(a.data() + column * 4u) * (1.0f - t) + length3(b.data() + column * 4u) * t;
        const float scale = wanted / length;
        for (std::uint32_t row = 0; row < 3u; ++row) axis[row] *= scale;
    }
    return out;
}

Matrix blend_linear(const Matrix &a, const Matrix &b, float t) noexcept {
    Matrix out{};
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = a[i] + (b[i] - a[i]) * t;
    return out;
}

std::array<float, 3> camera_position(const Matrix &view) noexcept {
    // view = [R | t] maps world to eye space; the eye sits at -R^T t.
    std::array<float, 3> position{};
    for (std::uint32_t axis = 0; axis < 3u; ++axis) {
        float sum = 0.0f;
        for (std::uint32_t row = 0; row < 3u; ++row) sum += view[axis * 4u + row] * view[12u + row];
        position[axis] = -sum;
    }
    return position;
}

float rotation_angle_degrees(const Matrix &a, const Matrix &b) noexcept {
    // trace(Ra^T Rb) = 1 + 2 cos(angle) for two rotations; the columns are
    // normalised first so a uniform scale does not read as a turn.
    float trace = 0.0f;
    for (std::uint32_t column = 0; column < 3u; ++column) {
        const float *x = a.data() + column * 4u;
        const float *y = b.data() + column * 4u;
        const float lengths = length3(x) * length3(y);
        if (lengths < 1e-12f) return 180.0f;
        trace += (x[0] * y[0] + x[1] * y[1] + x[2] * y[2]) / lengths;
    }
    const float cosine = std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f);
    return std::acos(cosine) * 180.0f / kPi;
}

} // namespace mhp3rd::gpu::interpolation
