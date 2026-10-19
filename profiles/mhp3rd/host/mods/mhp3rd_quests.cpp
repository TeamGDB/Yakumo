#include "mods/mhp3rd_quests.hpp"

#include "mods/mhp3rd_mod_format.hpp"
#include "mods/mod_ini.hpp"
#include "mods/mod_native.hpp"

#include "game/guest_ram.hpp"
#include "game/quest_file.hpp"

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

struct State {
    std::map<std::uint32_t, FileId> files; // by quest id
    std::vector<Quest> served;
    ymod_mod *host{};
    ymod_hook hook{};
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

bool code_as_traced(psprecomp::Runtime &runtime) {
    for (const CodeWord &expected : kSignature)
        if (!runtime.memory().contains(expected.address, 4u) ||
            runtime.memory().load32(expected.address) != expected.word)
            return false;
    return true;
}

} // namespace

bool install(psprecomp::Runtime &runtime, const std::map<FileId, Resolution::Source> &quests,
    std::map<FileId, Resolution::Source> &files, const fs::path &cache) {
    if (quests.empty()) return true;
    State &s = state();
    if (!code_as_traced(runtime)) {
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
    // The hook goes through the mods' own hooks, as a host-owned mod.
    native::attach(runtime);
    if (s.host == nullptr) s.host = native::add_mod("yakumo.quests", "Quests from mods", "");
    const auto *hooks = static_cast<const ymod_hooks *>(native::host()->service(s.host, YMOD_HOOKS, 1u));
    if (s.hook == 0u && hooks != nullptr) s.hook = hooks->instruction(s.host, nullptr, kNoFile, &give_file, nullptr);
    if (s.hook == 0u) {
        std::cerr << "[mods] the quest hook cannot be installed; quests mods add are not served\n";
        s = {};
        return false;
    }
    files.insert(encrypted.begin(), encrypted.end());
    std::cout << "[mods] " << encrypted.size() << " quest(s) served\n";
    return true;
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
