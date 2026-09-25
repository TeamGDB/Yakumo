#include "text/translation.hpp"

#include "psprecomp/guest_memory.hpp"

#include <iostream>

namespace mhp3rd::text {
namespace {

struct State {
    bool loaded{};
    std::string code{"original"};
    std::string name{"Original"};
    Translations translations;
    std::vector<std::filesystem::path> directories;
    std::optional<Arena> arena;
    bool applied{};
    bool warned_arena{};
};

State &state() {
    static State value;
    return value;
}

// The game's own text, whatever language the disc image is in: English on a
// patched image, Japanese on an original one, and anything else otherwise.
// Nothing is loaded or applied for it; the special codes are the older "en".
bool is_original(const std::string &code) {
    return code.empty() || code == "original" || code == "en" || code == "en-US" || code == "en-GB";
}

} // namespace

void set_language(const std::string &code, const std::vector<std::filesystem::path> &directories) {
    State &s = state();
    s.loaded = true;
    s.code = code.empty() ? "original" : code;
    s.name = "Original";
    s.translations = Translations{};
    s.directories = directories;
    s.arena.reset();
    s.applied = false;
    s.warned_arena = false;
    if (is_original(s.code)) return;

    for (const std::filesystem::path &directory : directories) {
        for (const Language &language : scan_languages(directory)) {
            if (language.code != s.code) continue;
            std::string error;
            const std::optional<Translations> loaded = Translations::from_file(language.file, error);
            if (!loaded) {
                std::cout << "[text] " << error << "\n";
                return;
            }
            s.translations = *loaded;
            if (!s.translations.name().empty()) s.name = s.translations.name();
            return;
        }
    }
    std::cout << "[text] no " << s.code << " translation found; the game's own text is used\n";
}

bool active() noexcept { return !state().translations.empty(); }

const std::string &language_code() noexcept { return state().code; }

const std::string &language_name() noexcept { return state().name; }

std::vector<Language> languages() {
    std::vector<Language> all;
    for (const std::filesystem::path &directory : state().directories) {
        for (Language &language : scan_languages(directory)) {
            const bool known = std::any_of(all.begin(), all.end(), [&](const Language &other) {
                return other.code == language.code;
            });
            if (!known) all.push_back(std::move(language));
        }
    }
    std::sort(all.begin(), all.end(), [](const Language &a, const Language &b) { return a.code < b.code; });
    return all;
}

const std::vector<std::filesystem::path> &search_directories() noexcept { return state().directories; }

void frame(psprecomp::GuestMemory &memory, const ArenaAllocator &allocate) {
    State &s = state();
    if (!s.loaded || s.translations.empty() || s.applied) return;
    if (!s.arena) {
        const std::optional<Arena> arena = allocate(s.translations.arena_bytes());
        if (!arena || !arena->valid()) {
            if (!s.warned_arena) {
                std::cout << "[text] no guest memory for the translations; the game's text is used\n";
                s.warned_arena = true;
            }
            return;
        }
        s.arena = arena;
    }
    const ApplyResult result = apply(memory, kTextBlock, s.translations, *s.arena);
    if (!result.block) return;  // the game has not loaded its text yet
    s.applied = true;
    std::cout << "[text] " << s.name << " (" << s.code << "): applied " << result.applied << " of "
              << s.translations.size() << " strings, " << result.missing << " not in this text, " << result.skipped
              << " did not fit\n";
}

} // namespace mhp3rd::text
