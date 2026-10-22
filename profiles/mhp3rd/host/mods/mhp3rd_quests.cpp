#include "mods/mhp3rd_quests.hpp"

#include "mods/mhp3rd_added_files.hpp"

#include "mods/mhp3rd_mod_format.hpp"
#include "mods/mhp3rd_tables.hpp"
#include "mods/mod_ini.hpp"
#include "mods/mod_native.hpp"

#include "game/guest_ram.hpp"
#include "game/quest_file.hpp"

#include "overlays.hpp"

#include "psprecomp/runtime.hpp"

#include "yakumo/mod.h"

#include <array>
#include <fstream>
#include <iostream>
#include <iterator>

namespace mhp3rd::mods::quests {
namespace {

namespace fs = std::filesystem;

// Where the game's id-to-file code arrives with no file for an id: s0 holds
// 0xFFFF and t1 the quest id; the next instruction gives up on 0xFFFF.
constexpr std::uint32_t kNoFile = 0x088C18B8u;

struct CodeWord {
    std::uint32_t address;
    std::uint32_t word;
};
// The instructions around it, as traced on NPJB-40001.
constexpr std::array<CodeWord, 5> kSignature{{
    {0x088C1778u, 0x30A9FFFFu}, // andi t1, a1, 0xFFFF   (the id, at the function's start)
    {0x088C18B4u, 0x3410FFFFu}, // ori s0, zero, 0xFFFF  (no file)
    {0x088C18B8u, 0x3402FFFFu}, // ori v0, zero, 0xFFFF
    {0x088C18BCu, 0x120200ACu}, // beq s0, v0, (give up)
    {0x088C18C4u, 0x966221B2u}, // lhu v0, 0x21B2(s3)    (the file loaded last)
}};

// Where the event quest loader (0x0888F9F4) has worked out the file for the
// quest id in a1, into s3, and before it reads the file.
constexpr std::uint32_t kEventFile = 0x0888FA4Cu;
constexpr std::array<CodeWord, 6> kEventSignature{{
    {0x0888F9F4u, 0x30A5FFFFu}, // andi a1, a1, 0xFFFF   (the id)
    {0x0888F9F8u, 0x3402EE48u}, // ori v0, zero, 61000
    {0x0888FA00u, 0x24A3253Au}, // addiu v1, a1, 9530    (the file, for ids up to 61000)
    {0x0888FA44u, 0x24A22187u}, // addiu v0, a1, 8583    (and above)
    {0x0888FA48u, 0x3053FFFFu}, // andi s3, v0, 0xFFFF
    {0x0888FA4Cu, 0x3C1409FCu}, // lui s4, 0x09FC
}};
// The event quest list, in lobby_task: the game's ids, 0 after the last.
constexpr const char *kEvents = "quest.events";
constexpr std::uint32_t kEventsGame = 0x0A188B1Cu;

struct State {
    std::map<std::uint32_t, FileId> files; // by quest id
    std::vector<Quest> served;
    std::map<std::uint32_t, Rebuild> rebuilds; // quests to build again once the text is loaded
    ymod_mod *host{};
    ymod_hook hook{};
    ymod_hook event_hook{};
    bool events{}; // the list was made longer for them
};

State &state() {
    static State s;
    return s;
}

int give_file(ymod_cpu *cpu, void *) {
    const std::uint32_t quest = cpu->gpr[9] & 0xFFFFu;                                                            // t1
    if (const auto found = state().files.find(quest); found != state().files.end()) cpu->gpr[16] = found->second; // s0
    return YMOD_CONTINUE;
}

int give_event_file(ymod_cpu *cpu, void *) {
    const std::uint32_t quest = cpu->gpr[5] & 0xFFFFu;                                                            // a1
    if (const auto found = state().files.find(quest); found != state().files.end()) cpu->gpr[19] = found->second; // s3
    return YMOD_CONTINUE;
}

template <std::size_t N> bool code_as_traced(psprecomp::Runtime &runtime, const std::array<CodeWord, N> &signature) {
    for (const CodeWord &expected : signature)
        if (!runtime.memory().contains(expected.address, 4u) ||
            runtime.memory().load32(expected.address) != expected.word)
            return false;
    return true;
}

// The hooks that give the game mods' quest files, and the event quest list
// made long enough for them: once, then the list grows with each quest.
bool attach(psprecomp::Runtime &runtime) {
    State &s = state();
    native::attach(runtime);
    if (s.host == nullptr) s.host = native::add_mod("yakumo.quests", "Quests from mods", "");
    const auto *hooks = static_cast<const ymod_hooks *>(native::host()->service(s.host, YMOD_HOOKS, 1u));
    if (s.hook == 0u && hooks != nullptr) s.hook = hooks->instruction(s.host, nullptr, kNoFile, &give_file, nullptr);
    if (s.hook == 0u) {
        std::cerr << "[mods] the quest hook cannot be installed; quests mods add are not served\n";
        return false;
    }
    std::string why;
    if (!code_as_traced(runtime, kEventSignature))
        why = "the event quest loader is not as traced";
    else if (s.event_hook == 0u &&
        (s.event_hook = hooks->instruction(s.host, nullptr, kEventFile, &give_event_file, nullptr)) == 0u)
        why = "the event quest hook cannot be installed";
    else if (const tables::Table *list = tables::find(kEvents); list == nullptr ||
        !tables::extend(runtime, kEvents, list->rows + static_cast<std::uint32_t>(s.files.size()), &why))
        why = "the event quest list cannot be made longer: " + why;
    s.events = why.empty();
    if (!s.events) std::cerr << "[mods] quests mods add are not at the counter: " << why << "\n";
    return true;
}

} // namespace

bool add(psprecomp::Runtime &runtime, std::vector<std::uint8_t> payload, const std::string &mod, std::string *why) {
    State &s = state();
    const auto fail = [why](std::string reason) {
        if (why != nullptr) *why = std::move(reason);
        return false;
    };
    const std::optional<game::QuestRecord> record = game::read_quest_record(payload);
    if (!record || !game::is_mod_quest(record->id)) return fail("it is not a quest with a mod's id");
    if (s.files.contains(record->id)) return fail("quest " + std::to_string(record->id) + " is served already");
    if (payload.size() > game::kQuestMaxPayload) return fail("it is larger than the game reads");
    if (!code_as_traced(runtime, kSignature)) return fail("the game's quest loading is not as traced");
    game::GuestRam ram(runtime.memory());
    const std::optional<game::QuestKeys> keys = game::read_quest_keys(ram);
    if (!keys) return fail("the game's quest keys are not in memory");
    const std::optional<FileId> file = p3rd::quest_file(record->id);
    if (!file || !added::add(runtime, *file, game::encrypt_quest(payload, *keys)))
        return fail("its file cannot be served");
    s.files[record->id] = *file;
    s.served.push_back({record->id, record->name, record->fee, record->frames, record->stars, mod});
    if (!attach(runtime)) {
        s.files.erase(record->id);
        s.served.pop_back();
        return fail("the quest hook cannot be installed");
    }
    std::cout << "[mods] " << mod << " adds quest " << record->id << " (" << record->name << ")\n";
    return true;
}

bool install(psprecomp::Runtime &runtime, const std::map<FileId, Resolution::Source> &quests,
    std::map<FileId, Resolution::Source> &files, const fs::path &cache) {
    if (quests.empty()) return true;
    State &s = state();
    if (!code_as_traced(runtime, kSignature)) {
        std::cerr << "[mods] the game's quest loading is not as traced; quests mods add are not served\n";
        return false;
    }
    game::GuestRam ram(runtime.memory());
    const std::optional<game::QuestKeys> keys = game::read_quest_keys(ram);
    if (!keys) {
        std::cerr << "[mods] the game's quest keys are not in memory; quests mods add are not served\n";
        return false;
    }
    std::error_code ec;
    fs::create_directories(cache, ec);
    std::map<FileId, Resolution::Source> encrypted;
    for (const auto &[file, source] : quests) {
        const std::optional<std::uint32_t> id = p3rd::quest_of_file(file);
        std::ifstream in(source.path, std::ios::binary);
        const std::vector<std::uint8_t> payload{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        const std::optional<game::QuestRecord> record = game::read_quest_record(payload);
        if (!id || !record || record->id != *id || payload.size() > game::kQuestMaxPayload) {
            std::cerr << "[mods] " << source.mod << ": " << to_utf8(source.path.filename())
                      << " is not the quest it was read as; it is not served\n";
            continue;
        }
        const fs::path path = cache / (std::to_string(*id) + ".bin");
        const std::vector<std::uint8_t> bytes = game::encrypt_quest(payload, *keys);
        std::ofstream(path, std::ios::binary | std::ios::trunc)
            .write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) != bytes.size()) {
            std::cerr << "[mods] quest " << *id << " cannot be written to " << to_utf8(cache) << "\n";
            continue;
        }
        encrypted[file] = {source.mod, path, {}};
        s.files[*id] = file;
        s.served.push_back({*id, record->name, record->fee, record->frames, record->stars, source.mod});
    }
    if (encrypted.empty()) return false;
    // The hooks go through the mods' own hooks, as a host-owned mod; at the
    // counter, among the event quests.
    if (!attach(runtime)) {
        s = {};
        return false;
    }
    files.insert(encrypted.begin(), encrypted.end());
    std::cout << "[mods] " << encrypted.size() << " quest(s) served\n";
    return true;
}

