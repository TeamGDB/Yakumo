// GE contracts against bounded synthetic display lists and vertex data.
#include "gpu/ge_state.hpp"
#include "perf/frame_stats.hpp"
#include "psprecomp/common.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <bit>
#include <cmath>
#include <iostream>
#include <vector>

// Timing instrumentation is outside the GE contract under test.
namespace mhp3rd::perf {
bool alternate_off(NewPath) {
    return false;
}
bool split_enabled() noexcept {
    return false;
}
std::uint64_t split_ticks() noexcept {
    return 0;
}
void add_split(Split, std::uint64_t) noexcept {}
}
namespace {
using namespace mhp3rd::gpu;
int failures{};
constexpr std::uint32_t list = 0x08001000u, vertices = 0x08008000u, indices = 0x08009000u;
void expect(bool ok, const char *message) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
bool near(float a, float b) {
    return std::fabs(a - b) < 0.00001f;
}
std::uint32_t command(std::uint32_t op, std::uint32_t data = 0) {
    return (op << 24) | (data & 0xffffffu);
}
std::uint32_t f24(float value) {
    return std::bit_cast<std::uint32_t>(value) >> 8;
}
void store(psprecomp::GuestMemory &m, std::uint32_t address, const std::vector<std::uint32_t> &words) {
    for (std::size_t i = 0; i < words.size(); ++i) m.store32(address + static_cast<std::uint32_t>(i) * 4, words[i]);
}
void floats(psprecomp::GuestMemory &m, std::uint32_t address, float x, float y, float z) {
    m.store32(address, std::bit_cast<std::uint32_t>(x));
    m.store32(address + 4, std::bit_cast<std::uint32_t>(y));
    m.store32(address + 8, std::bit_cast<std::uint32_t>(z));
}
void execute(GeState &ge, psprecomp::GuestMemory &m, std::vector<std::uint32_t> words) {
    // A bare END is a marker inside a list: only FINISH or SIGNAL followed by END ends it.
    words.push_back(command(0x0f));
    words.push_back(command(0x0c));
    store(m, list, words);
    bool finished = false;
    expect(ge.execute(m, list, 0, finished) == list + words.size() * 4, "END returns next command address");
    expect(finished, "END finishes list");
}
std::vector<std::uint32_t> setup(std::uint32_t type = 3u << 7) {
    return {command(0x10, 0x080000), command(0x12, type), command(1, vertices & 0xffffff)};
}
void mirrored_vram_vertices_and_indices() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t boundary_vertex = 0x041ffff8u;
    floats(memory, boundary_vertex, 1.25f, -2.5f, 3.75f);
    std::vector<Vertex> decoded;
    expect(decode_vertices(memory, boundary_vertex, 3u << 7, 1, decoded) == 12 && decoded.size() == 1 &&
            decoded[0].position == std::array<float, 4>{1.25f, -2.5f, 3.75f, 1},
        "noncontiguous EDRAM vertex preserves float components across its physical wrap");
    floats(memory, vertices, 1, 2, 3);
    floats(memory, vertices + 12, 4, 5, 6);
    for (std::uint32_t format = 1; format <= 3; ++format) {
        constexpr std::uint32_t index_at = 0x041fffffu;
        const std::uint32_t count = format == 1 ? 2 : 1;
        if (format == 1) {
            memory.store8(index_at, 0);
            memory.store8(index_at + 1, 1);
        } else if (format == 2)
            memory.store16(index_at, 1);
        else
            memory.store32(index_at, 1);
        GeState state;
        DrawCall result;
        state.set_draw_sink([&](const DrawCall &draw) { result = draw; });
        auto words = setup((3u << 7) | (format << 11));
        words.push_back(command(0x10, 0x040000));
        words.push_back(command(2, index_at & 0xffffff));
        words.push_back(command(4, count));
        execute(state, memory, words);
        expect(result.vertices.size() == count && result.indices.size() == count && !result.vertices.empty() &&
                result.vertices.back().position == std::array<float, 4>{4, 5, 6, 1},
            "8/16/32-bit GE indices wrapping EDRAM resolve the real RAM vertices");
        if (count == 2)
            expect(result.indices == std::vector<std::uint16_t>{0, 1},
                "wrapped byte indices retain their actual draw order");
        else
            expect(result.indices == std::vector<std::uint16_t>{0},
                "wrapped wide index is rebased to its one decoded vertex");
    }
}
void control_flow() {
    psprecomp::GuestMemory m;
    GeState ge;
    std::vector<std::uint32_t> events;
    ge.set_signal_sink([&](std::uint32_t signal, std::uint32_t pc) {
        events.push_back(signal);
        events.push_back(pc);
    });
    store(m, list, {command(0x10, 0x080000), command(0x0a, 0x1020), command(0x0f, 0x12345), command(0x0c)});
    store(m, list + 32, {command(0x0e, 0x23456), command(0x0b)});
    bool done = true;
    expect(ge.execute(m, list, list + 4, done) == list + 4 && !done, "stall stops before command");
    expect(ge.execute(m, list + 4, 0, done) == list + 16 && done, "CALL returns and END finishes");
    expect(events == std::vector<std::uint32_t>({0x3456, list + 36, 0x12345, list + 12}),
        "SIGNAL and FINISH mask values and use next PC");
    execute(ge, m, {command(0x0b), command(0x09, 0xffffff), command(0x05), command(0x06), command(0xff)});
    expect(
        ge.unhandled_command_count() == 3, "unsupported curves/commands counted; empty RET and conditional jump safe");
    try {
        (void)ge.execute(m, 0, 0, done);
        expect(false, "invalid command address fails loudly");
    } catch (const psprecomp::Error &) {
        expect(!done, "invalid command address does not finish");
    }
    execute(ge, m, {command(0x10, 0), command(0x14), command(0x08, 12), command(0xff)});
    expect(ge.unhandled_command_count() == 3, "ORIGIN-relative jump skips intervening command");
}
void bounded_list_and_offset_contracts() {
    psprecomp::GuestMemory memory;
    GeState state;
    bool finished = true;
    store(memory, list, {command(0x10, 0x080000), command(0x08, 0x001004)});
    try {
        (void)state.execute(memory, list, 0, finished);
        expect(false, "self-jumping display list fails at its instruction budget");
    } catch (const psprecomp::Error &) {
        expect(!finished, "self-jumping display list exhausts its instruction budget without hanging or claiming END");
    }
    floats(memory, vertices, 10, 20, 30);
    DrawCall result;
    state.set_draw_sink([&](const DrawCall &draw) { result = draw; });
    execute(state, memory,
        {command(0x10, 0x080000), command(0x13, 0x40), command(0x00), command(0xcb), command(0x12, 3u << 7),
            command(1, (vertices - 0x4000) & 0xffffff), command(4, 1)});
    expect(result.vertices.size() == 1 && result.vertices[0].position == std::array<float, 4>{10, 20, 30, 1},
        "OFFSET_ADDR adds to BASE when resolving actual vertex memory");
}
void vertex_formats() {
    psprecomp::GuestMemory m;
    std::vector<Vertex> out;
    expect(vertex_format(0).stride == 0, "empty vertex type has no storage");
    expect(decode_vertices(m, vertices, 0, 1, out) == 0, "empty format cannot decode");
    for (std::uint32_t component = 1; component <= 3; ++component)
        for (bool through : {false, true}) {
            const auto type = component | (component << 5) | (component << 7) | (through ? 1u << 23 : 0);
            const auto layout = vertex_format(type);
            const auto size = component == 1 ? 1u : component == 2 ? 2u : 4u;
            auto put = [&](std::uint32_t at, float value) {
                if (component == 1)
                    m.store8(at, static_cast<std::uint8_t>(static_cast<int>(value)));
                else if (component == 2)
                    m.store16(at, static_cast<std::uint16_t>(static_cast<int>(value)));
                else
                    m.store32(at, std::bit_cast<std::uint32_t>(value));
            };
            put(vertices + layout.texcoord_offset, component == 1 ? 192 : component == 2 ? 49152 : 1.5f);
            put(vertices + layout.texcoord_offset + size, component == 1 ? 64 : component == 2 ? 16384 : 0.5f);
            for (std::uint32_t axis = 0; axis < 3; ++axis) {
                put(vertices + layout.normal_offset + axis * size, axis == 0 ? -64 : 0);
                put(vertices + layout.position_offset + axis * size, axis == 0 ? -64 : axis == 2 ? -1 : 32);
            }
            expect(decode_vertices(m, vertices, type, 1, out) == layout.stride,
                "public vertex layout matches consumed stride");
            expect(out.size() == 1, "one decoded vertex");
            if (out.empty()) continue;
            const float scale = component == 1 ? 128.f : component == 2 ? 32768.f : 1.f;
            expect(near(out[0].normal[0], -64.f / scale), "signed normal normalized by sign-bit magnitude");
            expect(near(out[0].position[0], through ? -64.f : -64.f / scale), "transformed/through position units");
            expect(near(out[0].position[2],
                       through && component == 2 ? 65535.f
                           : through             ? -1.f
                                                 : -1.f / scale),
                "through16 Z is unsigned");
            expect(
                near(out[0].texcoord[0], component == 2 && through ? 49152.f : 1.5f), "UV upper half remains unsigned");
        }
    for (std::uint32_t color = 4; color <= 7; ++color) {
        const auto type = (color << 2) | (3u << 7);
        auto layout = vertex_format(type);
        m.store32(vertices + layout.color_offset, 0xffffffff);
        floats(m, vertices + layout.position_offset, 1, 2, 3);
        decode_vertices(m, vertices, type, 1, out);
        expect(out[0].color == 0xffffffff, "all color layouts expand white");
    }
    // Two bones with unequal translation and weights; normals must not translate.
    std::array<float, 96> bones{};
    for (int bone = 0; bone < 8; ++bone) {
        bones[bone * 12] = bones[bone * 12 + 4] = bones[bone * 12 + 8] = 1;
    }
    bones[9] = 10;
    bones[21] = 20;
    for (std::uint32_t weight = 1; weight <= 3; ++weight) {
        const auto type = (weight << 9) | (1u << 14) | (3u << 5) | (3u << 7);
        auto layout = vertex_format(type);
        if (weight == 1) {
            m.store8(vertices, 32);
            m.store8(vertices + 1, 96);
        } else if (weight == 2) {
            m.store16(vertices, 8192);
            m.store16(vertices + 2, 24576);
        } else {
            m.store32(vertices, std::bit_cast<std::uint32_t>(0.25f));
            m.store32(vertices + 4, std::bit_cast<std::uint32_t>(0.75f));
        }
        floats(m, vertices + layout.normal_offset, 1, 0, 0);
        floats(m, vertices + layout.position_offset, 2, 3, 4);
        decode_vertices(m, vertices, type, 1, out, bones.data());
        expect(near(out[0].position[0], 19.5f) && near(out[0].normal[0], 1),
            "bone blend weights positions and leaves normals untranslated");
    }
    const auto morph = (3u << 7) | (1u << 18);
    floats(m, vertices, 1, 2, 3);
    floats(m, vertices + 12, 9, 9, 9);
    floats(m, vertices + 24, 4, 5, 6);
    expect(decode_vertices(m, vertices, morph, 2, out) == 24, "morph target count multiplies vertex stride");
    expect(out.size() == 2 && near(out[1].position[0], 4), "morph stride reaches next vertex");
}
void draws() {
    psprecomp::GuestMemory m;
    for (std::uint32_t i = 0; i < 8; ++i) floats(m, vertices + i * 12, static_cast<float>(i), 2, 3);
    for (std::uint32_t index_type = 0; index_type < 4; ++index_type) {
        GeState ge;
        std::vector<DrawCall> calls;
        ge.set_draw_sink([&](const DrawCall &call) { calls.push_back(call); });
        auto words = setup((3u << 7) | (index_type << 11));
        if (index_type) {
            words.push_back(command(2, indices & 0xffffff));
            for (std::uint32_t i = 0; i < 6; ++i) {
                auto value = std::array<std::uint32_t, 6>{4, 2, 3, 5, 3, 4}[i];
                if (index_type == 1)
                    m.store8(indices + i, static_cast<std::uint8_t>(value));
                else if (index_type == 2)
                    m.store16(indices + i * 2, static_cast<std::uint16_t>(value));
                else
                    m.store32(indices + i * 4, value);
            }
        }
        words.push_back(command(4, (3u << 16) | 3));
        words.push_back(command(4, (3u << 16) | 3));
        execute(ge, m, words);
        expect(calls.size() == 2, "successive PRIM commands emit draws");
        if (calls.size() != 2) continue;
        expect(calls[0].world[0] == 1 && calls[0].projection[15] == 1, "default matrices identity");
        if (index_type) {
            expect(calls[0].indices == std::vector<std::uint16_t>({2, 0, 1}),
                "indices rebased to lowest referenced vertex");
            expect(calls[1].index_address ==
                    indices +
                        3 *
                            (index_type == 1          ? 1
                                    : index_type == 2 ? 2
                                                      : 4),
                "indexed prim advances index pointer");
            expect(calls[1].vertex_address == vertices && calls[1].vertices[0].position[0] == 3,
                "indexed prim retains shared vertex base");
        } else
            expect(calls[1].vertex_address == vertices + 36 && calls[1].vertices[0].position[0] == 3,
                "nonindexed prim consumes vertices");
        expect(ge.draw_count() == 2 && ge.vertex_count() == 6, "draw and referenced-vertex counters");
    }
    GeState ge;
    int calls = 0, hooks = 0;
    ge.set_draw_sink([&](const DrawCall &d) {
        ++calls;
        expect(d.raw_count == 3 && d.raw_stride == 12 && d.raw_vertices != nullptr,
            "raw draw exposes contiguous original bytes");
        expect(d.vertices.empty(), "raw-only draw avoids CPU decode");
    });
    ge.set_raw_vertices(true);
    ge.set_view_hook([&](DrawCall &) { ++hooks; });
    auto words = setup();
    words.push_back(command(4, (3u << 16) | 3));
    execute(ge, m, words);
    expect(calls == 1 && hooks == 1, "transformed raw draw invokes view hook");
    ge.set_draw_sink([&](const DrawCall &d) {
        ++calls;
        expect(d.vertices.size() == 3, "raw comparison decodes same vertices");
    });
    ge.set_raw_vertices(true, true);
    execute(ge, m, words);
    ge.set_draw_sink([&](const DrawCall &) { ++calls; });
    auto bad = setup();
    bad.push_back(command(4, 0));
    bad.push_back(command(0x10, 0));
    bad.push_back(command(1, 0xffffff));
    bad.push_back(command(4, 3));
    execute(ge, m, bad);
    expect(calls == 2, "empty/out-of-range draws are skipped");
}
void registers() {
    psprecomp::GuestMemory m;
    floats(m, vertices, 1, 2, 3);
    GeState ge;
    DrawCall draw;
    ge.set_draw_sink([&](const DrawCall &d) { draw = d; });
    auto words = setup();
    for (auto op : {0x17u, 0x18u, 0x19u, 0x1au, 0x1bu, 0x1du, 0x1eu, 0x1fu, 0x21u, 0x22u, 0x23u, 0x51u, 0x9bu, 0xe7u})
        words.push_back(command(op, 1));
    for (auto op : {0x48u, 0x49u, 0x4au, 0x4bu, 0x42u, 0x43u, 0x44u, 0x45u, 0x46u, 0x47u, 0x5bu, 0xcdu, 0xceu})
        words.push_back(command(op, f24(2.5f)));
    for (auto op : {0x54u, 0x55u, 0x56u, 0x57u, 0x5cu, 0xcfu, 0xe0u, 0xe1u}) words.push_back(command(op, 0x123456));
    words.insert(words.end(),
        {command(0x53, 7), command(0x58, 0x88), command(0x5d, 0x77), command(0x5e, 1), command(0x9c, 0x2000),
            command(0x9d, 0x080100), command(0x9e, 0x3000), command(0x9f, 0x040200), command(0xd2, 3),
            command(0xa0, 0x4000), command(0xa8, 0x080040), command(0xb8, 0x0304), command(0xc2, 1), command(0xc3, 5),
            command(0xb0, 0x5000), command(0xb1, 0x080000), command(0xc5, 0x027f0b), command(0xc4, 0x40),
            command(0xc4, 1), command(0xc4, 0), command(0xc6, 0x0103), command(0xc7, 0x0101), command(0xc9, 0x0102),
            command(0xd3, 0x701), command(0xd4, 2 | (3 << 10)), command(0xd5, 470 | (260 << 10)),
            command(0x4c, 0xff0030), command(0x4d, 0xff0040), command(0xd6, 10), command(0xd7, 500),
            command(0xdb, 0xabcd06), command(0xde, 5), command(0xdf, 0x321)});
    for (std::uint32_t light = 0; light < 4; ++light) {
        words.push_back(command(0x5f + light, 0x0201));
        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            for (auto base : {0x63u, 0x6fu, 0x7bu})
                words.push_back(command(base + light * 3 + axis, f24(static_cast<float>(axis + 1))));
            words.push_back(command(0x8f + light * 3 + axis, 0x101010u * (axis + 1)));
        }
        words.push_back(command(0x87 + light, f24(4)));
        words.push_back(command(0x8b + light, f24(0.5f)));
    }
    words.push_back(command(4, 1));
    execute(ge, m, words);
    expect(draw.texture.enabled && draw.texture.swizzled && draw.texture.width == 16 && draw.texture.height == 8,
        "texture size/mode enables");
    expect(draw.texture.address == 0x08004000 && draw.texture.buffer_width == 64 &&
            draw.texture.format == TextureFormat::Clut8,
        "texture address high bits and format");
    expect(draw.texture.clut_address == 0x08005000 && draw.texture.clut_format == 3 && draw.texture.clut_shift == 2 &&
            draw.texture.clut_mask == 127 && draw.texture.clut_offset == 2,
        "palette format bitfields");
    expect(draw.texture.clut_load_bytes == 32 && draw.texture.clut_max_bytes == 2048,
        "zero CLUT load retains prior size and max");
    expect(draw.texture.function == 2 && draw.texture.alpha_from_texture && draw.texture.min_filter == 3 &&
            draw.texture.mag_filter == 1 && draw.texture.wrap_s == 1 && draw.texture.wrap_t == 1,
        "texture sampling/function fields");
    expect(draw.texture.scale_u == 2.5f && draw.texture.scale_v == 2.5f && draw.texture.offset_u == 2.5f &&
            draw.texture.offset_v == 2.5f,
        "texture float24 transforms");
    expect(draw.target.color_address == 0x08002000 && draw.target.depth_address == 0x04003000 &&
            draw.target.color_stride == 256 && draw.target.depth_stride == 512 && draw.target.color_format == 3,
        "render target pointers/formats/strides");
    expect(draw.blend.enabled && draw.blend.source_factor == 1 && draw.blend.destination_factor == 2 &&
            draw.blend.equation == 3 && draw.blend.fixed_source == 0x123456 && draw.blend.fixed_destination == 0x123456,
        "blend contract");
    expect(draw.depth.test_enabled && !draw.depth.write_enabled && draw.depth.function == 5 &&
            draw.depth.range_near == 10 && draw.depth.range_far == 500,
        "depth contract");
    expect(draw.alpha_test.enabled && draw.alpha_test.function == 6 && draw.alpha_test.reference == 0xcd &&
            draw.alpha_test.mask == 0xab,
        "alpha-test bitfields");
    expect(draw.culling_enabled && draw.cull_clockwise && draw.clear_mode && draw.clear_flags == 7, "cull/clear flags");
    expect(draw.viewport.x_scale == 2.5f && draw.viewport.y_scale == 2.5f && draw.viewport.z_scale == 2.5f &&
            draw.viewport.x_offset == 2.5f && draw.viewport.y_offset == 2.5f && draw.viewport.z_offset == 2.5f,
        "viewport float24");
    expect(draw.viewport.offset_x == 3 && draw.viewport.offset_y == 4 && draw.viewport.scissor_x1 == 2 &&
            draw.viewport.scissor_y1 == 3 && draw.viewport.scissor_x2 == 470 && draw.viewport.scissor_y2 == 260,
        "screen offsets mask high bits and scissors unpack");
    expect(draw.material_color == 0x88123456 && draw.lighting_enabled && draw.lighting.material_update == 7 &&
            draw.lighting.reverse_normals && draw.lighting.material_emissive == 0x123456 &&
            draw.lighting.material_diffuse == 0x123456 && draw.lighting.material_specular == 0x123456 &&
            draw.lighting.specular_power == 2.5f,
        "material fields and ambient alpha");
    expect(draw.lighting.ambient_color == 0x123456 && draw.lighting.ambient_alpha == 0x77 && draw.lighting.mode == 1 &&
            draw.fog.enabled && draw.fog.end == 2.5f && draw.fog.scale == 2.5f && draw.fog.color == 0x123456,
        "global light/fog contract");
    for (const auto &light : draw.lighting.lights)
        expect(light.enabled && light.kind == 1 && light.type == 2 && light.position == std::array<float, 3>{1, 2, 3} &&
                light.direction == std::array<float, 3>{1, 2, 3} &&
                light.attenuation == std::array<float, 3>{1, 2, 3} && light.spot_exponent == 4 &&
                light.spot_cutoff == 0.5f && light.ambient == 0x101010 && light.diffuse == 0x202020 &&
                light.specular == 0x303030,
            "independent light register blocks");
    expect(draw.material_version == 10 && draw.environment_version == 70,
        "state change counters advance only affected families");
}
void matrices_transfer_copy() {
    psprecomp::GuestMemory m;
    floats(m, vertices, 1, 2, 3);
    GeState ge;
    DrawCall draw;
    BlockTransfer transfer;
    ge.set_draw_sink([&](const DrawCall &d) { draw = d; });
    ge.set_transfer_sink([&](const BlockTransfer &t) { transfer = t; });
    auto words = setup();
    for (auto number : {0x3au, 0x3cu, 0x3eu, 0x40u, 0x2au}) words.push_back(command(number));
    for (std::uint32_t i = 0; i < 17; ++i)
        for (auto data : {0x3bu, 0x3du, 0x3fu, 0x41u, 0x2bu})
            words.push_back(command(data, f24(static_cast<float>(i + 1))));
    words.insert(words.end(),
        {command(0xb2, 0x6000), command(0xb3, 0x040080), command(0xb4, 0x7000), command(0xb5, 0x080040),
            command(0xeb, 3 | (4 << 10)), command(0xec, 5 | (6 << 10)), command(0xee, 7 | (8 << 10)), command(0xea, 1),
            command(4, 1)});
    execute(ge, m, words);
    expect(draw.world[0] == 1 && draw.world[1] == 2 && draw.world[4] == 4 && draw.world[12] == 10 &&
            draw.world[15] == 1 && draw.world[3] == 0,
        "3x4 expands row-vector translation to shader 4x4");
    expect(draw.view == draw.world && draw.texture_matrix == draw.world && draw.projection[15] == 16,
        "interleaved matrix uploads have independent indices and bounded writes");
    expect(ge.view_matrix_source() == list + 16, "view upload source tracks number command");
    expect(transfer.source == 0x04006000 && transfer.destination == 0x08007000 && transfer.source_stride == 128 &&
            transfer.destination_stride == 64 && transfer.source_x == 3 && transfer.source_y == 4 &&
            transfer.destination_x == 5 && transfer.destination_y == 6 && transfer.width == 8 && transfer.height == 9 &&
            transfer.bytes_per_pixel == 4,
        "block transfer pointer/rectangle/pixel-size contract");
    std::uint32_t source{}, destination{};
    note_vram_copy(0x080a0000, 0x44001000, 32);
    expect(find_vram_copy(0x880a001f, source, destination) && source == 0x04001000 && destination == 0x080a0000,
        "VRAM trace canonicalizes aliases and includes final byte");
    expect(!find_vram_copy(0x080a0020, source, destination), "VRAM trace excludes end boundary");
    note_vram_copy(0x080a0000, 0x04002000, 16);
    expect(
        find_vram_copy(0x080a0000, source, destination) && source == 0x04002000, "new copy replaces same destination");
    for (std::uint32_t i = 0; i < 65; ++i) note_vram_copy(0x080b0000 + i * 64, 0x04000000 + i * 64, 32);
    expect(
        !find_vram_copy(0x080b0000, source, destination) && find_vram_copy(0x080b0000 + 64 * 64, source, destination),
        "copy history bounded to most recent64");
}
void checked_decode_progress_contract() {
    psprecomp::GuestMemory memory;
    floats(memory, vertices, 12.5f, -4.25f, 7.75f);
    std::vector<Vertex> decoded;
    bool exact = true;
    // The documented comparison counter logs each 100000 actual decodes.
    // Keep the work finite and verify every decoded payload, not only the log.
    for (unsigned run = 0; run < 100000; ++run)
        exact = (decode_vertices(memory, vertices, 3u << 7, 1, decoded) == 12 && decoded.size() == 1 &&
                    decoded[0].position == std::array<float, 4>{12.5f, -4.25f, 7.75f, 1}) &&
            exact;
    expect(exact, "comparison progress batch preserves all independently specified vertices");
}

