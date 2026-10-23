#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The mods folder, what each mod in it changes, which ones the player has
// turned on and in what order, and what that adds up to: for each game file,
// the replacement that wins and the patches that apply.
//
// Game-independent: a game says how a mod folder describes itself and how its
// files are numbered (ModFormat); nothing here knows an archive or a format.
namespace mhp3rd::mods {

// A file of the game's own, as the game's mods address it (for this game, an
// entry of DATA.BIN).
using FileId = std::uint32_t;

// Bytes a mod writes at an address, written in its mod.ini rather than kept
// in a file of its own. What the address means (an offset into a file, or a
// guest address) is the game's format's to say, as for a patch file's blocks.
struct Edit {
    std::uint32_t address{};
    std::vector<std::uint8_t> bytes;
    // When not empty, the edit applies only where these bytes are there now,
    // so a mod made for other contents of the file or memory does nothing.
    std::vector<std::uint8_t> expect;
    bool operator==(const Edit &) const = default;
};

// One thing a mod does to one game file.
struct FileChange {
    // Add: a new file, under an id past the disc's. Quest: a new quest, served
    // as a new file under `file`, the id the format gives that quest.
    enum class Kind { Replace, Patch, Add, Quest };
    Kind kind{Kind::Replace};
    FileId file{};
    std::filesystem::path source; // the mod's own file; empty for a patch given as `edits`
    std::vector<Edit> edits;
};

// A file of a mod whose target the player chooses, such as a model that can
// stand in for any one piece of equipment.
struct Slot {
    std::string label; // "Head", "Weapon"
    std::filesystem::path source;
    // What the file stands in for, in the game's own terms ("HEAD", "GS" for
    // this game), so a tool can find the file the player means.
    std::string part;
};

struct Mod {
    std::string id; // the folder's name; the key its choices are saved under
    std::filesystem::path folder;
    std::string name;
    std::string author;
    std::string type;              // as shown: "Files", "Patch", "Pack", ...
    std::string description;       // may hold line breaks
    std::string version;           // what the mod says it was made for, as shown
    std::filesystem::path preview; // an image, when the mod has one
    std::vector<FileChange> changes;
    // Guest code and data the mod writes to memory when the game starts, in
    // the game's own format (for this game, the loader's block files).
    std::vector<std::filesystem::path> code;
    // Bytes it writes to memory at start, after its code files: its raw code
    // images, then its edits.
    std::vector<Edit> memory;
    std::vector<Slot> slots;
    std::vector<std::string> members; // a pack: the mods it turns on and off
    std::vector<std::string> depends; // mods it needs, turned on with it
    int priority{3};                  // the format's load order for code, 0 first; not the player's rank
    // Its native code for the platform Yakumo runs on (docs/MODDING.md):
    // a shared library loaded when the game starts with the mod on.
    std::filesystem::path native;
    // Its script (Luau), run when the game starts with the mod on.
    std::filesystem::path script;
    std::vector<std::string> notes; // parts that do nothing here, and why
    std::string unusable;           // why it cannot be turned on; empty when it can
};

// What a game supplies: how a folder describes a mod, and how files are named.
class ModFormat {
public:
    virtual ~ModFormat() = default;
    // The mod in `folder`, or nothing when the folder is not one.
    [[nodiscard]] virtual std::optional<Mod> read(const std::filesystem::path &folder) const = 0;
    // "0601": how the game's mods spell a file.
    [[nodiscard]] virtual std::string file_name(FileId file) const = 0;
    [[nodiscard]] virtual std::optional<FileId> parse_file(const std::string &text) const = 0;
    // Whether a dependency names something the port does itself, such as the
    // file replacer the community's loader ships as a mod of its own.
    [[nodiscard]] virtual bool built_in(const std::string &dependency) const {
        (void)dependency;
        return false;
    }
};

// Where a native mod keeps its library for the platform Yakumo runs on:
// native/<platform>/<name><extension>, with platform one of windows-x64,
// linux-x64, linux-arm64, macos-arm64, macos-x64 and android-arm64.
[[nodiscard]] const char *native_platform() noexcept;
[[nodiscard]] const char *native_extension() noexcept;

// A setting a mod declares (a native or script mod, at run time): shown on
// its page in the Mods menu, kept between runs in the choices.
struct ModSetting {
    enum class Kind { Switch, Number, Choice };
    std::string key;
    std::string label;
    Kind kind{Kind::Switch};
    std::int64_t minimum{};
    std::int64_t maximum{1};
    std::int64_t fallback{};
    std::vector<std::string> choices; // Kind::Choice: the value is an index
};

// Whether `key` can name a setting: 1 to 64 letters, digits, '_', '.' and
// '-'. Keys are written into the choices file as "setting.<key>=", so
// nothing else may reach it.
[[nodiscard]] bool valid_setting_key(std::string_view key) noexcept;

// The player's choices for one mod.
struct ModChoice {
    bool enabled{};
    int rank{};                               // higher wins where two mods change the same file
    std::vector<std::optional<FileId>> slots; // per Mod::slots: the file each replaces
    std::map<std::string, std::int64_t> settings;
};

// What the enabled mods add up to.
struct Resolution {
    struct Source {
        std::string mod; // Mod::id
        std::filesystem::path path;
        std::vector<Edit> edits; // given in mod.ini instead of a file; then `path` is empty
        bool operator==(const Source &) const = default;
    };
    struct Conflict {
        FileId file{};
        std::string winner;                  // the replacement used; empty when only patches
        std::vector<std::string> overridden; // replacements that lost, highest first
        std::vector<std::string> patched_by; // patches applied to it, in order
    };
    std::map<FileId, Source> replacements;         // the winning replacement per file
    std::map<FileId, std::vector<Source>> patches; // applied in this order: the last wins
    std::map<FileId, Source> added;                // new files, the winning one per id
    std::map<FileId, Source> quests;               // new quests, by the file each is served as
    // Code written at start, by Mod::priority (0 first), then lowest rank
    // first: where two write the same bytes, the last wins.
    std::vector<Source> code;
    // Native libraries, loaded in this order: by priority, then rank.
    std::vector<Source> native;
    // Scripts, started after the native libraries, in the same order.
    std::vector<Source> scripts;
    std::vector<Conflict> conflicts;

