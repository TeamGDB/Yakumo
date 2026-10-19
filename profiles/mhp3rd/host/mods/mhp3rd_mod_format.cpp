#include "mods/mhp3rd_mod_format.hpp"

#include "mods/mod_ini.hpp"

#include "game/quest_file.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace mhp3rd::mods::p3rd {
namespace {

namespace fs = std::filesystem;

constexpr const char *kInfo = "MOD INFO";
// The largest raw code image a mod may load: far more than any code needs,
// and small enough to read whole.
constexpr std::uintmax_t kMaxImage = 8u * 1024u * 1024u;

// Guest RAM on the PSP; addresses given through a cached or uncached mirror
// are folded onto it first.
constexpr std::uint32_t kRamStart = 0x08000000u;
constexpr std::uint32_t kRamEnd = 0x0C000000u;
constexpr std::uint32_t kMirrorMask = 0x3FFFFFFFu;

// Equipment types from the manager's documentation, and how they are shown.
struct EquipType {
    const char *key;
    const char *label;
};
constexpr EquipType kEquipTypes[] = {
    {"GS", "Great Sword"},
    {"LS", "Long Sword"},
    {"SNS", "Sword and Shield"},
    {"DB", "Dual Blades"},
    {"LNC", "Lance"},
    {"GL", "Gunlance"},
    {"HMR", "Hammer"},
    {"HH", "Hunting Horn"},
    {"LBG", "Light Bowgun"},
    {"HBG", "Heavy Bowgun"},
    {"BOW", "Bow"},
    {"SAXE", "Switch Axe"},
    {"HEAD", "Head armour"},
    {"ARMS", "Arm armour"},
    {"BODY", "Body armour"},
    {"WAIST", "Waist armour"},
    {"LEGS", "Leg armour"},
    {"CATHELM", "Felyne helm"},
    {"CATPLATE", "Felyne plate"},
    {"CATWPN", "Felyne weapon"},
};

std::string hex4(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%04X", value);
    return text;
}

std::string hex8(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

// Whether `path` is inside `folder` once links and dots are resolved.
bool inside(const fs::path &folder, const fs::path &path) {
    std::error_code ec;
    const fs::path base = fs::weakly_canonical(folder, ec);
    if (ec) return false;
    const fs::path resolved = fs::weakly_canonical(path, ec);
    if (ec) return false;
    const fs::path relative = resolved.lexically_relative(base);
    return !relative.empty() && *relative.begin() != "..";
}

// A file of the mod, found without regard to case (mods are made on Windows
// and played on Linux too). `relative` may use either slash. Nothing outside
// the mod's folder: no "." or ".." parts, drives, roots or links out.
std::optional<fs::path> find_file(const fs::path &folder, const std::string &relative) {
    fs::path at = folder;
    std::string rest = relative;
    std::replace(rest.begin(), rest.end(), '\\', '/');
    if (rest.empty() || rest.front() == '/') return std::nullopt;
    for (const std::string &part : split_list(rest, '/')) {
        if (part.empty() || part == "." || part == ".." || part.find(':') != std::string::npos) return std::nullopt;
        std::error_code ec;
        if (fs::exists(at / from_utf8(part), ec)) {
            at /= from_utf8(part);
            continue;
        }
        bool found = false;
        for (fs::directory_iterator it(at, ec), end; !ec && it != end; it.increment(ec)) {
            if (iequals(to_utf8(it->path().filename()), part)) {
                at = it->path();
                found = true;
                break;
            }
        }
        if (!found) return std::nullopt;
    }
    std::error_code ec;
    if (!fs::is_regular_file(at, ec) || !inside(folder, at)) return std::nullopt;
    return at;
}

std::uint32_t load32(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) | static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u |
        static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u | static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u;
}

// "0601" -> 0x601; also without leading zeros, and with 0x.
std::optional<FileId> parse_hex(std::string text) {
    text = trim(text);
    if (text.size() > 2u && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text = text.substr(2);
    if (text.empty() || text.size() > 6u) return std::nullopt;
    FileId value{};
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (ec != std::errc{} || ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

// A file named like mhp3reload's files folder has them: 0601 or 0601P.
struct IdFile {
    FileId file{};
    bool patch{};
};
std::optional<IdFile> id_file_name(const std::string &name) {
    std::string stem = name;
    bool patch = false;
    if (stem.size() == 5u && (stem.back() == 'P' || stem.back() == 'p')) {
        patch = true;
        stem.pop_back();
    }
    if (stem.size() != 4u || !std::all_of(stem.begin(), stem.end(), [](unsigned char c) { return std::isxdigit(c); }))
        return std::nullopt;
    const auto file = parse_hex(stem);
    if (!file) return std::nullopt;
    return IdFile{*file, patch};
}

class Reader {
public:
    Reader(const ModFolderFormat &format, std::uint32_t entries, const fs::path &folder, Mod &mod)
        : format_(format), entries_(entries), folder_(folder), mod_(mod) {}

    // Files and targets as the Version key selects them.
    void file_changes(
        const IniFile &ini, const std::string &section, const std::string &version, FileChange::Kind kind) {
        std::string files = ini.get(section, "Files");
        std::string targets = ini.get(section, "Target");
        if (version == "BOTH") {
            if (const std::string *hd = ini.find(section, "FilesHD")) files = *hd;
            if (const std::string *hd = ini.find(section, "TargetHD")) targets = *hd;
        } else if (version != "HD") {
            unusable("Made for the PSP version" +
                std::string(version.empty() ? " (it gives no Version, which means PSP)" : "") +
                ": its file ids are not the HD version's.");
            return;
        }
        const std::vector<std::string> names = split_list(files);
        const std::vector<std::string> ids = split_list(targets);
        if (names.empty()) {
            unusable("Its mod.ini lists no files.");
            return;
        }
        if (names.size() != ids.size()) {
            unusable("Its mod.ini lists " + std::to_string(names.size()) + " files for " + std::to_string(ids.size()) +
                " targets.");
            return;
        }
        for (std::size_t i = 0; i < names.size(); ++i) {
            const std::optional<FileId> file = format_.parse_file(ids[i]);
            if (!file) {
                // "1508Q": a file id with a letter after it names a file that
                // another code mod adds, not one of the game's.
                const bool added = ids[i].size() > 4u && format_.parse_file(ids[i].substr(0, ids[i].size() - 1u));
                unusable("Target \"" + ids[i] + "\" is not a file of the game" +
                    (added ? std::string(": it is one that another mod adds") +
                                (mod_.depends.empty() ? "" : " (it needs " + mod_.depends.front() + ")") +
                                ", which Yakumo cannot load"
                           : "") +
                    ".");
                return;
            }
            if (entries_ != 0u && *file >= entries_) {
                unusable("Target " + format_.file_name(*file) + " is not a file of this game.");
                return;
            }
            const std::optional<fs::path> source = find_file(folder_, names[i]);
            if (!source) {
                unusable("The file \"" + names[i] + "\" is missing from the folder.");
                return;
            }
            mod_.changes.push_back({kind, *file, *source});
        }
    }

    // A code mod's files, as the Version key selects them; no targets.
    void code_files(const IniFile &ini, const std::string &section, const std::string &version) {
        std::string files = ini.get(section, "Files");
        if (version == "BOTH") {
            if (const std::string *hd = ini.find(section, "FilesHD")) files = *hd;
        } else if (version != "HD") {
            unusable("Made for the PSP version" +
                std::string(version.empty() ? " (it gives no Version, which means PSP)" : "") +
                ": its code is for the PSP version's memory.");
            return;
        }
        const std::vector<std::string> names = split_list(files);
        if (names.empty()) {
            unusable("Its mod.ini lists no files.");
            return;
        }
        for (const std::string &name : names) {
            const std::optional<fs::path> source = find_file(folder_, name);
            if (!source) {
                unusable("The file \"" + name + "\" is missing from the folder.");
                return;
            }
            mod_.code.push_back(*source);
        }
    }

    // Type="Yakumo": sections instead of parallel lists, and edits written in
    // the mod.ini itself (docs/MODDING.md).
    void yakumo(const IniFile &ini, const std::string &version) {
        if (!version.empty() && version != "HD" && version != "BOTH") {
            unusable("Made for the PSP version: its file ids and addresses are not the HD version's.");
            return;
        }
        if (const std::string format = trim(ini.get(kInfo, "Format", "1")); format != "1") {
            unusable("Made for a newer Yakumo: it is in mod format " + format + ", and this Yakumo reads format 1.");
            return;
        }
        for (const auto &[id, name] : ini.entries("Files")) {
            const std::optional<FileId> file = target(id);
            const std::optional<fs::path> source = file ? source_file(name) : std::nullopt;
            if (!source) return;
            mod_.changes.push_back({FileChange::Kind::Replace, *file, *source, {}});
        }
        // New files, under ids past the disc's last.
        for (const auto &[id, name] : ini.entries("Add")) {
            const std::optional<FileId> file = format_.parse_file(id);
            if (!file || *file >= 0xFFFFu || (entries_ != 0u && *file < entries_)) {
                unusable("[Add] " + trim(id) + ": a new file needs an id from " + format_.file_name(entries_) +
                    " to FFFE; the ids below are the game's own files.");
                return;
            }
            if (quest_of_file(*file)) {
                unusable("[Add] " + trim(id) + ": ids from " + format_.file_name(kFirstQuestFile) + " to " +
                    format_.file_name(kFirstQuestFile + kQuestFiles - 1u) + " are kept for the quests mods add.");
                return;
            }
            const std::optional<fs::path> source = source_file(name);
            if (!source) return;
            mod_.changes.push_back({FileChange::Kind::Add, *file, *source, {}});
        }
        // New quests: `62001 = quest.bin`, the quest as the game holds it once
        // decrypted, its record carrying the same id.
        for (const auto &[id, name] : ini.entries("Quests")) {
            std::uint32_t quest = 0u;
            const std::string text = trim(id);
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), quest);
            if (error != std::errc{} || end != text.data() + text.size() || !game::is_mod_quest(quest)) {
                unusable("[Quests] " + text + ": a new quest needs an id from 62000 to 65535.");
                return;
            }
            const std::optional<fs::path> source = source_file(name);
            if (!source) return;
            std::ifstream in(*source, std::ios::binary);
            const std::vector<std::uint8_t> payload{
                std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            const std::optional<game::QuestRecord> record =
                payload.size() <= game::kQuestMaxPayload ? game::read_quest_record(payload) : std::nullopt;
            if (!record) {
                unusable("[Quests] " + text + ": \"" + trim(name) + "\" is not a quest the game can read.");
                return;
            }
            if (record->id != quest) {
                unusable("[Quests] " + text + ": \"" + trim(name) + "\" is quest " + std::to_string(record->id) +
                    "; the id in mod.ini and the quest's own must be the same.");
                return;
            }
            mod_.changes.push_back({FileChange::Kind::Quest, *quest_file(quest), *source, {}});
        }
        for (const auto &[id, name] : ini.entries("Patches")) {
            const std::optional<FileId> file = target(id);
            const std::optional<fs::path> source = file ? source_file(name) : std::nullopt;
            if (!source) return;
            mod_.changes.push_back({FileChange::Kind::Patch, *file, *source, {}});
        }
        for (const std::string &section : ini.sections()) {
            if (section.size() <= 6u || !iequals(std::string_view(section).substr(0, 6), "Patch ")) continue;
            const std::optional<FileId> file = target(section.substr(6));
            if (!file) return;
            FileChange change{FileChange::Kind::Patch, *file, {}, {}};
            if (!edits(ini, section, change.edits)) return;
            if (!change.edits.empty()) mod_.changes.push_back(std::move(change));
        }
        for (const std::string &name : split_list(ini.get("Code", "Files"))) {
            const std::optional<fs::path> source = source_file(name);
            if (!source) return;
            mod_.code.push_back(*source);
        }
        // `address = file`: a raw image, as an assembler writes it, loaded at
        // that address. Read now: written at start with the [Memory] edits.
        for (const auto &[key, name] : ini.entries("Code")) {
            if (iequals(trim(key), "Files")) continue;
            std::string error;
            std::optional<Edit> at = parse_edit(key, "00", error);
            if (!at) {
                unusable("[Code] " + trim(key) + ": " + error + ".");
                return;
            }
            const std::optional<fs::path> source = source_file(name);
            if (!source) return;
            std::error_code ec;
            const std::uintmax_t size = fs::file_size(*source, ec);
            if (ec || size == 0u || size > kMaxImage) {
                unusable("[Code] " + trim(key) + ": \"" + trim(name) + "\" is empty, unreadable or larger than 8 MiB.");
                return;
            }
            std::ifstream in(*source, std::ios::binary);
            at->bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            mod_.memory.push_back(std::move(*at));
        }
        if (!edits(ini, "Memory", mod_.memory)) return;
        // Native code: native/<platform>/<Library><extension>.
        if (const std::string library = trim(ini.get("Native", "Library")); !library.empty()) {
            const std::string relative =
                std::string("native/") + native_platform() + "/" + library + native_extension();
            std::optional<fs::path> found = find_file(folder_, relative);
            if (!found) {
                unusable("It has no native code for this platform (" + std::string(native_platform()) + "): it needs " +
                    relative + ".");
                return;
            }
            mod_.native = *found;
            mod_.notes.push_back("Runs native code on your machine. Use native mods only from people you trust.");
        }
        // A script: [Script] File = main.luau.
        if (const std::string script = trim(ini.get("Script", "File")); !script.empty()) {
            const std::optional<fs::path> source = source_file(script);
            if (!source) return;
            mod_.script = *source;
        }
        if (mod_.changes.empty() && mod_.code.empty() && mod_.memory.empty() && mod_.native.empty() &&
            mod_.script.empty())
            unusable("It changes nothing: it has no [Files], [Add], [Quests], [Patches], [Patch <file>], [Code], "
                     "[Memory], [Native] or [Script] lines.");
    }

    // `parts`: the manager's key for what each file stands in for (HEAD, GS).
    void slots(
        const std::string &files, const std::vector<std::string> &labels, const std::vector<std::string> &parts) {
        const std::vector<std::string> names = split_list(files);
        if (names.size() != labels.size()) {
            unusable("Its mod.ini lists " + std::to_string(names.size()) + " files; this type takes " +
                std::to_string(labels.size()) + ".");
            return;
        }
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (iequals(names[i], "null")) continue;
            const std::optional<fs::path> source = find_file(folder_, names[i]);
            if (!source) {
                unusable("The file \"" + names[i] + "\" is missing from the folder.");
                return;
            }
            mod_.slots.push_back({labels[i], *source, parts[i]});
        }
    }

    void unusable(std::string why) {
        if (mod_.unusable.empty()) mod_.unusable = std::move(why);
    }

private:
    std::optional<FileId> target(const std::string &text) {
        const std::optional<FileId> file = format_.parse_file(text);
        if (!file)
            unusable("\"" + trim(text) + "\" is not a file id.");
        else if (entries_ != 0u && *file >= entries_)
            unusable("Target " + format_.file_name(*file) + " is not a file of this game.");
        else
            return file;
        return std::nullopt;
    }

    std::optional<fs::path> source_file(const std::string &name) {
        std::optional<fs::path> source = find_file(folder_, trim(name));
        if (!source) unusable("The file \"" + trim(name) + "\" is missing from the folder.");
        return source;
    }

    bool edits(const IniFile &ini, const std::string &section, std::vector<Edit> &out) {
        for (const auto &[key, value] : ini.entries(section)) {
            std::string error;
            std::optional<Edit> edit = parse_edit(key, value, error);
            if (!edit) {
                unusable("[" + section + "] " + trim(key) + ": " + error + ".");
                return false;
            }
            out.push_back(std::move(*edit));
        }
        return true;
    }

private:
    const ModFolderFormat &format_;
    std::uint32_t entries_;
    const fs::path &folder_;
    Mod &mod_;
};

std::string version_label(const std::string &version) {
    if (version == "HD") return "HD version";
    if (version == "BOTH") return "PSP and HD versions";
    return "PSP version";
}

// A folder without a mod.ini, holding files named by file id.
std::optional<Mod> read_id_files(const ModFolderFormat &format, std::uint32_t entries, const fs::path &folder) {
    Mod mod;
    for (const fs::path &where : {folder, folder / "files"}) {
        std::error_code ec;
        for (fs::directory_iterator it(where, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const auto id = id_file_name(to_utf8(it->path().filename()));
            if (!id || (entries != 0u && id->file >= entries)) continue;
            mod.changes.push_back(
                {id->patch ? FileChange::Kind::Patch : FileChange::Kind::Replace, id->file, it->path()});
        }
    }
    if (mod.changes.empty()) return std::nullopt;
    std::sort(mod.changes.begin(), mod.changes.end(),
        [](const FileChange &a, const FileChange &b) { return a.file < b.file; });
    mod.name = to_utf8(folder.filename());
    const bool patches = std::any_of(
        mod.changes.begin(), mod.changes.end(), [](const FileChange &c) { return c.kind == FileChange::Kind::Patch; });
    const bool files = std::any_of(mod.changes.begin(), mod.changes.end(),
        [](const FileChange &c) { return c.kind == FileChange::Kind::Replace; });
    mod.type = files && patches ? "Files and patches" : patches ? "Patch" : "Files";
    mod.version = "HD version (assumed)";
    mod.description = "No mod.ini: its files are named by the file id they replace (" +
        format.file_name(mod.changes.front().file) +
        "), as in mhp3reload's files folder. Taken to be made for the HD version.";
    return mod;
}

} // namespace

std::string ModFolderFormat::file_name(FileId file) const {
    return hex4(file);
}

std::optional<FileId> ModFolderFormat::parse_file(const std::string &text) const {
    return parse_hex(text);
}

bool ModFolderFormat::built_in(const std::string &dependency) const {
    return iequals(trim(dependency), "file_replacer");
}

std::optional<Mod> ModFolderFormat::read(const fs::path &folder) const {
    const std::optional<fs::path> ini_path = find_file(folder, "mod.ini");
    if (!ini_path) return read_id_files(*this, entries_, folder);
    const std::optional<IniFile> ini = IniFile::load(*ini_path);
    if (!ini) return std::nullopt;

    Mod mod;
    mod.name = ini->get(kInfo, "Name");
    if (mod.name.empty()) mod.name = to_utf8(folder.filename());
    mod.author = ini->get(kInfo, "Author");
    // Published mods also give it as a section of its own.
    if (mod.author.empty()) mod.author = ini->get("Author", "Name");
    std::string description = ini->get(kInfo, "Description");
    std::replace(description.begin(), description.end(), '\\', '\n');
    mod.description = std::move(description);
    const std::string version = [&] {
        std::string v = trim(ini->get(kInfo, "Version"));
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::toupper(c); });
        return v;
    }();
    mod.depends = split_list(ini->get(kInfo, "Depends"));
    if (const std::string *priority = ini->find(kInfo, "Priority")) {
        int value{};
        const std::string text = trim(*priority);
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec == std::errc{} && end == text.data() + text.size() && value >= 0 && value <= 5)
            mod.priority = value;
        else
            mod.notes.push_back("Its Priority \"" + text + "\" is not 0 to 5; 3 is used.");
    }
    if (const std::optional<fs::path> preview = find_file(folder, "preview.png")) mod.preview = *preview;
    if (ini->find(kInfo, "Script") != nullptr)
        mod.notes.push_back("Its settings script is not run: scripts are for the PSP mod manager.");
    if (!ini->has_section(kInfo)) mod.unusable = "Its mod.ini has no [MOD INFO] section.";

    Reader reader(*this, entries_, folder, mod);
    const std::string type = trim(ini->get(kInfo, "Type"));
    mod.version = version_label(version);
    if (iequals(type, "File")) {
        mod.type = "Files";
        reader.file_changes(*ini, kInfo, version, FileChange::Kind::Replace);
    } else if (iequals(type, "Patch")) {
        mod.type = "Patch";
        reader.file_changes(*ini, kInfo, version, FileChange::Kind::Patch);
    } else if (iequals(type, "Code")) {
        mod.type = "Code";
        reader.code_files(*ini, kInfo, version);
    } else if (iequals(type, "Yakumo")) {
        mod.type = "Yakumo mod";
        mod.version = "HD version";
        reader.yakumo(*ini, version);
        if (const std::string *own = ini->find(kInfo, "ModVersion")) mod.version = "HD version; mod version " + *own;
    } else if (iequals(type, "Pack")) {
        mod.type = "Pack";
        mod.members = split_list(ini->get(kInfo, "ModList"));
        // Published packs also name their mods only in Depends.
        if (mod.members.empty()) mod.members = mod.depends;
        mod.version.clear();
        if (mod.members.empty()) reader.unusable("The pack lists no mods.");
    } else if (iequals(type, "PseudoPack")) {
        mod.type = "Pack";
        for (const std::string &sub : split_list(ini->get(kInfo, "SubModList"))) {
            const std::string sub_type = trim(ini->get(sub, "Type"));
            Mod part;
            Reader sub_reader(*this, entries_, folder, part);
            if (iequals(sub_type, "File"))
                sub_reader.file_changes(*ini, sub, version, FileChange::Kind::Replace);
            else if (iequals(sub_type, "Patch"))
                sub_reader.file_changes(*ini, sub, version, FileChange::Kind::Patch);
            else if (iequals(sub_type, "Code"))
                sub_reader.code_files(*ini, sub, version);
            else
                part.unusable = "its type " + (sub_type.empty() ? std::string("is missing") : sub_type) +
                    " cannot be part of a pack here";
            if (!part.unusable.empty()) {
                mod.notes.push_back("Part \"" + sub + "\" is left out: " + part.unusable);
                continue;
            }
            mod.changes.insert(mod.changes.end(), part.changes.begin(), part.changes.end());
            mod.code.insert(mod.code.end(), part.code.begin(), part.code.end());
        }
        if (mod.changes.empty() && mod.code.empty()) reader.unusable("None of its parts can be used here.");
    } else if (type.size() > 5u && iequals(std::string_view(type).substr(0, 5), "Equip")) {
        const std::string kind = type.substr(5);
        mod.version.clear();
        if (iequals(kind, "SET")) {
            mod.type = "Armour set";
            reader.slots(ini->get(kInfo, "Files"), {"Head", "Arms", "Body", "Waist", "Legs"},
                {"HEAD", "ARMS", "BODY", "WAIST", "LEGS"});
        } else if (iequals(kind, "CATSET")) {
            mod.type = "Felyne armour set";
            reader.slots(ini->get(kInfo, "Files"), {"Felyne helm", "Felyne plate"}, {"CATHELM", "CATPLATE"});
        } else {
            const auto found = std::find_if(std::begin(kEquipTypes), std::end(kEquipTypes),
                [&kind](const EquipType &t) { return iequals(t.key, kind); });
            if (found == std::end(kEquipTypes)) {
                mod.type = type;
                reader.unusable("Unknown equipment type " + kind + ".");
            } else {
                mod.type = found->label;
                reader.slots(ini->get(kInfo, "Files"), {found->label}, {found->key});
            }
        }
        if (ini->find(kInfo, "Animation") != nullptr)
            mod.notes.push_back("Its custom animations are not used: they need a code mod.");
        if (ini->find(kInfo, "Audio") != nullptr) mod.notes.push_back("Its custom sounds are not used yet.");
    } else {
        mod.type = type.empty() ? "Unknown" : type;
        reader.unusable(type.empty() ? "Its mod.ini gives no Type." : "Unknown type " + type + ".");
    }
    return mod;
}