void test_nested_call_restores_offset() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t main = 0x08801000u;
    constexpr std::uint32_t first = 0x08801100u;
    constexpr std::uint32_t nested = 0x08811100u;
    constexpr std::uint32_t second = 0x08801200u;
    constexpr std::uint32_t wrong_second = 0x08821200u;

    memory.store32(main, 0x10080000u);       // BASE 0x08000000
    memory.store32(main + 4u, 0x0A801100u);  // CALL first
    memory.store32(main + 8u, 0x0A801200u);  // CALL second after both RETs
    memory.store32(main + 12u, 0x0F000001u); // FINISH callback
    memory.store32(main + 16u, 0x0C000000u); // END

    memory.store32(first, 0x13000100u);        // OFFSET_ADDR 0x10000
    memory.store32(first + 4u, 0x10080000u);   // BASE remains 0x08000000
    memory.store32(first + 8u, 0x0A801100u);   // CALL nested at 0x08811100
    memory.store32(first + 12u, 0x0B000000u);  // RET
    memory.store32(nested, 0x13000200u);       // OFFSET_ADDR 0x20000
    memory.store32(nested + 4u, 0x0B000000u);  // RET
    memory.store32(second, 0x0B000000u);       // RET to FINISH
    memory.store32(wrong_second, 0x0C000000u); // Wrong target terminates without FINISH

    mhp3rd::gpu::GeState ge;
    mhp3rd::gpu::GeState::ListExecutionState first_state;
    unsigned finishes = 0u;
    ge.set_signal_sink([&](std::uint32_t signal, std::uint32_t) {
        if ((signal & 0x10000u) != 0u) ++finishes;
    });
    bool done = false;
    const std::uint32_t next = ge.execute(memory, first_state, main, 0u, done);
    expect(done && next == main + 20u, "nested CALL/RET reaches outer END");
    expect(finishes == 1u, "nested CALL/RET restores OFFSET_ADDR before outer FINISH");

    memory.store32(main + 0x1000u, 0x10080000u);
    memory.store32(main + 0x1004u, 0x0A801200u);
    memory.store32(main + 0x1008u, 0x0F000002u);
    memory.store32(main + 0x100Cu, 0x0C000000u);
    done = false;
    mhp3rd::gpu::GeState::ListExecutionState second_state;
    ge.execute(memory, second_state, main + 0x1000u, 0u, done);
    expect(done && finishes == 2u, "later display list reaches its own FINISH");
}

