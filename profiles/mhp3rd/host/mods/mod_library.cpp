#include "mods/mod_library.hpp"

#include "mods/mod_ini.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <system_error>

namespace mhp3rd::mods {

namespace fs = std::filesystem;

namespace {

constexpr const char *kGeneral = "general";
constexpr const char *kModPrefix = "mod ";

} // namespace

const char *native_platform() noexcept {
#if defined(__ANDROID__) && defined(__aarch64__)
    return "android-arm64";
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "windows-x64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
    return "macos-x64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux-arm64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x64";
#else
    return "unknown";
#endif
}

const char *native_extension() noexcept {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

void ModLibrary::scan(const fs::path &folder) {
    mods_.clear();
    std::error_code ec;
    fs::directory_iterator it(folder, ec);
    if (ec) return;
    std::vector<fs::path> folders;
    for (const fs::directory_entry &entry : it) {
        const std::string name = to_utf8(entry.path().filename());
        if (name.empty() || name.front() == '.') continue; // .backup and hidden folders
        if (entry.is_directory(ec)) folders.push_back(entry.path());
    }
    std::sort(folders.begin(), folders.end());
    int top = 0;
    for (const auto &[id, c] : choices_) top = std::max(top, c.rank);
    for (const fs::path &path : folders) {
        std::optional<Mod> mod = format_->read(path);
        if (!mod) continue;
        mod->id = to_utf8(path.filename());
        mod->folder = path;
        // A mod seen for the first time goes above the others: the one added
        // last wins, as players expect.
        if (!choices_.contains(mod->id)) choices_[mod->id].rank = ++top;
        ModChoice &c = choices_[mod->id];
        c.slots.resize(mod->slots.size());
        if (!mod->unusable.empty()) c.enabled = false;
        mods_.push_back(std::move(*mod));
    }
    sort();
}

void ModLibrary::sort() {
    std::stable_sort(mods_.begin(), mods_.end(), [this](const Mod &a, const Mod &b) {
        const int ra = choices_.at(a.id).rank;
        const int rb = choices_.at(b.id).rank;
        if (ra != rb) return ra > rb;
        return a.id < b.id;
    });
}

void ModLibrary::load_choices(const fs::path &file) {
    choices_.clear();
    master_ = true;
    const std::optional<IniFile> ini = IniFile::load(file);
    if (!ini) return;
    master_ = ini->get(kGeneral, "enabled", "1") != "0";
    for (const std::string &section : ini->sections()) {
        if (!iequals(std::string_view(section).substr(0, 4), kModPrefix)) continue;
        const std::string id = section.substr(4);
        ModChoice c;
        c.enabled = ini->get(section, "enabled") == "1";
        try {
            c.rank = std::stoi(ini->get(section, "rank", "0"));
        } catch (const std::exception &) {
            c.rank = 0;
        }
        for (std::size_t slot = 0;; ++slot) {
            const std::string *value = ini->find(section, "slot" + std::to_string(slot + 1u));
            if (value == nullptr) break;
            c.slots.push_back(format_->parse_file(*value));
        }
        for (const auto &[key, value] : ini->entries(section)) {
            if (!iequals(std::string_view(key).substr(0, 8), "setting.") || key.size() <= 8u) continue;
            try {
                c.settings[key.substr(8)] = std::stoll(value);
            } catch (const std::exception &) {
            }
        }
        choices_[id] = std::move(c);
    }
}

bool ModLibrary::save_choices(const fs::path &file, std::string &error) const {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path partial = file;
    partial += ".part";
    {
        std::ofstream out(partial, std::ios::trunc);
        out << "# Written by Yakumo's Mods menu: which mods are on, and their order (a higher rank wins where\n"
               "# two mods change the same file). The mods themselves are folders in the mods folder.\n"
            << "[" << kGeneral << "]\nenabled=" << (master_ ? 1 : 0) << "\n";
        for (const auto &[id, c] : choices_) {
            out << "\n[" << kModPrefix << id << "]\nenabled=" << (c.enabled ? 1 : 0) << "\nrank=" << c.rank << "\n";
            for (std::size_t slot = 0; slot < c.slots.size(); ++slot)
                out << "slot" << slot + 1u << "=" << (c.slots[slot] ? format_->file_name(*c.slots[slot]) : "") << "\n";
            for (const auto &[key, value] : c.settings) out << "setting." << key << "=" << value << "\n";
        }
        if (!out) {
            error = "cannot write " + to_utf8(partial);
            return false;
        }
    }
    fs::rename(partial, file, ec);
    if (ec) {
        error = "cannot replace " + to_utf8(file) + ": " + ec.message();
        return false;
    }
    return true;
}

const Mod *ModLibrary::find(const std::string &id) const {
    for (const Mod &mod : mods_) {
        if (mod.id == id) return &mod;
    }
    return nullptr;
}

ModChoice ModLibrary::choice(const std::string &id) const {
    const auto found = choices_.find(id);
    return found != choices_.end() ? found->second : ModChoice{};
}

ModChoice &ModLibrary::choice_for(const std::string &id) {
    return choices_[id];
}

void ModLibrary::set_enabled(const std::string &id, bool on) {
    std::set<std::string> visited;
    // Dependencies and pack members may name each other; each mod once.
    const auto apply = [&](const auto &self, const std::string &target, bool value) -> void {
        if (!visited.insert(target).second) return;
        const Mod *mod = find(target);
        if (mod == nullptr) return;
        if (value && !mod->unusable.empty()) return;
        choice_for(target).enabled = value;
        for (const std::string &member : mod->members) self(self, member, value);
        if (value) {
            for (const std::string &dependency : mod->depends) self(self, dependency, true);
        }
    };
    apply(apply, id, on);
}

void ModLibrary::set_slot(const std::string &id, std::size_t slot, std::optional<FileId> file) {
    ModChoice &c = choice_for(id);
    if (c.slots.size() <= slot) c.slots.resize(slot + 1u);
    c.slots[slot] = file;
}

bool valid_setting_key(std::string_view key) noexcept {
    if (key.empty() || key.size() > 64u) return false;
    return std::all_of(key.begin(), key.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
            c == '-';
    });
}

namespace {

// A setting's value within its range: 0 or 1 for a switch, a choice's index,
// a number between its minimum and maximum.
std::int64_t clamped(const ModSetting &setting, std::int64_t value) {
    const std::int64_t maximum = setting.kind == ModSetting::Kind::Choice
        ? static_cast<std::int64_t>(setting.choices.empty() ? 0u : setting.choices.size() - 1u)
        : setting.kind == ModSetting::Kind::Switch ? 1
                                                   : setting.maximum;
    const std::int64_t minimum = setting.kind == ModSetting::Kind::Number ? setting.minimum : 0;
    return std::clamp(value, minimum, std::max(minimum, maximum));
}

} // namespace

std::int64_t ModLibrary::declare_setting(const std::string &id, const ModSetting &setting) {
    if (!valid_setting_key(setting.key)) return setting.fallback;
    std::vector<ModSetting> &declared = declared_[id];
    const auto same = std::find_if(
        declared.begin(), declared.end(), [&setting](const ModSetting &s) { return s.key == setting.key; });
    if (same != declared.end())
        *same = setting;
    else
        declared.push_back(setting);
    return clamped(setting, this->setting(id, setting.key, setting.fallback));
}

std::int64_t ModLibrary::declared_value(const std::string &id, const std::string &key, std::int64_t fallback) const {
    for (const ModSetting &declared : settings_of(id))
        if (declared.key == key) return clamped(declared, this->setting(id, key, declared.fallback));
    return this->setting(id, key, fallback);
}

const std::vector<ModSetting> &ModLibrary::settings_of(const std::string &id) const {
    static const std::vector<ModSetting> kNone;
    const auto found = declared_.find(id);
    return found != declared_.end() ? found->second : kNone;
}

std::int64_t ModLibrary::setting(const std::string &id, const std::string &key, std::int64_t fallback) const {
    const auto found = choices_.find(id);
    if (found == choices_.end()) return fallback;
    const auto value = found->second.settings.find(key);
    return value != found->second.settings.end() ? value->second : fallback;
}

void ModLibrary::set_setting(const std::string &id, const std::string &key, std::int64_t value) {
    if (!valid_setting_key(key)) return;
    choice_for(id).settings[key] = value;
}

void ModLibrary::add_runtime_change(const std::string &id, const FileChange &change) {
    std::vector<FileChange> &changes = runtime_changes_[id];
    std::erase_if(changes, [&change](const FileChange &c) { return c.file == change.file && c.kind == change.kind; });
    changes.push_back(change);
}

std::optional<FileChange> ModLibrary::runtime_change(const std::string &id, FileId file, FileChange::Kind kind) const {
    const auto found = runtime_changes_.find(id);
    if (found == runtime_changes_.end()) return std::nullopt;
    for (const FileChange &change : found->second)
        if (change.file == file && change.kind == kind) return change;
    return std::nullopt;
}

void ModLibrary::remove_runtime_change(const std::string &id, FileId file, FileChange::Kind kind) {
    if (const auto found = runtime_changes_.find(id); found != runtime_changes_.end())
        std::erase_if(found->second, [&](const FileChange &c) { return c.file == file && c.kind == kind; });
}

void ModLibrary::move(const std::string &id, int delta) {
    const auto it = std::find_if(mods_.begin(), mods_.end(), [&id](const Mod &m) { return m.id == id; });
    if (it == mods_.end()) return;
    const auto index = static_cast<std::ptrdiff_t>(it - mods_.begin());
    // The list is highest first: raising a mod swaps it with the one above.
    const std::ptrdiff_t other = index - delta;
    if (other < 0 || other >= static_cast<std::ptrdiff_t>(mods_.size())) return;
    ModChoice &a = choice_for(id);
    ModChoice &b = choice_for(mods_[static_cast<std::size_t>(other)].id);
    if (a.rank == b.rank)
        a.rank += delta;
    else
        std::swap(a.rank, b.rank);
    sort();
}

Resolution ModLibrary::resolve() const {
    Resolution result;
    if (!master_) return result;
    struct Touch {
        std::vector<std::string> replaced; // lowest priority first
        std::vector<std::string> patched;
    };
    std::map<FileId, Touch> touches;
    // Lowest priority first, so a later mod's replacement wins and its patch
    // is applied after (over) the earlier ones.
    for (auto it = mods_.rbegin(); it != mods_.rend(); ++it) {
        const Mod &mod = *it;
        const ModChoice c = choice(mod.id);
        if (!c.enabled || !mod.unusable.empty()) continue;
        std::vector<FileChange> changes = mod.changes;
        if (const auto extra = runtime_changes_.find(mod.id); extra != runtime_changes_.end())
            changes.insert(changes.end(), extra->second.begin(), extra->second.end());
        for (std::size_t slot = 0; slot < mod.slots.size() && slot < c.slots.size(); ++slot) {
            if (c.slots[slot]) changes.push_back({FileChange::Kind::Replace, *c.slots[slot], mod.slots[slot].source});
        }
        for (const FileChange &change : changes) {
            Touch &touch = touches[change.file];
            if (change.kind == FileChange::Kind::Add || change.kind == FileChange::Kind::Quest) {
                (change.kind == FileChange::Kind::Add ? result.added : result.quests)[change.file] = {
                    mod.id, change.source};
                std::erase(touch.replaced, mod.id);
                touch.replaced.push_back(mod.id);
            } else if (change.kind == FileChange::Kind::Replace) {
                result.replacements[change.file] = {mod.id, change.source};
                std::erase(touch.replaced, mod.id);
                touch.replaced.push_back(mod.id);
            } else {
                result.patches[change.file].push_back({mod.id, change.source, change.edits});
                touch.patched.push_back(mod.id);
            }
        }
    }
    // Code: by priority, then in the order above (lowest rank first).
    std::vector<std::pair<int, const Mod *>> coders;
    for (auto it = mods_.rbegin(); it != mods_.rend(); ++it) {
        if ((!it->code.empty() || !it->memory.empty() || !it->native.empty() || !it->script.empty()) &&
            choice(it->id).enabled && it->unusable.empty())
            coders.emplace_back(it->priority, &*it);
    }
    std::stable_sort(coders.begin(), coders.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &[priority, mod] : coders) {
        for (const std::filesystem::path &file : mod->code) result.code.push_back({mod->id, file, {}});
        if (!mod->memory.empty()) result.code.push_back({mod->id, {}, mod->memory});
    }
    // Native libraries and scripts start in the same order, except that a mod
    // starts after the mods it depends on, whose services it may import.
    std::vector<const Mod *> waiting;
    for (const auto &[priority, mod] : coders)
        if (!mod->native.empty() || !mod->script.empty()) waiting.push_back(mod);
    const auto starts = [&waiting](const std::string &id) {
        return std::any_of(waiting.begin(), waiting.end(), [&id](const Mod *m) { return m->id == id; });
    };
    while (!waiting.empty()) {
        // The first whose dependencies have started; with a cycle, the first.
        auto next = std::find_if(waiting.begin(), waiting.end(),
            [&starts](const Mod *mod) { return std::none_of(mod->depends.begin(), mod->depends.end(), starts); });
        if (next == waiting.end()) next = waiting.begin();
        const Mod *mod = *next;
        waiting.erase(next);
        if (!mod->native.empty()) result.native.push_back({mod->id, mod->native, {}});
        if (!mod->script.empty()) result.scripts.push_back({mod->id, mod->script, {}});
    }
    for (const auto &[file, touch] : touches) {
        const bool several = touch.replaced.size() > 1u;
        bool foreign_patch = false;
        for (const std::string &patcher : touch.patched)
            foreign_patch = foreign_patch || (!touch.replaced.empty() && patcher != touch.replaced.back());
        if (!several && !foreign_patch) continue;
        Resolution::Conflict conflict;
        conflict.file = file;
        if (!touch.replaced.empty()) conflict.winner = touch.replaced.back();
        for (auto r = touch.replaced.rbegin(); r != touch.replaced.rend(); ++r) {
            if (*r != conflict.winner) conflict.overridden.push_back(*r);
        }
        conflict.patched_by = touch.patched;
        result.conflicts.push_back(std::move(conflict));
    }
    return result;
}

} // namespace mhp3rd::mods