std::optional<OverlayImage> overlay_image(const std::vector<std::uint8_t> &bytes) {
    constexpr std::size_t kHeader = 64u;
    if (bytes.size() < kHeader || bytes[0] != 'M' || bytes[1] != 'W' || bytes[2] != 'o' || bytes[3] != '3')
        return std::nullopt;
    OverlayImage image;
    image.load = load32(bytes, 8u);
    image.code_end = image.load + static_cast<std::uint32_t>(kHeader) + load32(bytes, 12u);
    image.size = static_cast<std::uint32_t>(bytes.size());
    return image;
}

std::optional<FileId> quest_file(std::uint32_t quest) {
    if (!game::is_mod_quest(quest)) return std::nullopt;
    return static_cast<FileId>(kFirstQuestFile + (quest - game::kFirstModQuest));
}

std::optional<std::uint32_t> quest_of_file(FileId file) {
    if (file < kFirstQuestFile || file >= kFirstQuestFile + kQuestFiles) return std::nullopt;
    return game::kFirstModQuest + (file - kFirstQuestFile);
}

std::optional<std::uint32_t> ram_range(std::uint32_t address, std::size_t length) {
    const std::uint32_t guest = address & kMirrorMask;
    if (guest < kRamStart || guest >= kRamEnd || length > kRamEnd - guest) return std::nullopt;
    return guest;
}

