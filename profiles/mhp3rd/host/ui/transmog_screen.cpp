#include "ui/transmog_screen.hpp"

#include "mods/mod_ini.hpp"
#include "mods/transmog.hpp"
#include "mods/transmog_weapon.hpp"
#include "game/guest_ram.hpp"
#include "game/layered_armor.hpp"
#include "ui/text_input.hpp"
#include "ui/widgets.hpp"
#include "settings/settings.hpp"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace mhp3rd::ui {
namespace {
namespace transmog = mods::transmog;
// Transmog stores Head, Chest, Arms, Waist, Legs; the ownership helper uses
// the game's Chest, Arms, Waist, Legs, Head part order.
constexpr std::array<std::uint8_t, transmog::kSlots.size()> kGamePartForTransmogSlot{4u, 0u, 1u, 2u, 3u};
enum class Page { Main, Armor, Weapon, Preset };
struct State {
    Page page{Page::Main};
    std::size_t slot{};
    std::string search;
    std::string preset;
    std::string message;
    bool message_error{};
    double message_expires_at{};
    bool focus{true};
    bool open{};
    bool close_request{};
    std::optional<std::string> save_name;
};
State &state() { static State value; return value; }
void open(Page page) {
    state().page = page;
    state().focus = true;
    ImGui::SetScrollY(0.0f);
}
void focus() {
    if (std::exchange(state().focus, false)) focus_next_row();
}
std::string appearance_name(std::size_t slot, const transmog::Set &set) {
    if (!set.armor[slot]) return "Default (equipped armor)";
    for (const auto &armor : transmog::armor_list(slot))
        if (armor.id == set.armor[slot]->id && armor.gender == set.armor[slot]->gender)
            return armor.name + (armor.gender == 0 ? " (male)" : " (female)");
    return "Armor #" + std::to_string(set.armor[slot]->id) + (set.armor[slot]->gender == 0 ? " (male)" : " (female)");
}
void applied(const std::string &message) {
    (void)message;
    state().message.clear();
    state().message_error = false;
    state().message_expires_at = 0.0;
}
void confirmed(const char *message) {
    state().message = message;
    state().message_error = false;
    state().message_expires_at = ImGui::GetTime() + 3.0;
}
std::string weapon_name(const transmog::Set &set) {
    if (!set.weapon) return "Default (equipped weapon)";
    for (const auto &piece : transmog::weapon::weapon_list(set.weapon->kind))
        if (piece.id == set.weapon->id) return piece.name;
    return "Weapon #" + std::to_string(set.weapon->id);
}
std::set<std::uint16_t> owned_armor_ids(std::size_t slot) {
    std::set<std::uint16_t> ids;
    (void)game::read([&](const game::Ram &ram) {
        for (const auto &offer : game::layered::offers(ram, kGamePartForTransmogSlot[slot], true))
            if (offer.owned) ids.insert(offer.id);
    });
    return ids;
}
const char *weapon_class_name(std::uint8_t kind) {
    for (const auto &weapon_class : transmog::kWeaponClasses)
        if (weapon_class.kind == kind) return weapon_class.label;
    return nullptr;
}
void main_page(bool menu_paused) {
    auto &s = state();
    if (!s.message_error && !s.message.empty() && ImGui::GetTime() >= s.message_expires_at) {
        s.message.clear();
        s.message_expires_at = 0.0;
    }
    auto &store = transmog::store();
    auto selection = transmog::current_selection();
    const auto equipped_weapon_kind = transmog::weapon::equipped_weapon_kind();
    const bool initialized = !store.path().empty();
    const std::string configuration_problem = transmog::configuration_problem();
    section("Layered Sets");
    auto &player_settings = settings::current();
    if (toggle_row("List all armor", player_settings.layered_all,
                   {false, {}, "Off, list armor you own or wear. On, list all armor appearances."})) {
        player_settings.layered_all = !player_settings.layered_all;
        settings::save();
        applied({});
    }
    paragraph("Off: armor you own or wear. On: all armor appearances.", colors::kTextDim);
    if (!configuration_problem.empty()) {
        paragraph(configuration_problem, colors::kDanger);
        if (button_row("Back up invalid settings and restore defaults", {false, {}, "Preserve the unreadable settings file under a new backup name, then create a fresh default file."})) {
            const auto backup = transmog::back_up_invalid_settings_and_reset();
            applied(backup.empty() ? "Invalid settings were preserved by Yakumo; default appearances are ready."
                                   : "Invalid settings backed up to " + backup.string() + "; default appearances are ready.");
        }
    }
    focus();
    const bool can_edit = !menu_paused && transmog::available() && transmog::preview_active();
    if (button_row("Reset all to default", {!initialized || !can_edit || !configuration_problem.empty(), {},
            "Stage default armor and weapon appearances. Apply saves them; Cancel restores the saved appearances."}, colors::kAccentBright)) {
        transmog::reset_appearances();
        applied({});
    }
    if (button_row("Apply this set", {!can_edit, {}, "Save the previewed armor and weapon appearances."}, colors::kAccentBright)) {
        transmog::apply_preview();
        transmog::begin_preview();
        selection = transmog::current_selection();
        confirmed("Set applied.");
    }
    if (button_row("Cancel preview", {!transmog::preview_active(), {}, "Restore the saved armor and weapon appearances."}, colors::kDanger)) {
        transmog::cancel_preview();
        if (transmog::available()) transmog::begin_preview();
        selection = transmog::current_selection();
        applied({});
    }
    if (!s.message_error && !s.message.empty()) paragraph(s.message, colors::kAccentBright);
    if (value_row("Weapon",
                  equipped_weapon_kind && selection.weapon && selection.weapon->kind != *equipped_weapon_kind
                      ? "Default (equipped weapon)" : weapon_name(selection),
                  {!transmog::available() || equipped_weapon_kind == std::nullopt, {},
                   "Choose an appearance for the equipped weapon."})) {
        s.search.clear();
        open(Page::Weapon);
    }
    const auto weapon_status = transmog::weapon::status();
    if (!weapon_status.empty()) paragraph(weapon_status, colors::kDanger);
    if (!transmog::available()) paragraph(transmog::unavailable_reason(), colors::kDanger);
    for (std::size_t slot = 0; slot < transmog::kSlots.size(); ++slot) {
        if (value_row(transmog::kSlots[slot], appearance_name(slot, selection),
                      {!transmog::available(), {}, "Choose an appearance for this armor slot."})) {
            s.slot = slot;
            s.search.clear();
            open(Page::Armor);
        }
    }
    section("Saved Layered Sets");
    if (button_row("Save current set…", {!initialized || !configuration_problem.empty() || !can_edit, {}, "Save the previewed armor and weapon appearance choices under a name after a hunter and its appearance catalogs have loaded. An existing name replaces that saved set."})) {
        TextInputRequest request;
        request.title = "Save transmog set";
        request.prompt = "Set name (an existing name replaces that set)";
        request.max_length = 48u;
        request.allowed = &transmog::Store::name_character;
        open_text_input(std::move(request), [](std::optional<std::string> name) {
            if (name) state().save_name = std::move(*name);
        });
    }
    if (store.presets().empty()) paragraph("No saved sets yet.");
    for (const auto &[name, set] : store.presets()) {
        ImGui::PushID(name.c_str());
        if (value_row(name.c_str(), set == transmog::current_selection() ? "Previewed choices" : "Load / delete",
                      {false, {}, "Open this saved set to load or delete it."})) {
            applied({});
            s.preset = name;
            open(Page::Preset);
        }
        ImGui::PopID();
    }
}

void armor_page() {
    auto &s = state();
    section((std::string(transmog::kSlots[s.slot]) + " appearance").c_str());
    focus();
    if (!transmog::available()) {
        paragraph(transmog::unavailable_reason(), colors::kDanger);
        return;
    }
    if (button_row("Default (equipped armor)", {false, {}, "Remove the appearance override for this slot."})) {
        transmog::set_appearance(s.slot, std::nullopt);
        applied(std::string(transmog::kSlots[s.slot]) + " restored to default.");
        open(Page::Main);
        return;
    }
    if (value_row("Search", s.search.empty() ? "All appearances" : s.search)) {
        TextInputRequest request;
        request.title = "Search armor appearances";
        request.initial = s.search;
        request.max_length = 48u;
        open_text_input(std::move(request), [](std::optional<std::string> search) {
            if (search) { state().search = std::move(*search); state().focus = true; }
        });
    }
    const auto armor = transmog::armor_list(s.slot);
    const auto owned = settings::current().layered_all ? std::set<std::uint16_t>{} : owned_armor_ids(s.slot);
    const std::string filter = mods::lower(s.search);
    unsigned shown{};
    for (const auto &piece : armor) {
        if (!settings::current().layered_all && !owned.contains(piece.id)) continue;
        if (!filter.empty() && mods::lower(piece.name).find(filter) == std::string::npos) continue;
        ++shown;
        const transmog::ArmorAppearance choice{piece.id, piece.gender};
        const bool selected = transmog::current_selection().armor[s.slot] == choice;
        ImGui::PushID(static_cast<int>(piece.id) * 2 + piece.gender);
        const auto name = piece.name + (piece.gender == 0 ? " (male)" : " (female)");
        if (list_row("##armor", name, selected ? "Selected" : "", ListIcon::None, selected)) {
            ImGui::PopID();
            transmog::set_appearance(s.slot, choice);
            applied(std::string(transmog::kSlots[s.slot]) + " preview: " + piece.name + ". Apply to save this set.");
            open(Page::Main);
            return;
        }
        ImGui::PopID();
    }
    if (shown == 0u)
        paragraph(settings::current().layered_all ? "No matching armor appearances."
                                                   : "No matching owned appearances. Turn on List all armor to see every donor.");
}

void weapon_page() {
    auto &s = state();
    section("Weapon appearance");
    focus();
    if (!transmog::available()) {
        paragraph(transmog::unavailable_reason(), colors::kDanger);
        return;
    }
    const auto equipped_kind = transmog::weapon::equipped_weapon_kind();
    if (button_row("Default (equipped weapon)")) {
        transmog::set_weapon_appearance(std::nullopt);
        applied("Weapon restored to default. Armor appearances are unchanged.");
        open(Page::Main);
        return;
    }
    if (!equipped_kind) {
        paragraph("Equip a supported weapon to choose its appearance.", colors::kTextDim);
        return;
    }
    const auto *class_name = weapon_class_name(*equipped_kind);
    if (class_name == nullptr) {
        paragraph("This weapon cannot use a layered appearance. Equip a supported weapon to continue.", colors::kDanger);
        return;
    }
    if (value_row("Search", s.search.empty() ? "All appearances" : s.search)) {
        TextInputRequest request;
        request.title = "Search weapon appearances";
        request.initial = s.search;
        request.max_length = 48u;
        open_text_input(std::move(request), [](std::optional<std::string> search) {
            if (search) { state().search = std::move(*search); state().focus = true; }
        });
    }
    const auto filter = mods::lower(s.search);
    unsigned shown{};
    for (const auto &piece : transmog::weapon::weapon_list(*equipped_kind)) {
        if (!filter.empty() && mods::lower(piece.name).find(filter) == std::string::npos) continue;
        ++shown;
        const transmog::WeaponAppearance choice{piece.id, *equipped_kind};
        const bool selected = transmog::current_selection().weapon == choice;
        ImGui::PushID(piece.id);
        if (list_row("##weapon", piece.name, selected ? "Selected" : "", ListIcon::None, selected)) {
            ImGui::PopID();
            transmog::set_weapon_appearance(choice);
            applied("Weapon preview: " + piece.name + ". Apply to save this set; armor appearances are unchanged.");
            open(Page::Main);
            return;
        }
        ImGui::PopID();
    }
    if (shown == 0u) paragraph("No matching weapon appearances.");
}

void preset_page() {
    auto &s = state();
    auto &store = transmog::store();
    section(s.preset.c_str());
    const auto found = store.presets().find(s.preset);
    if (found == store.presets().end()) {
        open(Page::Main);
        return;
    }
    focus();
    if (button_row("Load this set", {!transmog::available(), {}, "Replace active armor and weapon appearance choices with this set."}, colors::kAccentBright)) {
        transmog::load_set(s.preset);
        confirmed("Saved set applied.");
        open(Page::Main);
        return;
    }
    for (std::size_t slot = 0; slot < transmog::kSlots.size(); ++slot)
        info_row(transmog::kSlots[slot], appearance_name(slot, found->second));
    info_row("Weapon", weapon_name(found->second));
    if (button_row("Delete this saved set", {false, {}, "Delete only this preset. Your active appearances stay as they are."}, colors::kDanger)) {
        store.delete_preset(s.preset);
        applied("Deleted saved set " + s.preset + ". Active appearances are unchanged.");
        open(Page::Main);
    }
}
}