void test_bare_end_inside_call_does_not_finish_list() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t main = 0x08802000u;
    constexpr std::uint32_t sublist = 0x08802100u;
    memory.store32(main, 0x10080000u);         // BASE
    memory.store32(main + 4u, 0x0A802100u);    // CALL sublist
    memory.store32(main + 8u, 0x0F000001u);    // FINISH after RET
    memory.store32(main + 12u, 0x0C000000u);   // END after FINISH
    memory.store32(sublist, 0xF1D00018u);      // Unhandled command before bare END
    memory.store32(sublist + 4u, 0x0C8C5E12u); // Bare END with nonzero payload
    memory.store32(sublist + 8u, 0x0B000000u); // RET

    mhp3rd::gpu::GeState ge;
    mhp3rd::gpu::GeState::ListExecutionState list_state;
    unsigned finishes = 0u;
    ge.set_signal_sink([&](std::uint32_t signal, std::uint32_t) {
        if ((signal & 0x10000u) != 0u) ++finishes;
    });
    bool done = false;
    const std::uint32_t next = ge.execute(memory, list_state, main, 0u, done);
    expect(done && next == main + 16u, "bare END inside CALL continues through RET to outer END");
    expect(finishes == 1u, "bare END inside CALL preserves exactly one outer FINISH callback");
}

