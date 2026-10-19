#include "mods/mhp3rd_game_events.hpp"

#include "mods/mod_ini.hpp"

#include "overlays.hpp"

#include "psprecomp/guest_memory.hpp"

#include "yakumo/mod.h"

namespace mhp3rd::mods::p3rd {
namespace {

// The quest overlay in the task slot is the game on a quest; the accepted
// quest's id is beside the character.
constexpr std::uint32_t kTaskSlot = 0x0A05E600u;
constexpr std::uint32_t kCharacterPointer = 0x08AB3640u;
constexpr std::uint32_t kQuestIdOffset = 0x60472u;

} // namespace

std::uint32_t GameWatch::overlay_loaded(const psprecomp::GuestMemory &memory, std::uint32_t base) {
    const std::optional<LoadedOverlay> loaded = overlay_at(memory, base);
    if (!loaded) return 0u;
    overlays_[loaded->base] = loaded->name;
    if (const auto told = told_.find(loaded->base); told != told_.end() && told->second == loaded->name) return 0u;
    told_[loaded->base] = loaded->name;
    return loaded->base;
}

void GameWatch::look(const psprecomp::GuestMemory &memory, Fire fire) {
    for (auto it = overlays_.begin(); it != overlays_.end();) {
        const std::optional<LoadedOverlay> now = overlay_at(memory, it->first);
        if (now && now->base == it->first && now->name == it->second) {
            ++it;
            continue;
        }
        const std::uint32_t base = it->first;
        it = overlays_.erase(it);
        fire(YMOD_EVENT_OVERLAY_UNLOADED, base);
    }
    const std::optional<LoadedOverlay> task = overlay_at(memory, kTaskSlot);
    const bool on_quest = task && task->base == kTaskSlot && iequals(task->name, "game_task");
    if (on_quest && !quest_) {
        std::uint32_t id = 0u;
        if (memory.contains(kCharacterPointer, 4u)) {
            const std::uint32_t at = memory.load32(kCharacterPointer) + kQuestIdOffset;
            if (memory.contains(at, 2u)) id = memory.load16(at);
        }
        quest_ = id;
        fire(YMOD_EVENT_QUEST_STARTED, id);
    } else if (!on_quest && quest_) {
        const std::uint32_t id = *quest_;
        quest_.reset();
        fire(YMOD_EVENT_QUEST_ENDED, id);
    }
}

} // namespace mhp3rd::mods::p3rd
