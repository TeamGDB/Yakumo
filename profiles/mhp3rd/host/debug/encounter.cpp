#include "debug/encounter.hpp"

#include "debug/game_state.hpp"
#include "game/guest_ram.hpp"

#include <bit>
#include <cmath>
#include <sstream>
#include <utility>

namespace mhp3rd::debug::encounter {
namespace {
constexpr std::uint32_t kQuestData = 0x08A3A630u;
constexpr std::uint32_t kMapState = 0x09FBEBA0u;
constexpr std::uint32_t kMapId = 0x9A30u;
constexpr std::uint32_t kArea = 0x9A31u;
constexpr std::uint32_t kActorArea = 0xD6u;
constexpr std::uint32_t kHunter = 0x096498D0u;
constexpr std::uint32_t kBariothOverlay = 0x0A1BB000u;
constexpr std::uint32_t kSlots = 20u; // manager +0x250; its reset clears exactly twenty pointers
constexpr std::uint32_t kCamera = 0x08A2F958u;

bool user(const game::Ram &ram, std::uint32_t at, std::size_t size) {
    return at >= 0x08800000u && (at & 3u) == 0u && ram.contains(at, size);
}

bool overlay(const game::Ram &ram, std::uint32_t at, const char *name) {
    return user(ram, at, 64u) && ram.load32(at) == 0x336F574Du && ram.load32(at + 8u) == at &&
        game::read_text(ram, at + 32u) == name;
}

bool record(const game::Ram &ram) {
    if (!ram.contains(kQuestData, 0x30u)) return false;
    const auto offset = ram.load32(kQuestData);
    if (offset < 0x30u || offset > 0x10000u || offset % 4u || !ram.contains(kQuestData + offset, 0x48u)) return false;
    if (!ram.contains(quests::kCharacterPointer, 4u)) return false;
    const auto character = ram.load32(quests::kCharacterPointer);
    return user(ram, character, quests::kQuestIdOffset + 2u) &&
        ram.load16(character + quests::kQuestIdOffset) == kQuest && ram.load16(kQuestData + offset + 0x1Cu) == kQuest &&
        ram.load8(kQuestData + offset + 0x20u) == 4u;
}

bool point(const game::Ram &ram, std::uint32_t at, std::array<float, 3> &out) {
    if (!user(ram, at, 12u)) return false;
    for (std::uint32_t i = 0; i < 3u; ++i) {
        out[i] = std::bit_cast<float>(ram.load32(at + i * 4u));
        if (!std::isfinite(out[i]) || std::abs(out[i]) > 1'000'000.0f) return false;
    }
    return true;
}

bool in_view(const game::Ram &ram, const std::array<float, 3> &position) {
    if (!ram.contains(kCamera, 4u)) return false;
    const auto camera = ram.load32(kCamera);
    if (!user(ram, camera, 0x1010u)) return false;
    const float p[4] = {position[0], position[1] + 100.0f, position[2], 1.0f};
    float eye[4]{}, clip[4]{};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 4; ++col)
            eye[row] += std::bit_cast<float>(ram.load32(camera + 0xF50u + (col * 4u + row) * 4u)) * p[col];
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 4; ++col)
            clip[row] += std::bit_cast<float>(ram.load32(camera + 0xFD0u + (col * 4u + row) * 4u)) * eye[col];
    return std::isfinite(clip[0]) && std::isfinite(clip[1]) && std::isfinite(clip[3]) && clip[3] > 1.0f &&
        std::abs(clip[0] / clip[3]) < 0.9f && std::abs(clip[1] / clip[3]) < 0.9f;
}

bool relocate(game::Ram &ram, const View &view) {
    // The preset's traced landing is on the flat frozen lake. Refuse a flying
    // monster or a different spawn/terrain; this is not an arbitrary warp API.
    if (std::abs(view.target[0] - 1500.0f) > 200.0f || std::abs(view.target[2] - 1100.0f) > 200.0f ||
        view.target[1] < 450.0f || view.target[1] > 550.0f)
        return false;
    auto destination = view.target;
    destination[2] += 700.0f;
    // World translation, physics position and previous position must agree.
    // Otherwise the next physics step sweeps across the entire old position.
    for (const auto offset : {0x40u, 0x80u, 0x90u})
        for (std::uint32_t axis = 0; axis < 3u; ++axis)
            ram.store32(kHunter + offset + axis * 4u, std::bit_cast<std::uint32_t>(destination[axis]));
    return true;
}