namespace {

std::uint32_t word_at(std::span<const std::uint8_t> data, std::size_t at) {
    return static_cast<std::uint32_t>(data[at]) | static_cast<std::uint32_t>(data[at + 1u]) << 8u |
        static_cast<std::uint32_t>(data[at + 2u]) << 16u | static_cast<std::uint32_t>(data[at + 3u]) << 24u;
}

Blocks read_plain(std::span<const std::uint8_t> data) {
    Blocks result;
    std::size_t at = 0u;
    while (at + 8u <= data.size()) {
        const std::uint32_t address = word_at(data, at);
        const std::uint32_t length_word = word_at(data, at + 4u);
        if (address == 0xFFFFFFFFu) break;
        const std::uint32_t length = length_word & 0x7FFFFFFFu;
        if (length > data.size() - at - 8u) {
            result.problems.push_back("the block for " + hex8(address) + " runs past the end of the file");
            break;
        }
        const auto payload = data.begin() + static_cast<std::ptrdiff_t>(at + 8u);
        result.blocks.push_back(
            {address, std::vector<std::uint8_t>(payload, payload + length), (length_word & 0x80000000u) != 0u, {}});
        at += 8u + length;
    }
    return result;
}

// A typed file, or nothing when the data is not one: an unknown type, or a
// block that runs past the end.
std::optional<Blocks> read_typed(std::span<const std::uint8_t> data, const Place &place) {
    Blocks result;
    std::size_t at = 0u;
    // A version before the blocks ("0.01"): four printable characters.
    if (data.size() >= 4u &&
        std::all_of(data.begin(), data.begin() + 4, [](std::uint8_t c) { return c == '.' || (c >= '0' && c <= '9'); }))
        at = 4u;
    std::optional<std::uint32_t> main; // where the last main block went
    std::string main_id;
    const auto placed = [&](const std::string &id, std::span<const std::uint8_t> code, bool run) {
        std::optional<std::uint32_t> base = place ? place(id, static_cast<std::uint32_t>(code.size())) : std::nullopt;
        if (!base) {
            result.problems.push_back("there is no memory for its code block " + id);
            return base;
        }
        result.blocks.push_back({*base, std::vector<std::uint8_t>(code.begin(), code.end()), run, {}});
        return base;
    };
    while (at + 4u <= data.size()) {
        const auto type = static_cast<std::int32_t>(word_at(data, at));
        at += 4u;
        if (type == -1) break;
        if (type == 0 || type == 1 || type == 3) {
            // 0: address, length, bytes; 1: length, id, bytes; 3: length, bytes.
            const std::size_t head = type == 3 ? 4u : 8u;
            if (at + head > data.size()) return std::nullopt;
            const std::uint32_t length_word = word_at(data, type == 0 ? at + 4u : at);
            const std::uint32_t length = length_word & 0x7FFFFFFFu;
            if (length > data.size() - at - head) return std::nullopt;
            const auto payload = data.subspan(at + head, length);
            const bool run = (length_word & 0x80000000u) != 0u;
            if (type == 0) {
                result.blocks.push_back(
                    {word_at(data, at), std::vector<std::uint8_t>(payload.begin(), payload.end()), run, {}});
            } else if (type == 1) {
                main_id.assign(reinterpret_cast<const char *>(data.data() + at + 4u), 4u);
                main = placed(main_id, payload, run);
            } else {
                (void)placed("init", payload, true);
            }
            at += head + length;
        } else if (type == 2) {
            if (at + 8u > data.size()) return std::nullopt;
            const std::uint32_t address = word_at(data, at);
            const std::uint32_t offset = static_cast<std::uint32_t>(data[at + 4u]) | data[at + 5u] << 8u;
            const std::uint8_t op = data[at + 6u];
            const bool nop = data[at + 7u] != 0u;
            at += 8u;
            if (!main) {
                result.problems.push_back("the hook at " + hex8(address) + " has no code block to go to");
                continue;
            }
            const std::uint32_t instruction =
                static_cast<std::uint32_t>(op) << 24u | (((*main + offset) >> 2u) & 0x03FFFFFFu);
            std::vector<std::uint8_t> bytes(nop ? 8u : 4u, 0u);
            for (unsigned i = 0; i < 4u; ++i) bytes[i] = static_cast<std::uint8_t>(instruction >> (8u * i));
            result.blocks.push_back({address, std::move(bytes), false, {}});
        } else {
            return std::nullopt;
        }
    }
    return result;
}

} // namespace

