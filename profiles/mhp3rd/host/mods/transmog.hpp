#pragma once

#include "mods/transmog_store.hpp"

#include <string>
#include <span>
#include <vector>

namespace psprecomp { class Runtime; struct AllegrexContext; }
namespace mhp3rd::game { class Ram; }

namespace mhp3rd::mods::transmog {

struct Armor {
    std::uint16_t id{};
    std::string name;
    std::uint8_t gender{};
};

[[nodiscard]] Store &store();
[[nodiscard]] bool available();
[[nodiscard]] std::string unavailable_reason();
[[nodiscard]] std::string configuration_problem();
[[nodiscard]] std::filesystem::path back_up_invalid_settings_and_reset();
[[nodiscard]] bool preview_active();
[[nodiscard]] const Set &current_selection();
void begin_preview();
void apply_preview();
void cancel_preview();
void save_current_selection(std::string_view name);
// Names and available appearances come from the running supported executable.
[[nodiscard]] std::vector<Armor> armor_list(std::size_t slot);
// Validate the entire resulting set before persisting any gameplay-facing
// appearance change. Reset remains available for an incompatible saved set.
void set_appearance(std::size_t slot, std::optional<ArmorAppearance> armor);
void set_weapon_appearance(std::optional<WeaponAppearance> weapon);
[[nodiscard]] bool enabled();
// Native visual lookups use one immutable set throughout an asynchronous rebuild.
[[nodiscard]] const Set &visual_selection();
[[nodiscard]] std::size_t equipment_count(std::uint8_t kind);
[[nodiscard]] std::vector<std::string> equipment_names(std::uint8_t kind);
void load_set(std::string_view name);
// Stages defaults during an active preview; otherwise persists defaults immediately.
void reset_appearances();
// Called after a display import finishes, only when its execution-context
// token still matches. The native frame is suspended until visual loading ends.
void frame(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx);
[[nodiscard]] std::string refresh_status();
void ge_list_started(std::uint32_t id, std::uint32_t start, std::int32_t thread);
void ge_list_finished(std::uint32_t id, bool done);
void ge_finish_queued(std::uint32_t id);
void ge_finish_returned(std::uint32_t id);

struct CodeWord { std::uint32_t address; std::uint32_t word; };
[[nodiscard]] std::span<const CodeWord> helper_signature();
// Return addresses, rather than call instruction addresses.
[[nodiscard]] std::span<const std::uint32_t> callers();
[[nodiscard]] std::uint16_t converter_adjustment(std::uint16_t donor_base,
    std::uint16_t converted_model, std::uint16_t caller_base);
// Pure appearance lookup: no equipment, table or save writes. Counts are the
// game's loaded name-table lengths, in kSlots order.
[[nodiscard]] std::optional<std::uint16_t> select_appearance(
    const game::Ram &ram, const Set &selection, const std::array<std::size_t, 5> &counts,
    std::uint32_t kind, std::uint32_t gender);

using AppearanceFunction = void (*)(psprecomp::Runtime &, psprecomp::AllegrexContext &);
struct AppearanceHooks { AppearanceFunction model{}; AppearanceFunction converter{}; };
void install(psprecomp::Runtime &runtime, AppearanceHooks originals, std::string_view executable_sha256);

} // namespace mhp3rd::mods::transmog
