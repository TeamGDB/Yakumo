#include "debug/game_state.hpp"

#include "debug/debug_tools.hpp"
#include "debug/quest_start.hpp"
#include "camera/lock_on.hpp"
#include "game/guest_ram.hpp"
#include "game/layered_armor.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>
#include <bit>
#include <cmath>

namespace mhp3rd::debug {
namespace p3rd {
namespace {

std::uint32_t item_slot(std::uint32_t slot) {
    return kItemBox + slot * 4u;
}
std::uint32_t equipment_slot(std::uint32_t slot) {
    return kEquipmentBox + slot * kEquipmentRecord;
}

} // namespace

ItemInfo item_info(const Ram &ram, std::uint16_t id) {
    ItemInfo info;
    const std::uint32_t at = kItemData + static_cast<std::uint32_t>(id) * kItemRecord;
    if (!ram.contains(at, kItemRecord)) return info;
    info.category = ram.load8(at + 4u);
    info.rarity = ram.load8(at + 5u);
    info.carry = ram.load8(at + 6u);
    info.use = ram.load8(at + 7u);
    info.buy = read32(ram, at + 12u);
    info.sell = read32(ram, at + 16u);
    return info;
}

ItemGroup item_group(const ItemInfo &info) {
    switch (info.category) {
    case 1:
        return ItemGroup::Ammo;
    case 3:
        return ItemGroup::Decoration;
    case 0:
        if (info.use != 0u) return ItemGroup::Consumable;
        // Materials stack to 99 in the pouch and are not used from it.
        return info.carry == kMostPerStack ? ItemGroup::Material : ItemGroup::Other;
    default:
        return ItemGroup::Other;
    }
}

const char *group_name(ItemGroup group) {
    switch (group) {
    case ItemGroup::Material:
        return "Material";
    case ItemGroup::Consumable:
        return "Consumable";
    case ItemGroup::Ammo:
        return "Ammo";
    case ItemGroup::Decoration:
        return "Decoration";
    default:
        return "Other";
    }
}

std::vector<Item> item_list(const Ram &ram) {
    std::vector<Item> items;
    const std::vector<std::string> names = text_table(ram, kTextBlock, kItemNameTable);
    // Id 0 is the empty slot.
    for (std::size_t id = 1; id < names.size(); ++id) {
        if (names[id].empty()) continue;
        const auto item_id = static_cast<std::uint16_t>(id);
        items.push_back({item_id, names[id], item_group(item_info(ram, item_id))});
    }
    return items;
}

std::vector<ItemStack> item_box(const Ram &ram) {
    std::vector<ItemStack> box(kItemBoxSlots);
    for (std::uint32_t i = 0; i < kItemBoxSlots; ++i)
        box[i] = {ram.load16(item_slot(i)), ram.load16(item_slot(i) + 2u)};
    return box;
}

std::uint32_t box_count(const Ram &ram, std::uint16_t id) {
    std::uint32_t total = 0u;
    for (const ItemStack &s : item_box(ram))
        if (s.id == id) total += s.count;
    return total;
}

std::uint32_t free_item_slots(const Ram &ram) {
    std::uint32_t free = 0u;
    for (const ItemStack &s : item_box(ram))
        if (s.id == 0u) ++free;
    return free;
}

std::uint32_t give_item(Ram &ram, std::uint16_t id, std::uint32_t count) {
    if (id == 0u || count == 0u) return 0u;
    std::uint32_t left = count;
    // Top up the item's own stacks first.
    for (std::uint32_t i = 0; i < kItemBoxSlots && left > 0u; ++i) {
        if (ram.load16(item_slot(i)) != id) continue;
        const std::uint16_t have = ram.load16(item_slot(i) + 2u);
        if (have >= kMostPerStack) continue;
        const std::uint32_t add = std::min<std::uint32_t>(left, kMostPerStack - have);
        ram.store16(item_slot(i) + 2u, static_cast<std::uint16_t>(have + add));
        left -= add;
    }
    for (std::uint32_t i = 0; i < kItemBoxSlots && left > 0u; ++i) {
        if (ram.load16(item_slot(i)) != 0u) continue;
        const std::uint32_t add = std::min<std::uint32_t>(left, kMostPerStack);
        ram.store16(item_slot(i), id);
        ram.store16(item_slot(i) + 2u, static_cast<std::uint16_t>(add));
        left -= add;
    }
    return count - left;
}

std::uint32_t remove_item(Ram &ram, std::uint16_t id) {
    if (id == 0u) return 0u;
    std::uint32_t removed = 0u;
    for (std::uint32_t i = 0; i < kItemBoxSlots; ++i) {
        if (ram.load16(item_slot(i)) != id) continue;
        removed += ram.load16(item_slot(i) + 2u);
        ram.store16(item_slot(i), 0u);
        ram.store16(item_slot(i) + 2u, 0u);
    }
    return removed;
}

std::uint32_t fill_materials(Ram &ram, std::uint16_t count) {
    std::set<std::uint16_t> held;
    for (const ItemStack &s : item_box(ram))
        if (s.id != 0u) held.insert(s.id);
    std::uint32_t kinds = 0u;
    for (const Item &item : item_list(ram)) {
        if (item.group != ItemGroup::Material || held.count(item.id) != 0u) continue;
        if (give_item(ram, item.id, count) == 0u) break; // the box is full
        ++kinds;
    }
    return kinds;
}

std::vector<std::optional<Equipment>> equipment_box(const Ram &ram) {
    std::vector<std::optional<Equipment>> box(kEquipmentBoxSlots);
    for (std::uint32_t i = 0; i < kEquipmentBoxSlots; ++i) {
        const std::uint32_t at = equipment_slot(i);
        if (ram.load8(at) == 0u) continue;
        box[i] = Equipment{ram.load8(at + 1u), ram.load16(at + 2u), ram.load16(at + 4u)};
    }
    return box;
}

std::uint32_t free_equipment_slots(const Ram &ram) {
    std::uint32_t free = 0u;
    for (std::uint32_t i = 0; i < kEquipmentBoxSlots; ++i)
        if (ram.load8(equipment_slot(i)) == 0u) ++free;
    return free;
}

std::optional<std::uint32_t> give_equipment(Ram &ram, std::uint8_t kind, std::uint16_t id) {
    if (equipment_kind(kind) == nullptr || id == 0u) return std::nullopt;
    for (std::uint32_t i = 0; i < kEquipmentBoxSlots; ++i) {
        const std::uint32_t at = equipment_slot(i);
        if (ram.load8(at) != 0u) continue;
        for (std::uint32_t b = 4u; b < kEquipmentRecord; b += 2u) ram.store16(at + b, 0u);
        ram.store16(at + 2u, id);
        ram.store8(at + 1u, kind);
        ram.store8(at, 1u);
        return i;
    }
    return std::nullopt;
}

bool on_quest(const Ram &ram) {
    // The overlay header: "MWo3", its load address at +8, its name at +32.
    if (!ram.contains(kTaskSlot, 64u) || ram.load32(kTaskSlot) != 0x336F574Du ||
        ram.load32(kTaskSlot + 8u) != kTaskSlot)
        return false;
    return read_text(ram, kTaskSlot + 32u) == "game_task.ovl";
}

std::vector<Monster> monsters(const Ram &ram) {
    std::vector<Monster> out;
    if (!on_quest(ram)) return out;
    for (std::uint32_t i = 0; i < kMonsterSlots; ++i) {
        const std::uint32_t at = ram.load32(kMonsterTable + i * 4u);
        if (at < 0x08800000u || !ram.contains(at, kMonsterMostHealth + 2u)) continue;
        Monster m{at, ram.load8(at + kMonsterKind), static_cast<std::int16_t>(ram.load16(at + kMonsterHealth)),
            static_cast<std::int16_t>(ram.load16(at + kMonsterMostHealth))};
        if (m.most <= 0) continue; // a companion, or not spawned yet
        out.push_back(m);
    }
    return out;
}

std::string monster_name(const Ram &ram, std::uint8_t kind) {
    if (kind >= kMonsterKinds) return "Monster " + std::to_string(kind);
    const std::string name = text_entry(ram, kTextBlock, kMonsterNameTable, kMonsterNameFirst + kind);
    return !name.empty() ? name : "Monster " + std::to_string(kind);
}

std::uint32_t money(const Ram &ram) {
    return ram.load32(kMoney);
}
void set_money(Ram &ram, std::uint32_t zenny) {
    ram.store32(kMoney, std::min(zenny, kMostMoney));
}

} // namespace p3rd

namespace {
std::uint32_t number(const std::string &text) {
    return static_cast<std::uint32_t>(std::strtoll(text.c_str(), nullptr, 0));
}
} // namespace

bool game_command(Ram &ram, const std::string &command, const std::vector<std::string> &args,
    std::vector<std::string> &out, std::uint32_t observed_hunter) {
    using namespace p3rd;
    const auto arg = [&](std::size_t i, std::uint32_t fallback = 0u) {
        return i < args.size() ? number(args[i]) : fallback;
    };
    if (command == "observe") {
        // Read-only, numeric evidence. Do not confuse the accepted ID with the
        // record the active quest accessor actually resolves (TEXT_TRANSLATION.md).
        const auto overlay = [&](std::uint32_t at) {
            if (!ram.contains(at, 64u) || ram.load32(at) != 0x336F574Du || ram.load32(at + 8u) != at)
                return std::string();
            const std::string name = read_text(ram, at + 32u);
            return name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.") ==
                    std::string::npos
                ? name
                : std::string();
        };
        const bool quest = on_quest(ram);
        const bool loaded = character_loaded(ram);
        const std::uint32_t character =
            ram.contains(quests::kCharacterPointer, 4u) ? ram.load32(quests::kCharacterPointer) : 0u;
        const bool valid_character = character >= 0x08800000u && ram.contains(character, quests::kQuestIdOffset + 2u);
        constexpr std::uint32_t active = 0x08A3A630u;
        const std::uint32_t offset = ram.contains(active, 4u) ? ram.load32(active) : 0u;
        const bool valid_active = quest && offset >= 4u && offset <= 0x10000u && ram.contains(active + offset, 0x48u);
        std::ostringstream json;
        json << "{\"schema\":1,\"request\":" << arg(0) << ",\"loaded\":" << (loaded ? "true" : "false")
             << ",\"village_ready\":" << (loaded && quests::start_blocked(ram).empty() ? "true" : "false")
             << ",\"task\":\"" << overlay(kTaskSlot) << "\",\"map\":\"" << overlay(quests::kMapSlot)
             << "\",\"on_quest\":" << (quest ? "true" : "false") << ",\"accepted_id\":";
        if (valid_character)
            json << ram.load16(character + quests::kQuestIdOffset);
        else
            json << "null";
        json << ",\"active_id\":";
        if (valid_active)
            json << ram.load16(active + offset + 0x1Cu);
        else
            json << "null";
        json << ",\"quest_ticks\":";
        if (quest && ram.contains(kQuestTimeLeft, 4u))
            json << ram.load32(kQuestTimeLeft);
        else
            json << "null";
        json << ",\"active_record_offset\":";
        if (valid_active)
            json << offset;
        else
            json << "null";
        json << ",\"scene\":";
        const auto scene_at = ram.contains(quests::kNextScenePointer, 4u) ? ram.load32(quests::kNextScenePointer) : 0u;
        if (!overlay(kTaskSlot).empty() && scene_at >= 0x08800000u && ram.contains(scene_at, 4u))
            json << ram.load32(scene_at);
        else
            json << "null";
        json << ",\"hunter_position\":";
        const std::uint32_t hunter_object = observed_hunter;
        const std::uint32_t hunter_position = hunter_object + camera::lock_on_layout::kPosition;
        const auto vtable = ram.contains(hunter_object, 4u) ? ram.load32(hunter_object) : 0u;
        const bool own_hunter = loaded && hunter_object >= 0x08800000u && vtable >= 0x08800000u &&
            ram.contains(vtable + game::layered::kFileFunctionSlot, 4u) &&
            ram.load32(vtable + game::layered::kFileFunctionSlot) == game::layered::kRecordFileFunction &&
            ram.contains(hunter_object + game::layered::kObjectIndex, 2u) &&
            ram.load16(hunter_object + game::layered::kObjectIndex) == 0u;
        std::array<float, 3> point{};
        bool finite = own_hunter && ram.contains(hunter_position, 12u);
        for (std::size_t i = 0; finite && i < point.size(); ++i) {
            point[i] = std::bit_cast<float>(ram.load32(hunter_position + static_cast<std::uint32_t>(i) * 4u));
            finite = std::isfinite(point[i]);
        }
        if (finite)
            json << '[' << point[0] << ',' << point[1] << ',' << point[2] << ']';
        else
            json << "null";
        json << ",\"hunter_files\":";
        if (own_hunter && ram.contains(hunter_object + game::layered::kLoadState, 4u) &&
            ram.load32(hunter_object + game::layered::kLoadState) >= game::layered::kLoadDone &&
            ram.contains(hunter_object + 0xB8Cu, 20u)) {
            json << '[';
            for (std::uint32_t i = 0; i < 5u; ++i) {
                if (i) json << ',';
                json << ram.load32(hunter_object + 0xB8Cu + i * 4u);
            }
            json << ']';
        } else {
            json << "null";
        }
        json << ",\"equipment\":\"";
        if (loaded && ram.contains(kCharacter + 0x38u, 84u))
            for (std::uint32_t i = 0; i < 84u; ++i) {
                char byte[3];
                std::snprintf(byte, sizeof(byte), "%02x", ram.load8(kCharacter + 0x38u + i));
                json << byte;
            }
        json << "\",\"hunter_records\":[";
        for (std::uint32_t record = 0; record < game::layered::kHunters; ++record) {
            if (record) json << ',';
            const std::uint32_t at =
                character + game::layered::kHunterRecords + 8u + record * game::layered::kHunterRecordBytes;
            if (!loaded || !valid_character || !ram.contains(at, 84u)) {
                json << "null";
                continue;
            }
            json << '"';
            for (std::uint32_t i = 0; i < 84u; ++i) {
                char byte[3];
                std::snprintf(byte, sizeof(byte), "%02x", ram.load8(at + i));
                json << byte;
            }
            json << '"';
        }
        json << "],\"hunter_armor\":[";
        constexpr std::array<std::uint32_t, 5> armor_offsets = {0x1Cu, 0x26u, 0x30u, 0x3Au, 0x44u};
        for (std::uint32_t record = 0; record < game::layered::kHunters; ++record) {
            if (record) json << ',';
            const std::uint32_t at =
                character + game::layered::kHunterRecords + record * game::layered::kHunterRecordBytes;
            if (!loaded || !valid_character || !ram.contains(at, 0x46u)) {
                json << "null";
                continue;
            }
            json << '[';
            for (std::size_t part = 0; part < armor_offsets.size(); ++part) {
                if (part) json << ',';
                json << ram.load16(at + armor_offsets[part]);
            }
            json << ']';
        }
        json << "],\"monsters\":[";
        bool first = true;
        for (const Monster &m : monsters(ram)) {
            if (!first) json << ',';
            first = false;
            json << "{\"address\":" << m.address << ",\"kind\":" << static_cast<unsigned>(m.kind)
                 << ",\"health\":" << m.health << ",\"maximum\":" << m.most;
            if (ram.contains(m.address, camera::lock_on_layout::kExtent)) {
                json << ",\"same_area\":"
                     << ((ram.load32(m.address + camera::lock_on_layout::kFlags) & camera::lock_on_layout::kOtherArea)
                                ? "false"
                                : "true")
                     << ",\"position\":[";
                for (std::uint32_t i = 0; i < 3u; ++i) {
                    if (i) json << ',';
                    const float coordinate =
                        std::bit_cast<float>(ram.load32(m.address + camera::lock_on_layout::kPosition + i * 4u));
                    if (std::isfinite(coordinate))
                        json << coordinate;
                    else
                        json << "null";
                }
                json << ']';
            }
            json << '}';
        }
        json << ']';
        if (args.size() == 5u && args[1] == "text") {
            const std::uint32_t block = arg(2);
            const auto table = arg(3);
            const auto entry = arg(4);
            const std::string text = table < 128u && entry < 8192u && ram.contains(block, 4u)
                ? text_entry(ram, block, static_cast<int>(table), entry)
                : std::string();
            json << ",\"text_hex\":\"";
            for (unsigned char value : text) {
                char byte[3];
                std::snprintf(byte, sizeof(byte), "%02x", value);
                json << byte;
            }
            json << '"';
        }
        if (args.size() == 4u && args[1] == "field") {
            const std::uint32_t base = arg(2), ref = arg(3);
            std::string bytes;
            bool terminated = false;
            if (base >= 0x08800000u && ref < 0x200000u && ref % 4u == 0u && base + ref >= base &&
                ram.contains(base + ref, 4u)) {
                const std::uint32_t offset = ram.load32(base + ref);
                if (base + offset >= base) {
                    const std::uint32_t start = base + offset;
                    for (std::uint32_t i = 0; i < 8192u && start + i >= start && ram.contains(start + i, 1u); ++i) {
                        const auto byte = ram.load8(start + i);
                        if (byte == 0u) {
                            terminated = true;
                            break;
                        }
                        bytes += static_cast<char>(byte);
                    }
                }
            }
            json << ",\"text_hex\":";
            if (terminated) {
                json << '"';
                for (unsigned char value : bytes) {
                    char byte[3];
                    std::snprintf(byte, sizeof(byte), "%02x", value);
                    json << byte;
                }
                json << '"';
            } else {
                json << "null";
            }
        }
        json << '}';
        out.push_back("observation " + json.str());
    } else if (command == "state") {
        out.push_back("character: " + (character_loaded(ram) ? hunter_name(ram) : std::string("(none)")) + ", zenny " +
            std::to_string(money(ram)) + ", free item slots " + std::to_string(free_item_slots(ram)) +
            ", free equipment slots " + std::to_string(free_equipment_slots(ram)) + ", items named " +
            std::to_string(item_list(ram).size()));
    } else if (command == "money") {
        set_money(ram, arg(0));
        out.push_back("zenny now " + std::to_string(money(ram)));
    } else if (command == "give") {
        const auto id = static_cast<std::uint16_t>(arg(0));
        const std::uint32_t given = give_item(ram, id, arg(1, 1u));
        out.push_back("gave " + std::to_string(given) + " of item " + std::to_string(id) + "; box holds " +
            std::to_string(box_count(ram, id)));
    } else if (command == "remove") {
        const auto id = static_cast<std::uint16_t>(arg(0));
        out.push_back("removed " + std::to_string(remove_item(ram, id)) + " of item " + std::to_string(id));
    } else if (command == "fillmats") {
        out.push_back("added " + std::to_string(fill_materials(ram, static_cast<std::uint16_t>(arg(0, 99u)))) +
            " kinds of material");
    } else if (command == "giveequip") {
        const auto kind = static_cast<std::uint8_t>(arg(0));
        const auto id = static_cast<std::uint16_t>(arg(1));
        const std::optional<std::uint32_t> slot = give_equipment(ram, kind, id);
        const std::vector<std::string> names = equipment_names(ram, kind);
        out.push_back(slot ? "equipment " + std::to_string(kind) + ":" + std::to_string(id) + " (" +
                    (id < names.size() ? names[id] : std::string("?")) + ") in slot " + std::to_string(*slot)
                           : std::string("no equipment given"));
    } else if (command == "item") {
        const auto id = static_cast<std::uint16_t>(arg(0));
        const std::vector<std::string> names = text_table(ram, kTextBlock, kItemNameTable);
        const ItemInfo info = item_info(ram, id);
        out.push_back("item " + std::to_string(id) + " " + (id < names.size() ? names[id] : std::string("?")) + ": " +
            group_name(item_group(info)) + ", rarity " + std::to_string(info.rarity) + ", carry " +
            std::to_string(info.carry) + ", in box " + std::to_string(box_count(ram, id)));
    } else if (command == "table") {
        const std::vector<std::string> t = text_table(ram, kTextBlock, static_cast<int>(arg(0)));
        out.push_back("table " + std::to_string(arg(0)) + ": " + std::to_string(t.size()) + " entries");
        for (std::size_t i = arg(1); i < t.size() && i < arg(1) + arg(2, 8u); ++i)
            out.push_back("  " + std::to_string(i) + " " + t[i]);
    } else if (command == "quest") {
        if (!on_quest(ram)) out.push_back("not on a quest");
        for (const std::string &line : quest_lines(ram)) out.push_back(line);
    } else if (command == "monsterhp") {
        // monsterhp N: every monster's health to N (at least 1).
        const auto hp = static_cast<std::uint16_t>(std::max<std::uint32_t>(arg(0, 1u), 1u));
        std::size_t changed = 0u;
        for (const Monster &m : monsters(ram)) {
            ram.store16(m.address + kMonsterHealth, hp);
            ++changed;
        }
        out.push_back("set " + std::to_string(changed) + " monsters to " + std::to_string(hp) + " health");
    } else {
        return false;
    }
    return true;
}

void held_cheats_frame(Ram &ram, const HeldCheats &cheats) {
    using namespace p3rd;
    static std::optional<std::uint32_t> frozen_time;
    if (!on_quest(ram)) {
        frozen_time.reset();
        return;
    }
    if (cheats.health) {
        const std::uint16_t most = ram.load16(kMostHealth);
        if (most > 0u) {
            ram.store16(kHealth, most);
            ram.store16(kRecoverableHealth, most);
        }
    }
    if (cheats.stamina) {
        const float most = static_cast<float>(ram.load16(kMostStamina));
        std::uint32_t bits = 0u;
        static_assert(sizeof(bits) == sizeof(most));
        std::memcpy(&bits, &most, sizeof(bits));
        if (most > 0.0f) ram.store32(kStamina, bits);
    }
    if (cheats.timer) {
        if (!frozen_time) frozen_time = ram.load32(kQuestTimeLeft);
        ram.store32(kQuestTimeLeft, *frozen_time);
    } else {
        frozen_time.reset();
    }
    if (cheats.one_hit)
        for (const Monster &m : monsters(ram))
            if (m.health > 1) ram.store16(m.address + kMonsterHealth, 1u);
}

std::vector<std::string> quest_lines(const Ram &ram) {
    using namespace p3rd;
    std::vector<std::string> lines;
    if (!on_quest(ram)) return lines;
    const std::uint32_t left = ram.load32(kQuestTimeLeft) / 30u;
    char clock[32];
    std::snprintf(
        clock, sizeof(clock), "%u:%02u left of %u min", left / 60u, left % 60u, ram.load32(kQuestTimeLimit) / 1800u);
    lines.push_back(std::string("Time ") + clock);
    lines.push_back("Hunter health " + std::to_string(static_cast<std::int16_t>(ram.load16(kHealth))) + "/" +
        std::to_string(ram.load16(kMostHealth)));
    for (const Monster &m : monsters(ram))
        lines.push_back(monster_name(ram, m.kind) + " " + std::to_string(m.health) + "/" + std::to_string(m.most));
    return lines;
}

} // namespace mhp3rd::debug