void rebuild_later(std::uint32_t id, Rebuild build) {
    state().rebuilds[id] = std::move(build);
}

namespace {

// Builds the quests named to rebuild_later() and serves the new payloads.
void rebuild(psprecomp::Runtime &runtime) {
    State &s = state();
    game::GuestRam ram(runtime.memory());
    const std::optional<game::QuestKeys> keys = game::read_quest_keys(ram);
    for (auto &[id, build] : s.rebuilds) {
        const auto file = s.files.find(id);
        std::string why;
        const std::optional<std::vector<std::uint8_t>> payload = build(runtime, &why);
        if (file == s.files.end() || !keys || !payload || payload->size() > game::kQuestMaxPayload ||
            !added::replace(file->second, game::encrypt_quest(*payload, *keys))) {
            std::cerr << "[mods] quest " << id << " is not built again" << (why.empty() ? "" : ": " + why)
                      << "; it is served as it was first built\n";
            continue;
        }
        std::cout << "[mods] quest " << id << " is built again with the game's text\n";
    }
    s.rebuilds.clear();
}

} // namespace

void overlay_loaded(psprecomp::Runtime &runtime) {
    State &s = state();
    if (!s.rebuilds.empty()) {
        const std::optional<LoadedOverlay> lobby = overlay_at(runtime.memory(), kEventsGame);
        if (lobby && iequals(lobby->name, "lobby_task")) rebuild(runtime);
    }
    if (!s.events) return;
    const std::optional<LoadedOverlay> overlay = overlay_at(runtime.memory(), kEventsGame);
    const std::optional<tables::Placement> list = tables::placement(kEvents);
    if (!overlay || !iequals(overlay->name, "lobby_task") || !list || list->address == kEventsGame) return;
    auto &memory = runtime.memory();
    if (!memory.contains(list->address, list->rows * 2u)) return;
    // After the game's own, once: they are there again if the copy was
    // filled before, and the list keeps them across loads.
    std::uint32_t row = 0u;
    while (row < list->rows && memory.load16(list->address + row * 2u) != 0u) {
        if (s.files.contains(memory.load16(list->address + row * 2u))) return;
        ++row;
    }
    for (const auto &[quest, file] : s.files) {
        if (row + 1u >= list->rows) break;
        memory.store16(list->address + row++ * 2u, static_cast<std::uint16_t>(quest));
    }
    memory.store16(list->address + row * 2u, 0u);
}

const std::vector<Quest> &served() {
    return state().served;
}

const Quest *find(std::uint32_t id) {
    for (const Quest &quest : state().served)
        if (quest.id == id) return &quest;
    return nullptr;
}

void reset_for_tests() {
    state() = {};
}

} // namespace mhp3rd::mods::quests