const char *name(Phase phase) {
    switch (phase) {
    case Phase::Idle:
        return "idle";
    case Phase::Village:
        return "village";
    case Phase::Departing:
        return "departing";
    case Phase::Loading:
        return "loading";
    case Phase::Camera:
        return "camera";
    case Phase::Ready:
        return "ready";
    case Phase::Finished:
        return "finished";
    case Phase::Failed:
        return "failed";
    }
    return "unknown";
}
} // namespace

bool supported(const game::Ram &ram) {
    return ram.contains(kInitialAreaChoice, 0x78u) && ram.load32(kInitialAreaChoice) == 0x27BDFFE0u &&
        ram.load32(kInitialAreaChoice + 8u) == 0x3C0208ABu && ram.load32(kInitialAreaChoice + 0x3Cu) == 0x90C50718u &&
        ram.load32(0x088BDF5Cu) == 0x0E2338ECu;
}

View inspect(const game::Ram &ram) {
    View v;
    const auto bad = [&](const char *why) {
        v.reason = why;
        return v;
    };
    if (!p3rd::on_quest(ram) || !record(ram)) return bad("original quest 507 is not active");
    if (!overlay(ram, quests::kMapSlot, "P_m04a07d.ovl")) return bad("Tundra area 7 is not loaded");
    if (!ram.contains(kMapState, 4u)) return bad("map state is unavailable");
    const auto map = ram.load32(kMapState);
    if (!user(ram, map, kArea + 1u) || ram.load8(map + kMapId) != 4u || ram.load8(map + kArea) != 7u)
        return bad("map state does not match the loaded area");
    if (!ram.contains(p3rd::kMonsterTable, kSlots * 4u)) return bad("actor capacity is unavailable");
    for (std::uint32_t i = 0; i < kSlots; ++i) {
        const auto actor = ram.load32(p3rd::kMonsterTable + i * 4u);
        if (!actor) continue;
        if (!user(ram, actor, 0xE00u) || ram.load8(actor + p3rd::kMonsterKind) >= p3rd::kMonsterKinds)
            return bad("invalid actor table entry");
        ++v.population;
        if (ram.load8(actor + p3rd::kMonsterKind) != 5u) continue;
        if (v.actor) return bad("more than one Barioth is loaded");
        v.actor = actor;
    }
    if (!v.actor) return bad("Barioth has not been created");
    if (static_cast<std::int16_t>(ram.load16(v.actor + p3rd::kMonsterHealth)) <= 0 ||
        static_cast<std::int16_t>(ram.load16(v.actor + p3rd::kMonsterMostHealth)) <= 0)
        return bad("Barioth is not alive");
    if (ram.load8(v.actor + kActorArea) != 7u || (ram.load32(v.actor + 4u) & 4u))
        return bad("Barioth is outside the hunter area");
    if (!overlay(ram, kBariothOverlay, "em005m0.ovl")) return bad("Barioth code overlay is not loaded");
    const auto vt = ram.load32(v.actor);
    const auto code = ram.load32(kBariothOverlay + 12u);
    const auto bytes = std::uint64_t{64u} + code + ram.load32(kBariothOverlay + 16u);
    if (bytes > 0x300000u || !ram.contains(kBariothOverlay, bytes) || vt < kBariothOverlay + 64u ||
        std::uint64_t{vt} + 0x50u > kBariothOverlay + bytes || !user(ram, vt, 0x50u))
        return bad("Barioth vtable is not owned by its loaded overlay");
    const auto getter = ram.load32(vt + 0x4Cu);
    if (getter < kBariothOverlay + 64u || std::uint64_t{getter} + 12u > kBariothOverlay + 64u + code ||
        !user(ram, getter, 12u) || ram.load32(getter) != 0x24820DD0u || ram.load32(getter + 4u) != 0x03E00008u ||
        ram.load32(getter + 8u) != 0u)
        return bad("Barioth model accessor is unknown");
    v.model = ram.load32(v.actor + 0xDD4u);
    if (!user(ram, v.model, 64u) || ram.load32(v.model) != 0x006F6D70u || ram.load32(v.model + 4u) != 0x00323031u ||
        ram.load32(v.model + 8u) < 64u || ram.load32(v.model + 8u) > 0x800000u ||
        !ram.contains(v.model, ram.load32(v.model + 8u)))
        return bad("Barioth PMO model is not ready");
    if (!user(ram, kHunter, 0x28Cu)) return bad("local hunter memory is unavailable");
    const auto hunter_vt = ram.load32(kHunter);
    if (hunter_vt != 0x0A1A5A90u || !user(ram, hunter_vt, 16u) || ram.load32(hunter_vt + 12u) != 0x0A102464u ||
        ram.load8(kHunter + 0x62u) != 0u || ram.load8(kHunter + kActorArea) != 7u ||
        static_cast<std::int16_t>(ram.load16(kHunter + 0x246u)) <= 0)
        return bad("local hunter is not ready in the target area");
    if (!point(ram, v.actor + 0x80u, v.target) || !point(ram, kHunter + 0x80u, v.hunter))
        return bad("actor coordinates are unknown");
    v.ticks = ram.load32(p3rd::kQuestTimeLeft);
    if (!v.ticks || v.ticks > 90000u) return bad("quest clock is not ready");
    v.in_view = in_view(ram, v.target);
    return v;
}