void test_finish_end_across_stall() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t main = 0x08803000u;
    memory.store32(main, 0x0F000001u);
    memory.store32(main + 4u, 0x0C000000u);
    mhp3rd::gpu::GeState ge;
    mhp3rd::gpu::GeState::ListExecutionState list_state;
    unsigned finishes = 0u;
    ge.set_signal_sink([&](std::uint32_t signal, std::uint32_t) {
        if ((signal & 0x10000u) != 0u) ++finishes;
    });
    bool done = false;
    const std::uint32_t stalled = ge.execute(memory, list_state, main, main + 4u, done);
    expect(!done && stalled == main + 4u && finishes == 1u, "list stalls after FINISH before END");
    const std::uint32_t next = ge.execute(memory, list_state, stalled, 0u, done);
    expect(done && next == main + 8u && finishes == 1u, "END after resumed FINISH completes without another callback");
}

void test_top_level_bare_end_does_not_finish_early() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t main = 0x08803500u;
    memory.store32(main, 0x0CFFFFFFu);       // Bare END must continue.
    memory.store32(main + 4u, 0x0E000123u);  // Actual command after END.
    memory.store32(main + 8u, 0x0F000004u);  // FINISH.
    memory.store32(main + 12u, 0x0C000000u); // Real FINISH/END completion.
    mhp3rd::gpu::GeState ge;
    mhp3rd::gpu::GeState::ListExecutionState list_state;
    std::vector<std::uint32_t> signals;
    ge.set_signal_sink([&](std::uint32_t signal, std::uint32_t) { signals.push_back(signal); });
    bool done = false;
    const std::uint32_t next = ge.execute(memory, list_state, main, 0u, done);
    expect(done && next == main + 16u, "top-level bare END continues through commands to FINISH/END");
    expect(signals.size() == 2u && signals[0] == 0x123u && signals[1] == 0x10004u,
        "commands after bare END and the real FINISH callback both execute");
}