Blocks read_blocks(std::span<const std::uint8_t> data, const Place &place) {
    // A plain file starts with an address; a typed one with a type (0 to 3)
    // or a version. Anything that does not read as typed is plain.
    if (data.size() >= 4u && (word_at(data, 0u) <= 3u || std::isdigit(data[0]) != 0)) {
        if (std::optional<Blocks> typed = read_typed(data, place)) return std::move(*typed);
    }
    return read_plain(data);
}

Blocks read_blocks(const fs::path &file, const Place &place) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return Blocks{{}, {"cannot read " + to_utf8(file.filename())}};
    const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return read_blocks(data, place);
}

Blocks read_blocks(const Resolution::Source &source, const Place &place) {
    if (source.edits.empty()) return read_blocks(source.path, place);
    Blocks result;
    for (const Edit &edit : source.edits) result.blocks.push_back({edit.address, edit.bytes, false, edit.expect});
    return result;
}

std::optional<std::string> mismatch(std::span<const std::uint8_t> expect, std::span<const std::uint8_t> found) {
    if (expect.empty() || std::equal(expect.begin(), expect.end(), found.begin(), found.end())) return std::nullopt;
    const auto spell = [](std::span<const std::uint8_t> bytes) {
        std::string text;
        for (const std::uint8_t b : bytes) {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%02X", b);
            text += (text.empty() ? "" : " ") + std::string(hex);
        }
        return text;
    };
    return "expected " + spell(expect) + ", found " + spell(found);
}