std::string Controller::begin(const game::Ram &ram, const quests::Quest &quest, std::uint64_t now_ms) {
    if (phase_ != Phase::Idle && phase_ != Phase::Finished && phase_ != Phase::Failed)
        return "encounter refused: preparation is already active";
    if (quest.id != kQuest || !supported(ram)) return "encounter refused: unsupported quest or executable";
    if (const auto why = quests::start_blocked(ram); !why.empty()) return "encounter refused: " + why;
    cancel();
    quest_ = quest;
    phase_ = Phase::Village;
    deadline_ = now_ms + 90000u;
    return "encounter preparing barioth-tundra";
}

void Controller::fail(std::string reason) {
    phase_ = Phase::Failed;
    reason_ = std::move(reason);
}

bool Controller::initial_choice(game::Ram &ram, std::uint32_t return_pc, bool resumed, bool writes_allowed) {
    if (phase_ != Phase::Departing || return_pc != kInitialAreaReturn) return false;
    if (!writes_allowed || !resumed || !supported(ram) || !p3rd::on_quest(ram) || !record(ram)) {
        fail("initial-area lifecycle guard failed");
        return false;
    }
    const auto list = ram.load32(kQuestData + 0x20u);
    if (list < 0x30u || list > 0x10000u || list % 4u || !ram.contains(kQuestData + list, 16u) ||
        ram.load32(kQuestData + list) != 1u) {
        fail("quest does not declare exactly one initial monster");
        return false;
    }
    const auto spawn = ram.load32(kQuestData + list + 12u);
    if (spawn < 0x30u || spawn > 0x10000u || spawn % 4u || !ram.contains(kQuestData + spawn, 0x30u) ||
        ram.load16(kQuestData + spawn) != 5u || ram.load8(kQuestData + spawn + 6u) != 7u) {
        fail("original Barioth spawn record is unavailable");
        return false;
    }
    if (!ram.contains(kMapState, 4u)) {
        fail("initial map state is unavailable");
        return false;
    }
    const auto map = ram.load32(kMapState);
    if (!user(ram, map, kArea + 1u) || ram.load8(map + kMapId) != 4u || ram.load8(map + kArea) != 0u ||
        !ram.contains(p3rd::kMonsterTable, kSlots * 4u)) {
        fail("initial map state is unknown");
        return false;
    }
    for (std::uint32_t i = 0; i < kSlots; ++i)
        if (ram.load32(p3rd::kMonsterTable + i * 4u)) {
            fail("actor initialization has already started");
            return false;
        }
    ram.store8(map + kArea, 7u);
    phase_ = Phase::Loading;
    return true;
}

