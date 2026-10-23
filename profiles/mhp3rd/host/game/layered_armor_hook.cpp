#include "game/layered_armor.hpp"

#include "game/guest_ram.hpp"
#include "mods/transmog.hpp"

#include "psprecomp/runtime.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

// The generated units the game's code was compiled into, found by CMake in the
// corpus (MHP3RD_LAYERED_FUNCTIONS in CMakeLists.txt).
#if defined(MHP3RD_LAYERED_FUNCTIONS_INC)
namespace psprecomp {
#define MHP3RD_LAYERED_FUNCTION(address, unit) void unit(Runtime &, AllegrexContext &);
#include MHP3RD_LAYERED_FUNCTIONS_INC
#undef MHP3RD_LAYERED_FUNCTION
} // namespace psprecomp
#endif

// The appearance selector itself lives in mods/transmog.cpp. This file keeps
// the two wrappers that do not depend on how the appearance is chosen: the
// Smithy preview guard (docs/LAYERED_ARMOR.md, "Smithy previews") and the
// developer device-test observation of the hunter the real loader steps.
namespace mhp3rd::game::layered {
namespace {

struct Compiled {
    std::uint32_t address;
    psprecomp::Runtime::RecompiledFunction function;
    const char *name;
};
constexpr Compiled kCompiled[] = {
#if defined(MHP3RD_LAYERED_FUNCTIONS_INC)
#define MHP3RD_LAYERED_FUNCTION(address, unit) {address, &psprecomp::unit, #unit},
#include MHP3RD_LAYERED_FUNCTIONS_INC
#undef MHP3RD_LAYERED_FUNCTION
#endif
    {0u, nullptr, nullptr},
};

const Compiled *compiled(std::uint32_t address) {
    for (const Compiled &entry : kCompiled)
        if (entry.address == address && entry.function != nullptr) return &entry;
    return nullptr;
}

struct CodeWord {
    std::uint32_t address;
    std::uint32_t word;
};
// What the observation relies on: the load step reading the hunter's state.
constexpr CodeWord kLoadSignature[] = {
    {0x088A5598u, 0x8E230B80u},  // lw v1,0xB80(s1): the step reads the state
    {0x088A55BCu, 0xAE320B80u},  // sw s2,0xB80(s1): 0 becomes 1, the first part
    {0x088A5610u, 0x8E250B84u},  // lw a1,0xB84(s1): the part
    {0x088A5618u, 0x8C6200A8u},  // lw v0,0xA8(v1): the object's file function for it
    {0x088A5638u, 0x8C430B8Cu},  // lw v1,0xB8C(v0): compared with the file loaded
    {0x088A53DCu, 0x28420003u},  // slti v0,v0,3: the request cancels a load below 3
    {0x088A5404u, 0xAE000B84u},  // sw zero,0xB84(s0): and sets the part
    {0x088A5408u, 0xAE000B80u},  // sw zero,0xB80(s0): and the state to 0
    {0x088BD258u, 0x8C620094u},  // lw v0,0x94(v1): the driver steps through the vtable
};

struct State {
    bool trace{};
    const Compiled *preview_original{};
    const Compiled *step{};
    std::uint32_t observed_object{};
    const psprecomp::GuestMemory *observed_memory{};
    std::array<std::uint32_t, 16> observed_map{};
};
State &state() {
    static State s;
    return s;
}

std::array<std::uint32_t, 16> map_header(const psprecomp::GuestMemory &memory) {
    constexpr std::uint32_t slot = 0x0A055E80u;
    std::array<std::uint32_t, 16> result{};
    if (memory.contains(slot, 64u))
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = memory.load32(slot + static_cast<std::uint32_t>(i) * 4u);
    return result;
}

bool device_observations() {
#if defined(MHP3RD_DEBUG_MENU)
    const char *mode = std::getenv("MHP3RD_DEVICE_TEST");
    return mode && std::string(mode) == "1";
#else
    return false;
#endif
}

// The look the live hunter was last loaded with, in the order of the game's
// parts (chest, arms, waist, legs, head). The Smithy preview is private to
// one sex, so only a donor of that sex can be described by an equipment id;
// any other part stays real, which leaves the preview to the game's own code.
Pieces loaded_pieces(std::uint8_t sex) {
    Pieces pieces{kReal, kReal, kReal, kReal, kReal};
    const mods::transmog::Set &applied = mods::transmog::applied_selection();
    for (std::uint8_t part = 0; part < kParts; ++part) {
        const auto &armor = applied.armor[part == 4u ? 0u : part + 1u];
        if (armor && armor->gender == sex) pieces[part] = armor->id;
    }
    return pieces;
}

void preview_copy_part(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    State &s = state();
    GuestRam ram(runtime.memory());
    // At this constructor call, s3 holds the live hunter, a0 the preview and
    // a1 the part. Use the look the hunter was loaded with, even if the menu
    // staged a different one since.
    const std::uint32_t source = ctx.gpr[19];
    if (ram.contains(source + 0x458u, 1u)) {
        const Pieces loaded = loaded_pieces(ram.load8(source + 0x458u));
        if (prepare_preview_copy(ram, ctx.gpr[31], ctx.gpr[4], source, ctx.gpr[5], loaded) && s.trace)
            std::cout << "[layered-preview] copied part=" << ctx.gpr[5] << " appearance=" << loaded[ctx.gpr[5]]
                      << '\n';
    }
    s.preview_original->function(runtime, ctx);
}

void observe_load_step(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    State &s = state();
    const GuestRam ram(runtime.memory());
    const std::uint32_t object = ctx.gpr[4];
    if (own_hunter(ram, object)) {
        s.observed_object = object;
        s.observed_memory = &runtime.memory();
        s.observed_map = map_header(runtime.memory());
    }
    s.step->function(runtime, ctx);
}

} // namespace

void install_guards(psprecomp::Runtime &runtime) {
    State &s = state();
    const char *text = std::getenv("MHP3RD_TRACE_TRANSMOG");
    s.trace = text != nullptr && *text != '\0' && std::string(text) != "0";
    // This guard stays in place whether or not Layered Sets is on: the live
    // hunter can still carry the old loaded appearance until the next scene
    // load. It changes nothing unless a layered part was loaded.
    const char *preview_fix = std::getenv("MHP3RD_LAYERED_PREVIEW_FIX");
    if (!(preview_fix && std::string(preview_fix) == "0")) {
        s.preview_original = compiled(kCopyPart);
        if (s.preview_original != nullptr && (runtime.memory().load32(kCopyPart) != 0x27BDFFF0u ||
                                              runtime.memory().load32(kCopyPart + 4u) != 0xAFB20008u))
            s.preview_original = nullptr;
        if (s.preview_original != nullptr)
            runtime.register_function(kCopyPart, &preview_copy_part, "mhp3rd_layered_preview_copy");
        else if (runtime.has_function(kCopyPart))
            std::cerr << "[layered] the Smithy preview constructor is not as traced; the preview guard is "
                         "unavailable\n";
    }
    if (!device_observations()) return;
    s.step = compiled(kLoadStep);
    for (const CodeWord &expected : kLoadSignature)
        if (runtime.memory().load32(expected.address) != expected.word) s.step = nullptr;
    if (s.step == nullptr) {
        std::cerr << "[layered] the hunter's load step is not as traced; device observations are unavailable\n";
        return;
    }
    runtime.register_function(kLoadStep, &observe_load_step, "mhp3rd_device_hunter_observation");
}

std::uint32_t observed_hunter(const psprecomp::GuestMemory &memory) {
    const State &s = state();
    if (!device_observations() || s.observed_memory != &memory || s.observed_map != map_header(memory)) return 0u;
    const GuestRam ram(const_cast<psprecomp::GuestMemory &>(memory));
    return s.observed_object && own_hunter(ram, s.observed_object) ? s.observed_object : 0u;
}

} // namespace mhp3rd::game::layered