void test_interleaved_list_completion_preserves_stalled_call() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t list_a = 0x08807000u;
    constexpr std::uint32_t child_a = 0x08807100u;
    constexpr std::uint32_t continuation_a = 0x08807200u;
    constexpr std::uint32_t list_b = 0x08807300u;

    memory.store32(list_a, 0x10080000u);       // BASE
    memory.store32(list_a + 4u, 0x0A807100u);  // CALL child A
    memory.store32(list_a + 8u, 0x0A807200u);  // CALL continuation after child RET
    memory.store32(list_a + 12u, 0x0F000001u); // FINISH
    memory.store32(list_a + 16u, 0x0C000000u); // END
    memory.store32(child_a, 0x13000100u);      // OFFSET_ADDR 0x10000
    memory.store32(child_a + 4u, 0x0B000000u); // RET; list A stalls here
    memory.store32(child_a + 8u, 0x0F000011u); // Wrong path if A's frame was lost
    memory.store32(child_a + 12u, 0x0C000000u);
    memory.store32(continuation_a, 0x0B000000u); // RET
    memory.store32(list_b, 0x13000200u);         // OFFSET_ADDR 0x20000
    memory.store32(list_b + 4u, 0x0F000002u);    // FINISH
    memory.store32(list_b + 8u, 0x0C000000u);    // END

    mhp3rd::gpu::GeState ge;
    mhp3rd::gpu::GeState::ListExecutionState state_a;
    mhp3rd::gpu::GeState::ListExecutionState state_b;
    unsigned finishes = 0u;
    std::uint32_t last_finish_pc = 0u;
    ge.set_signal_sink([&](std::uint32_t signal, std::uint32_t pc) {
        if ((signal & 0x10000u) != 0u) {
            ++finishes;
            last_finish_pc = pc;
        }
    });
    bool done_a = false;
    const std::uint32_t stalled = ge.execute(memory, state_a, list_a, child_a + 4u, done_a);
    expect(!done_a && stalled == child_a + 4u, "list A stalls with its CALL frame at RET");

    bool done_b = false;
    const std::uint32_t next_b = ge.execute(memory, state_b, list_b, 0u, done_b);
    expect(done_b && next_b == list_b + 12u, "list B completes while list A remains stalled");

    done_a = false;
    const std::uint32_t next_a = ge.execute(memory, state_a, stalled, 0u, done_a);
    expect(done_a && next_a == list_a + 20u, "resumed list A returns through its saved frame to its own END");
    expect(finishes == 2u && last_finish_pc == list_a + 16u,
        "list A restores OFFSET_ADDR and reaches its own post-RET continuation");
}