bool transmog_page(bool back, bool menu_paused) {
    auto &s = state();
    s.open = true;
    if (!settings::current().layered_armor && transmog::preview_active()) transmog::cancel_preview();
    if (back) {
        if (s.page == Page::Main) {
            if (transmog::preview_active()) {
                transmog::cancel_preview();
                if (!menu_paused) s.close_request = true;
            }
            s.open = false;
            s.focus = true;
            return false;
        }
        open(Page::Main);
    }
    try {
        if (s.save_name) {
            const auto name = std::exchange(s.save_name, std::nullopt);
            (void)transmog::store();  // Finish any pending legacy import before writing the first preset.
            if (!transmog::available())
                throw std::runtime_error("Cannot save a set until Layered armor is enabled and a hunter with loaded appearance catalogs is available: " +
                                         transmog::unavailable_reason());
            transmog::save_current_selection(*name);
            applied("Saved set " + *name + ".");
        }
        if (!transmog::preview_active() && settings::current().layered_armor &&
            transmog::configuration_problem().empty())
            transmog::begin_preview();
        switch (s.page) {
        case Page::Main: main_page(menu_paused); break;
        case Page::Armor: armor_page(); break;
        case Page::Weapon: weapon_page(); break;
        case Page::Preset: preset_page(); break;
        }
    } catch (const std::exception &error) {
        // Persistence failure is a recoverable UI error: no successful commit
        // was made, and the player can fix the path or retry the operation.
        s.message = error.what();
        s.message_error = true;
        s.message_expires_at = 0.0;
        std::cerr << "[transmog] " << s.message << '\n';
    }
    if (!settings::current().layered_armor && transmog::preview_active()) transmog::cancel_preview();
    if (!s.message.empty() && s.message_error) paragraph(s.message, colors::kDanger);
    if (s.page != Page::Main && button_row("Back", {false, {}, "Back to Layered Sets."})) open(Page::Main);
    return true;
}

void reset_transmog_page() {
    State &s = state();
    s.page = Page::Main;
    s.search.clear();
    s.preset.clear();
    s.message.clear();
    s.message_error = false;
    s.message_expires_at = 0.0;
    s.save_name.reset();
    s.close_request = false;
    s.open = false;
    s.focus = true;
}

bool transmog_page_open() { return state().open; }
bool transmog_subpage_open() { return state().page != Page::Main; }

bool take_transmog_close_request() {
    return std::exchange(state().close_request, false);
}
} // namespace mhp3rd::ui
