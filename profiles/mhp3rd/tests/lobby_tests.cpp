// The lobby report (lobby/report.hpp): the JSON sent to the dashboard, when it
// is sent, and the hunter's own data read from a buffer standing for guest
// memory. No game data, no socket and no game run.
#include "lobby/quests.hpp"
#include "lobby/report.hpp"
#include "lobby/snapshot.hpp"

#include "game/equipment_models.hpp"
#include "game/game_data.hpp"
#include "game/guest_ram.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace mhp3rd::lobby {
// The app reads the quest details from DATA.BIN; the tests below do not need
// them, so the test binary provides the empty definition.
std::vector<PostedQuest> posted_quests(const game::Ram &) {
    return {};
}
} // namespace mhp3rd::lobby

namespace {
using namespace mhp3rd;

int failures{};

void check(bool condition, const char *message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool has(const std::string &text, const std::string &part) {
    return text.find(part) != std::string::npos;
}

void test_json_string() {
    check(lobby::json_string("plain") == "\"plain\"", "a plain string is quoted");
    check(lobby::json_string("a\"b\\c") == "\"a\\\"b\\\\c\"", "quotes and backslashes are escaped");
    check(lobby::json_string("a\nb\tc") == "\"a\\nb\\tc\"", "newlines and tabs are escaped");
    check(lobby::json_string(std::string(1u, '\x01')) == "\"\\u0001\"", "control characters are escaped");
    check(lobby::json_string("Jos\xC3\xA9") == "\"Jos\xC3\xA9\"", "UTF-8 passes through");
}

lobby::Snapshot sample() {
    lobby::Snapshot snapshot;
    snapshot.mac = "02:00:00:00:00:01";
    snapshot.nickname = "Alpha";
    snapshot.product = "ULJM05800";
    snapshot.server = "example.invalid:27312";
    snapshot.hall = "MHP3Q000";
    snapshot.name = "Tester";
    snapshot.sex = "male";
    snapshot.hr = 7;
    snapshot.title = "Village Hero";
    snapshot.token = "secret";
    snapshot.equipment = {{"head", 12u, "Hunter Helm"}};
    snapshot.weapon = lobby::Weapon{5u, 3u, "Iron Sword", "Great Sword"};
    snapshot.quests = {{1u, 10522u, 5u, 800u, 9600u, 36000u, "The Festival of Fear", "Repel the Jhen Mohran",
        "Jhen Mohran", "Guildmaster"}};
    return snapshot;
}

void test_report_line() {
    const std::string line = lobby::report_line(sample(), 1000);
    check(line.back() == '\n', "the report line ends in a newline");
    check(has(line, "\"type\":\"report\""), "the report names its type");
    check(has(line, "\"full\":true"), "the report is a full snapshot");
    check(has(line, "\"mac\":\"02:00:00:00:00:01\""), "the report carries the MAC");
    check(has(line, "\"hall\":\"MHP3Q000\""), "the report carries the hall");
    check(has(line, "\"name\":\"Tester\""), "the report carries the hunter name");
    check(has(line, "\"hr\":7"), "the report carries the hunter rank");
    check(has(line, "\"title\":\"Village Hero\""), "the report carries the title");
    check(has(line, "\"token\":\"secret\""), "the report carries the shared token");
    check(has(line, "\"equipment\":[{\"slot\":\"head\",\"id\":12,\"name\":\"Hunter Helm\"}]"),
        "the report carries the worn armor");
    check(has(line, "\"weapon\":{\"kind\":5,\"id\":3,\"name\":\"Iron Sword\",\"type\":\"Great Sword\"}"),
        "the report carries the weapon and its class");
    check(has(line,
              "\"quests\":[{\"slot\":1,\"id\":10522,\"stars\":5,\"fee\":800,\"reward\":9600,\"time_limit\":36000,"
              "\"name\":\"The Festival of Fear\",\"objective\":\"Repel the Jhen Mohran\",\"monsters\":\"Jhen Mohran\","
              "\"client\":\"Guildmaster\"}]"),
        "the report carries the posted quests");
    check(has(line, "\"ts\":1000"), "the report carries the time");

    lobby::Snapshot empty;
    empty.mac = "02:00:00:00:00:02";
    const std::string short_line = lobby::report_line(empty, 1001);
    check(!has(short_line, "\"hr\""), "an unread rank is left out");
    check(!has(short_line, "\"equipment\""), "no equipment is left out");
    check(has(short_line, "\"type\":\"report\""), "an empty report is still a report");
}

void test_leave_line() {
    const std::string line = lobby::leave_line("02:00:00:00:00:01", "secret", 1000);
    check(has(line, "\"type\":\"leave\""), "the leave names its type");
    check(has(line, "\"mac\":\"02:00:00:00:00:01\""), "the leave carries the MAC");
    check(has(line, "\"token\":\"secret\""), "the leave carries the shared token");
    check(line.back() == '\n', "the leave line ends in a newline");
}

void test_planner() {
    lobby::Planner planner(30);
    lobby::Snapshot snapshot = sample();
    snapshot.hall.clear();
    check(planner.update(snapshot, 1000).empty(), "nothing is sent while not in a hall");

    snapshot.hall = "MHP3Q000";
    std::vector<std::string> lines = planner.update(snapshot, 1000);
    check(lines.size() == 1u && has(lines[0], "\"type\":\"report\""), "entering a hall reports once");
    check(planner.update(snapshot, 1005).empty(), "nothing changes while the state is the same");

    lines = planner.update(snapshot, 1031);
    check(lines.size() == 1u && has(lines[0], "\"type\":\"report\""), "a heartbeat reports again");

    snapshot.name = "Other";
    lines = planner.update(snapshot, 1032);
    check(lines.size() == 1u && has(lines[0], "\"name\":\"Other\""), "a changed hunter reports at once");

    lobby::Snapshot left = snapshot;
    left.hall.clear();
    lines = planner.update(left, 1033);
    check(lines.size() == 1u && has(lines[0], "\"type\":\"leave\""), "leaving a hall sends a leave");
    check(planner.update(left, 1034).empty(), "leaving twice sends nothing");
}

constexpr std::uint32_t kBase = 0x09F40000u;
constexpr std::size_t kSize = 0x00090000u; // up to 0x09FD0000, including the rank byte

void test_read_hunter() {
    game::BufferRam ram(kBase, kSize);
    const std::uint32_t character = game::kCharacter;
    // The name, as the game keeps it: UTF-16 code units, zero-terminated.
    const char *name = "Tester";
    for (std::size_t i = 0; i <= std::char_traits<char>::length(name); ++i)
        ram.store16(character + static_cast<std::uint32_t>(i * 2u), static_cast<std::uint16_t>(name[i]));
    ram.store8(character + game::kCharacterSex, 0u); // male
    // The worn head piece (kind 4), in the character's armor records.
    const std::uint32_t head = character + game::kCharacterArmor + 4u * game::kEquipmentRecord;
    ram.store8(head, 1u);
    ram.store8(head + 1u, 4u);
    ram.store16(head + 2u, 12u);
    // The carried weapon, in the character's weapon record.
    const std::uint32_t weapon = character + game::kCharacterWeapon;
    ram.store8(weapon, 1u);
    ram.store8(weapon + 1u, 5u);
    ram.store16(weapon + 2u, 3u);
    ram.store8(character + 0x8Du, 7u); // hunter rank

    lobby::Snapshot snapshot;
    lobby::read_hunter(ram, snapshot);
    check(snapshot.name == "Tester", "the hunter's name is read");
    check(snapshot.sex == "male", "the hunter's sex is read");
    check(snapshot.hr == 7, "the hunter rank is read");
    check(snapshot.equipment.size() == 1u && snapshot.equipment[0].slot == "head" && snapshot.equipment[0].id == 12u,
        "the worn head piece is read");
    check(snapshot.weapon && snapshot.weapon->kind == 5u && snapshot.weapon->id == 3u &&
            snapshot.weapon->type == "Great Sword",
        "the weapon and its class are read");

    game::BufferRam empty(kBase, kSize);
    lobby::Snapshot none;
    lobby::read_hunter(empty, none);
    check(none.name.empty() && none.equipment.empty() && !none.weapon && none.hr == 0, "no character reads as no data");
}

void write_utf16(game::Ram &ram, std::uint32_t at, const char *text) {
    for (std::size_t i = 0; text[i] != '\0'; ++i)
        ram.store16(at + static_cast<std::uint32_t>(i * 2u), static_cast<std::uint16_t>(text[i]));
}

void test_hunter_rank_and_guild_card() {
    game::BufferRam character(kBase, kSize);
    character.store8(game::kCharacter + 0x8Du, 7u);
    check(lobby::hunter_rank(character) == 7u, "the hunter rank is read from the character block");
    check(lobby::hunter_rank(game::BufferRam(kBase, 0x100u)) == 0u, "a missing rank byte reads as rank 0");

    game::BufferRam cards(lobby::kGuildCardListBase, lobby::kGuildCardListBytes);
    const std::uint32_t own_card = lobby::kGuildCardListBase + 0x0DBAu;
    write_utf16(cards, own_card, "Tester");
    write_utf16(cards, own_card + 0x54u, "Hero");
    write_utf16(cards, own_card + 0x6Eu, "Hello");
    std::string title;
    std::string comment;
    lobby::guild_card(cards, "Tester", title, comment);
    check(title == "Hero" && comment == "Hello", "the guild card title and comment are read");
    lobby::guild_card(cards, "Nobody", title, comment);
    check(title.empty() && comment.empty(), "another name reads no guild card");
}

void test_posted_quest_ids() {
    game::BufferRam board(lobby::kQuestBoardBase, lobby::kQuestBoardBytes);
    board.store16(lobby::kQuestBoardBase + 0x00u, 10101u);
    board.store16(lobby::kQuestBoardBase + 0x02u, 10102u);
    board.store16(lobby::kQuestBoardBase + 0x04u, 10101u); // duplicate
    board.store16(lobby::kQuestBoardBase + 0x06u, 10500u);
    board.store16(lobby::kQuestBoardBase + 0x08u, 10501u);
    const std::vector<std::uint16_t> ids = lobby::posted_quest_ids(board);
    check(ids.size() == 4u && ids[0] == 10101u && ids[1] == 10102u && ids[2] == 10500u && ids[3] == 10501u,
        "the board's ids are read in order, without duplicates");
    check(lobby::posted_quest_ids(game::BufferRam(lobby::kQuestBoardBase, 0x100u)).empty(),
        "a missing board reads no quests");
}

void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void test_diagnostic_dump() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "yakumo-lobby-diag-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    set_env("MHP3RD_LOBBY_DUMP", root.string().c_str());
    set_env("MHP3RD_LOBBY_DUMP_TEXT", "Zinogre");

    game::BufferRam ram(kBase, kSize);
    write_utf16(ram, game::kCharacter, "Tester");
    const char *text = "Zinogre";
    for (std::size_t i = 0u; text[i] != '\0'; ++i)
        ram.store8(lobby::kQuestBoardBase + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(text[i]));
    lobby::Snapshot snapshot;
    snapshot.hall = "MHP3QTEST";
    lobby::read_hunter(ram, snapshot);

    std::filesystem::path dump;
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
        if (entry.is_directory()) {
            dump = entry.path();
            break;
        }
    }
    check(!dump.empty(), "a diagnostic dump folder is written");
    if (!dump.empty()) {
        check(std::filesystem::exists(dump / "summary.txt"), "the dump has a summary");
        check(std::filesystem::exists(dump / "character.bin"), "the dump has the character block");
        check(std::filesystem::exists(dump / "name_hits.txt"), "the dump has the name hits");
        check(std::filesystem::exists(dump / "text_hits.txt"), "the dump has the text hits");
        std::ifstream summary(dump / "summary.txt");
        const std::string text((std::istreambuf_iterator<char>(summary)), std::istreambuf_iterator<char>());
        check(has(text, "name: Tester"), "the summary names the hunter");
        check(has(text, "Zinogre"), "the summary mentions the searched text");
    }

    set_env("MHP3RD_LOBBY_DUMP", "");
    set_env("MHP3RD_LOBBY_DUMP_TEXT", "");
    std::filesystem::remove_all(root, error);
}

} // namespace

int main() {
    test_json_string();
    test_report_line();
    test_leave_line();
    test_planner();
    test_read_hunter();
    test_hunter_rank_and_guild_card();
    test_posted_quest_ids();
    test_diagnostic_dump();
    if (failures == 0) std::cout << "lobby tests passed\n";
    return failures == 0 ? 0 : 1;
}