PatchOutcome apply_patch(std::vector<std::uint8_t> &bytes, const fs::path &patch) {
    return apply_patch(bytes, read_blocks(patch));
}

PatchOutcome apply_patch(std::vector<std::uint8_t> &bytes, const Blocks &patch) {
    PatchOutcome outcome;
    outcome.problems = patch.problems;
    const std::optional<OverlayImage> overlay = overlay_image(bytes);
    // Changes the file's bytes at `at`, unless they are not what the block expects.
    const auto change = [&bytes, &outcome](const Block &block, std::size_t at) {
        if (const auto wrong =
                mismatch(block.expect, std::span<const std::uint8_t>(bytes).subspan(at, block.bytes.size()))) {
            outcome.problems.push_back("the block for " + hex8(block.address) + " is skipped: " + *wrong);
            return false;
        }
        std::copy(block.bytes.begin(), block.bytes.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
        ++outcome.applied;
        return true;
    };
    for (const Block &block : patch.blocks) {
        const std::uint32_t address = block.address;
        const auto length = static_cast<std::uint32_t>(block.bytes.size());
        const std::uint32_t guest = address & kMirrorMask;
        const bool in_ram = guest >= kRamStart && guest < kRamEnd;
        if (block.run && !in_ram) {
            outcome.problems.push_back(
                "the block for " + hex8(address) + " is code to run as it loads, but its address is in the file");
            continue;
        }
        const bool in_image =
            overlay && in_ram && guest >= overlay->load && guest - overlay->load + length <= overlay->size;
        if (in_image && guest >= overlay->code_end) {
            change(block, guest - overlay->load);
            if (block.run) outcome.run.push_back(guest);
        } else if (in_ram) {
            // Outside the overlay, and its header and code too: the overlay
            // keeps the bytes its recompiled code was made from, and the
            // change goes to memory once its code is installed. A patch for
            // any other file is code loaded with it: its memory blocks are
            // written once the game has read the whole file.
            outcome.after_load.push_back({guest, block.bytes, block.expect});
            if (block.run) outcome.run.push_back(guest);
        } else if (!in_ram && static_cast<std::uint64_t>(address) + length <= bytes.size()) {
            change(block, address);
        } else {
            outcome.problems.push_back("the block for " + hex8(address) + " is outside the file");
        }
    }
    return outcome;
}

} // namespace mhp3rd::mods::p3rd