void test_invalid_list_reports_address() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t main = 0x08804000u;
    memory.store32(main, 0x10050000u);      // BASE 0x05000000 is unmapped.
    memory.store32(main + 4u, 0x0A000000u); // CALL that address.
    mhp3rd::gpu::GeState ge;
    mhp3rd::gpu::GeState::ListExecutionState list_state;
    bool done = false;
    try {
        (void)ge.execute(memory, list_state, main, 0u, done);
        expect(false, "invalid GE CALL must fail loudly");
    } catch (const psprecomp::Error &error) {
        const std::string message = error.what();
        expect(
            message.find("0x08804000") != std::string::npos && message.find("0x05000000") != std::string::npos && !done,
            "invalid GE CALL identifies list start and unmapped target");
    }
}

void test_completed_call_does_not_leak_into_next_list() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t first = 0x08805000u;
    constexpr std::uint32_t sublist = 0x08805100u;
    constexpr std::uint32_t second = 0x08805200u;
    memory.store32(first, 0x10080000u);
    memory.store32(first + 4u, 0x0A805100u);
    memory.store32(first + 8u, 0x0C000000u);
    memory.store32(sublist, 0x0F000001u);
    memory.store32(sublist + 4u, 0x0C000000u); // FINISH+END at CALL depth one.
    memory.store32(second, 0x0B000000u);       // Empty RET must not return to first list.
    memory.store32(second + 4u, 0x0F000002u);
    memory.store32(second + 8u, 0x0C000000u);
    mhp3rd::gpu::GeState ge;
    mhp3rd::gpu::GeState::ListExecutionState first_state;
    mhp3rd::gpu::GeState::ListExecutionState second_state;
    unsigned finishes = 0u;
    ge.set_signal_sink([&](std::uint32_t signal, std::uint32_t) {
        if ((signal & 0x10000u) != 0u) ++finishes;
    });
    bool done = false;
    (void)ge.execute(memory, first_state, first, 0u, done);
    expect(done && finishes == 1u, "called sublist may finish its list");
    done = false;
    const auto next = ge.execute(memory, second_state, second, 0u, done);
    expect(done && next == second + 12u && finishes == 2u, "completed CALL frames do not redirect a later list");
}