Action Controller::tick(game::Ram &ram, std::uint64_t now_ms, bool writes_allowed) {
    if (phase_ == Phase::Idle || phase_ == Phase::Failed || phase_ == Phase::Finished) return Action::None;
    if (!writes_allowed) {
        fail("offline mutation guard closed");
        return Action::None;
    }
    if (now_ms > deadline_ && phase_ != Phase::Ready) {
        fail("preparation timed out: " + reason_);
        return Action::None;
    }
    if (phase_ == Phase::Village) {
        if (!quests::start_blocked(ram).empty()) {
            stable_since_ = 0u;
        } else if (!stable_since_) {
            stable_since_ = now_ms;
        } else if (now_ms - stable_since_ >= 2000u) {
            const auto result = quests::start(ram, quest_);
            if (result.rfind("started quest ", 0u) != 0u)
                fail("quest departure was refused");
            else {
                phase_ = Phase::Departing;
                reason_ = "waiting for native initial-area selection";
                deadline_ = now_ms + 45000u;
            }
        }
        return Action::None;
    }
    if (phase_ == Phase::Departing) return Action::None;
    if (!p3rd::on_quest(ram)) {
        phase_ = Phase::Finished;
        view_ = {};
        reason_ = "quest overlay lifetime ended";
        return Action::None;
    }
    auto next = inspect(ram);
    if (phase_ == Phase::Ready) {
        if (!next.valid() || next.actor != view_.actor || next.model != view_.model) {
            phase_ = Phase::Finished;
            reason_ = "quest, target or model lifetime ended";
            view_ = {};
        } else {
            view_ = next;
        }
        return Action::None;
    }
    reason_ = next.reason;
    if (!next.valid()) {
        stable_frames_ = 0u;
        return Action::None;
    }
    view_ = next;
    if (phase_ == Phase::Loading) {
        if (next.ticks >= 90000u || next.target[1] < 450.0f || next.target[1] > 550.0f ||
            std::abs(next.target[1] - previous_height_) > 0.2f) {
            previous_height_ = next.target[1];
            stable_frames_ = 0u;
            reason_ = "waiting for Barioth to land and the quest clock to advance";
        } else if (++stable_frames_ >= 3u) {
            if (!relocate(ram, next)) {
                fail("Barioth is outside the verified frozen-lake landing");
            } else {
                phase_ = Phase::Camera;
                first_ticks_ = next.ticks;
                deadline_ = now_ms + 10000u;
                stable_frames_ = 0u;
                reason_ = "waiting for follow camera and visible target";
                return Action::Focus;
            }
        }
    } else if (phase_ == Phase::Camera && next.in_view && next.ticks < first_ticks_ &&
        std::hypot(next.target[0] - next.hunter[0], next.target[2] - next.hunter[2]) < 1800.0f) {
        if (++stable_frames_ >= 3u) {
            phase_ = Phase::Ready;
            reason_.clear();
        }
    } else {
        stable_frames_ = 0u;
        reason_ = "waiting for follow camera and visible target";
    }
    return Action::None;
}

void Controller::cancel() {
    *this = Controller{};
}

std::string Controller::status() const {
    std::ostringstream out;
    out << "encounter {\"schema\":1,\"preset\":\"barioth-tundra\",\"phase\":\"" << name(phase_)
        << "\",\"quest_id\":507,\"actor\":" << view_.actor << ",\"model\":" << view_.model
        << ",\"population\":" << view_.population << ",\"in_view\":" << (view_.in_view ? "true" : "false")
        << ",\"quest_ticks\":" << view_.ticks << ",\"reason\":\"" << reason_ << "\"}";
    return out.str();
}

} // namespace mhp3rd::debug::encounter