    [[nodiscard]] bool empty() const {
        return replacements.empty() && patches.empty() && added.empty() && quests.empty() && code.empty() &&
            native.empty() && scripts.empty();
    }
    // Same files from the same sources; conflicts are not compared.
    [[nodiscard]] bool same_files(const Resolution &other) const {
        return replacements == other.replacements && patches == other.patches && added == other.added &&
            quests == other.quests && code == other.code && native == other.native && scripts == other.scripts;
    }
};

class ModLibrary {
public:
    explicit ModLibrary(const ModFormat &format) : format_(&format) {}

    // Reads every folder in `folder` (not recursively). A folder that is not a
    // mod is skipped. Choices of mods that are gone are kept, not shown.
    void scan(const std::filesystem::path &folder);
    // The saved choices. A missing file means no mod is on.
    void load_choices(const std::filesystem::path &file);
    bool save_choices(const std::filesystem::path &file, std::string &error) const;

    // Highest priority first.
    [[nodiscard]] const std::vector<Mod> &mods() const noexcept { return mods_; }
    [[nodiscard]] const Mod *find(const std::string &id) const;
    [[nodiscard]] ModChoice choice(const std::string &id) const;
    [[nodiscard]] bool enabled(const std::string &id) const { return choice(id).enabled; }

    // On also turns on the mods it depends on; a pack turns its members on
    // and off with it. A mod that cannot be used stays off.
    void set_enabled(const std::string &id, bool on);
    void set_slot(const std::string &id, std::size_t slot, std::optional<FileId> file);

    // Settings a running mod declares; the value saved before, or the
    // fallback, clamped to the setting's range. A key that is not
    // valid_setting_key() is not declared: its fallback comes back.
    std::int64_t declare_setting(const std::string &id, const ModSetting &setting);
    [[nodiscard]] const std::vector<ModSetting> &settings_of(const std::string &id) const;
    // The value saved, or the fallback; as the player or the choices file
    // left it, not clamped.
    [[nodiscard]] std::int64_t setting(const std::string &id, const std::string &key, std::int64_t fallback = 0) const;
    // A setting's value now as the mod sees it: clamped to its range, as
    // declare_setting() gave it; for a key the mod did not declare, setting().
    [[nodiscard]] std::int64_t declared_value(
        const std::string &id, const std::string &key, std::int64_t fallback = 0) const;
    void set_setting(const std::string &id, const std::string &key, std::int64_t value);

    // A change a running mod makes from code (a native mod replacing a file
    // with bytes it made): part of that mod's changes from now on, until
    // Yakumo ends; it is not kept for the next start.
    void add_runtime_change(const std::string &id, const FileChange &change);
    // The change of that kind to `file` add_runtime_change() gave, if any, and
    // taking it back.
    [[nodiscard]] std::optional<FileChange> runtime_change(
        const std::string &id, FileId file, FileChange::Kind kind) const;
    void remove_runtime_change(const std::string &id, FileId file, FileChange::Kind kind);
    // +1 raises the mod's priority by one place, -1 lowers it.
    void move(const std::string &id, int delta);

    // The switch for all mods at once.
    [[nodiscard]] bool master() const noexcept { return master_; }
    void set_master(bool on) noexcept { master_ = on; }

    [[nodiscard]] Resolution resolve() const;
    [[nodiscard]] const ModFormat &format() const noexcept { return *format_; }

private:
    void sort();
    ModChoice &choice_for(const std::string &id);

    const ModFormat *format_;
    std::vector<Mod> mods_;
    std::map<std::string, ModChoice> choices_;
    std::map<std::string, std::vector<ModSetting>> declared_;
    std::map<std::string, std::vector<FileChange>> runtime_changes_;
    bool master_{true};
};

} // namespace mhp3rd::mods