void test_command_budget_fails_loudly() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t main = 0x08806000u;
    memory.store32(main, 0x10080000u);      // BASE 0x08000000.
    memory.store32(main + 4u, 0x08806004u); // JUMP to itself.
    mhp3rd::gpu::GeState ge;
    mhp3rd::gpu::GeState::ListExecutionState list_state;
    bool done = false;
    try {
        (void)ge.execute(memory, list_state, main, 0u, done);
        expect(false, "cyclic GE list must fail at the command budget");
    } catch (const psprecomp::Error &error) {
        const std::string message = error.what();
        expect(message.find("exceeded 2000000 commands") != std::string::npos &&
                message.find("0x08806000") != std::string::npos && !done,
            "command-budget error reports the list and last command location");
    }
}

// --- MHP3RD_GE_STRICT_LISTS: strict list handling and its off switch ---------

GeState make_ge(bool strict) {
    GeState ge;
    ge.set_strict_lists(strict);
    return ge;
}

std::uint32_t base_command() {
    return command(0x10, 0x080000);
}

std::uint32_t call_command(std::uint32_t target) {
    return command(0x0a, target & 0xffffffu);
}

void strict_lists_switch_parsing() {
    static_assert(strict_lists_from_text(nullptr), "unset keeps strict lists on");
    static_assert(!strict_lists_from_text("0"), "0 turns strict lists off");
    expect(strict_lists_from_text(nullptr), "unset variable keeps strict lists on");
    expect(!strict_lists_from_text("0"), "exactly 0 turns strict lists off");
    expect(strict_lists_from_text("1"), "1 keeps strict lists on");
    expect(strict_lists_from_text(""), "an empty value keeps strict lists on");
    expect(strict_lists_from_text("00"), "00 is not the documented off value");
    expect(strict_lists_from_text("0 "), "trailing text after 0 keeps strict lists on");
    expect(strict_lists_from_text(" 0"), "leading text before 0 keeps strict lists on");
    expect(strict_lists_from_text("false"), "only 0 turns strict lists off");
    GeState fresh;
    expect(fresh.strict_lists(), "strict lists are on by default");
    GeState legacy = make_ge(false);
    GeState copy = legacy;
    expect(!copy.strict_lists(), "a copied GE state (free-camera replay) keeps the switch");
}

void bare_end_rules(bool strict) {
    constexpr std::uint32_t start = 0x08820000u;
    psprecomp::GuestMemory memory;
    // NOP-like unhandled command, bare END, FINISH, END.
    store(memory, start, {command(0xff), command(0x0c), command(0x0f, 7), command(0x0c)});
    GeState ge = make_ge(strict);
    std::vector<EndEvent> ends;
    ge.set_end_sink([&](const EndEvent &end) { ends.push_back(end); });
    bool done = false;
    const std::uint32_t next = ge.execute(memory, start, 0, done);
    expect(done, "FINISH followed by END completes the list in both modes");
    if (strict) {
        expect(next == start + 16u, "strict: a bare END is skipped and the list ends at FINISH+END");
        expect(ends.size() == 2u, "strict: both END commands are reported");
        if (ends.size() == 2u) {
            expect(ends[0].end_address == start + 4u && ends[0].preceding_command == command(0xff) && !ends[0].finished,
                "strict: bare END reports its address and preceding command and does not finish");
            expect(
                ends[1].end_address == start + 12u && ends[1].preceding_command == command(0x0f, 7) && ends[1].finished,
                "strict: END after FINISH reports the whole FINISH word and finishes");
        }
    } else {
        expect(next == start + 8u, "legacy: the first END completes the list");
        expect(ends.size() == 1u && ends[0].end_address == start + 4u && ends[0].finished,
            "legacy: the bare END is reported as finishing");
    }
}

void finish_and_signal_end_in_both_modes(bool strict) {
    constexpr std::uint32_t start = 0x08821000u;
    psprecomp::GuestMemory memory;
    store(memory, start, {command(0x0e, 5), command(0x0c)});
    GeState ge = make_ge(strict);
    std::vector<std::uint32_t> signals;
    ge.set_signal_sink([&](std::uint32_t signal, std::uint32_t) { signals.push_back(signal); });
    bool done = false;
    expect(ge.execute(memory, start, 0, done) == start + 8u && done, "SIGNAL followed by END completes the list");
    expect(signals.size() == 1u && signals[0] == 5u, "SIGNAL reaches the signal sink once");

    store(memory, start, {command(0x0f, 6), command(0x0c)});
    done = false;
    signals.clear();
    expect(ge.execute(memory, start, 0, done) == start + 8u && done, "FINISH followed by END completes the list");
    expect(signals.size() == 1u && signals[0] == (0x10000u | 6u), "FINISH reaches the signal sink once");
}

void unmapped_command_handling(bool strict) {
    psprecomp::GuestMemory memory;
    GeState ge = make_ge(strict);
    bool done = true;
    if (strict) {
        try {
            (void)ge.execute(memory, 0, 0, done);
            expect(false, "strict: an unmapped command address throws");
        } catch (const psprecomp::Error &error) {
            const std::string message = error.what();
            expect(message.find("Display list from 0x00000000") != std::string::npos &&
                    message.find("reached unmapped command 0x00000000") != std::string::npos && !done,
                "strict: the error names the list start and the unmapped command");
        }
    } else {
        done = true;
        expect(ge.execute(memory, 0, 0, done) == 0u && !done,
            "legacy: an unmapped command address returns quietly without finishing");
    }
}

// BASE, then `calls` CALLs each to the next word, then FINISH and END: the
// stack is `calls` deep when the list ends.
void store_call_chain(psprecomp::GuestMemory &memory, std::uint32_t start, std::uint32_t calls) {
    store(memory, start, {base_command()});
    for (std::uint32_t i = 0; i < calls; ++i) store(memory, start + 4u + i * 4u, {call_command(start + 8u + i * 4u)});
    store(memory, start + 4u + calls * 4u, {command(0x0f), command(0x0c)});
}

void call_depth_limit(bool strict) {
    constexpr std::uint32_t start = 0x08822000u;
    for (const std::uint32_t calls : {1023u, 1024u}) {
        psprecomp::GuestMemory memory;
        store_call_chain(memory, start, calls);
        GeState ge = make_ge(strict);
        bool done = false;
        expect(ge.execute(memory, start, 0, done) == start + 12u + calls * 4u && done,
            calls == 1023u ? "nesting one below the CALL depth limit works"
                           : "nesting up to the CALL depth limit works");
    }
    psprecomp::GuestMemory memory;
    store_call_chain(memory, start, 1025u);
    GeState ge = make_ge(strict);
    bool done = false;
    if (strict) {
        try {
            (void)ge.execute(memory, start, 0, done);
            expect(false, "strict: the CALL past the depth limit throws");
        } catch (const psprecomp::Error &error) {
            const std::string message = error.what();
            expect(message.find("CALL depth limit of 1024") != std::string::npos &&
                    message.find("Display list from 0x08822000") != std::string::npos && !done,
                "strict: the depth error names the limit and the list start");
        }
    } else {
        expect(ge.execute(memory, start, 0, done) == start + 12u + 1025u * 4u && done,
            "legacy: CALLs nest past the strict limit");
    }
}

void command_budget(bool strict) {
    constexpr std::uint32_t start = 0x08823000u;
    psprecomp::GuestMemory memory;
    store(memory, start, {base_command(), command(0x08, (start + 4u) & 0xffffffu)}); // JUMP to itself
    GeState ge = make_ge(strict);
    bool done = true;
    if (strict) {
        try {
            (void)ge.execute(memory, start, 0, done);
            expect(false, "strict: a runaway list throws at the command budget");
        } catch (const psprecomp::Error &error) {
            expect(std::string(error.what()).find("exceeded 2000000 commands") != std::string::npos && !done,
                "strict: the budget error names the limit");
        }
    } else {
        expect(ge.execute(memory, start, 0, done) == start + 4u && !done,
            "legacy: a runaway list stops at the budget without throwing or finishing");
    }
}

// Two lists, each stalled inside its own CALL, are resumed in turn.
void per_list_call_stacks(bool strict) {
    constexpr std::uint32_t list_a = 0x08824000u, sub_a = 0x08824100u;
    constexpr std::uint32_t list_b = 0x08824200u, sub_b = 0x08824300u;
    psprecomp::GuestMemory memory;
    store(memory, list_a, {base_command(), call_command(sub_a), command(0x0f, 1), command(0x0c)});
    store(memory, sub_a, {command(0x0b)});
    store(memory, list_b, {base_command(), call_command(sub_b), command(0x0f, 2), command(0x0c)});
    store(memory, sub_b, {command(0x0b)});
    GeState ge = make_ge(strict);
    ListExecutionState state_a, state_b;
    bool done_a = false, done_b = false;
    expect(ge.execute(memory, state_a, list_a, sub_a, done_a) == sub_a && !done_a, "list A stalls inside its CALL");
    expect(ge.execute(memory, state_b, list_b, sub_b, done_b) == sub_b && !done_b, "list B stalls inside its CALL");
    const std::uint32_t after_a = ge.execute(memory, state_a, sub_a, 0, done_a);
    if (strict) {
        expect(done_a && after_a == list_a + 16u, "strict: list A returns through its own CALL stack");
        expect(ge.execute(memory, state_b, sub_b, 0, done_b) == list_b + 16u && done_b,
            "strict: list B still returns through its own CALL stack");
    } else {
        expect(done_a && after_a == list_b + 16u,
            "legacy: the stack is shared, so list A returns into list B's continuation");
    }
}

}
int main() {
    const bool checked = std::getenv("MHP3RD_CHECK_DECODE") != nullptr;
    std::ostringstream diagnostics;
    auto *original_output = checked ? std::cout.rdbuf(diagnostics.rdbuf()) : nullptr;
    control_flow();
    bounded_list_and_offset_contracts();
    vertex_formats();
    mirrored_vram_vertices_and_indices();
    draws();
    registers();
    matrices_transfer_copy();
    test_nested_call_restores_offset();
    test_bare_end_inside_call_does_not_finish_list();
    test_finish_end_across_stall();
    test_top_level_bare_end_does_not_finish_early();
    test_interleaved_list_completion_preserves_stalled_call();
    test_invalid_list_reports_address();
    test_completed_call_does_not_leak_into_next_list();
    test_command_budget_fails_loudly();
    strict_lists_switch_parsing();
    for (const bool strict : {true, false}) {
        bare_end_rules(strict);
        finish_and_signal_end_in_both_modes(strict);
        unmapped_command_handling(strict);
        call_depth_limit(strict);
        command_budget(strict);
        per_list_call_stacks(strict);
    }
    if (checked) {
        checked_decode_progress_contract();
        std::cout.rdbuf(original_output);
        std::cout << diagnostics.str();
        expect(diagnostics.str().find("[material] cmd=0x53 value=0x7") != std::string::npos,
            "material trace identifies the actual register and value");
        expect(diagnostics.str().find("[lighting] cmd=0x18") != std::string::npos &&
                diagnostics.str().find("[lit-draw] vtype=") != std::string::npos,
            "lighting trace includes command values and actual lit draw inputs");
        expect(diagnostics.str().find("[fbtex] block transfer 0x4006000") != std::string::npos &&
                diagnostics.str().find("[fbtex] unhandled GE command 0xff") != std::string::npos,
            "framebuffer trace identifies block transfers and ignored commands");
        expect(diagnostics.str().find("[decode-check] 100000 runs compared, 0 differed") != std::string::npos,
            "comparison progress reports its real bounded successful milestone");
        expect(diagnostics.str().find(" differs over ") == std::string::npos,
            "fast and scalar decoding agree exactly across format and skinning contracts");
    }
    std::cout << (failures ? "FAIL" : "PASS") << ": GE contracts (" << failures << " failures)\n";
    return failures ? 1 : 0;
}
