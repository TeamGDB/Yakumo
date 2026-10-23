// The lobby report (lobby/report.hpp): the JSON sent to the dashboard, when it
// is sent, and the hunter's own data read from a buffer standing for guest
// memory. No game data, no socket and no game run.
#include "lobby/quest_file.hpp"
#include "lobby/quests.hpp"
#include "lobby/report.hpp"
#include "lobby/reporter.hpp"
#include "lobby/snapshot.hpp"

#include "adhoc/sockets.hpp"
#include "game/equipment_models.hpp"
#include "game/game_data.hpp"
#include "game/guest_ram.hpp"
#include "settings/settings.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mhp3rd::lobby {
// The app reads the quest details from DATA.BIN; the test binary supplies the
// quests its snapshot tests need.
std::vector<PostedQuest> test_posted_quests;
std::vector<PostedQuest> posted_quests(const game::Ram &) {
    return test_posted_quests;
}
} // namespace mhp3rd::lobby

namespace mhp3rd::settings {
// The reporter only tests its network thread here; frame() is not entered.
Settings &current() {
    static Settings settings;
    return settings;
}
} // namespace mhp3rd::settings

namespace {
using namespace mhp3rd;
namespace net = mhp3rd::adhoc::net;

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
    check(lobby::json_string("\b\f\r") == "\"\\b\\f\\r\"", "backspace, form feed and carriage return are escaped");
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
    snapshot.quests = {{1u, 10522u, 5u, 3u, 800u, 9600u, 36000u, "Great Desert", "The Festival of Fear",
        "Repel the Jhen Mohran", "Jhen Mohran", "Guildmaster"}};
    snapshot.stats.quests = {{"Village", 12u}, {"Low Rank", 34u}};
    snapshot.stats.fs_level = 23u;
    snapshot.stats.play_seconds = 3661u;
    snapshot.stats.weapons = {{"Sword and Shield", 10u}, {"Lance", 4u}};
    snapshot.stats.recent_quests = {"The Festival of Fear"};
    snapshot.stats.awards = 2u;
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
              "\"quests\":[{\"slot\":1,\"id\":10522,\"stars\":5,\"min_hr\":3,\"fee\":800,\"reward\":9600,"
              "\"time_limit\":36000,\"zone\":\"Great Desert\",\"name\":\"The Festival of Fear\","
              "\"objective\":\"Repel the Jhen Mohran\",\"monsters\":\"Jhen Mohran\",\"client\":\"Guildmaster\"}]"),
        "the report carries the posted quests");
    check(has(line,
              "\"stats\":{\"quests\":[{\"label\":\"Village\",\"count\":12},{\"label\":\"Low Rank\",\"count\":34}],"
              "\"fs_level\":23,\"play_seconds\":3661,\"weapons\":[{\"label\":\"Sword and Shield\",\"count\":10},"
              "{\"label\":\"Lance\",\"count\":4}],\"recent_quests\":[\"The Festival of Fear\"],\"awards\":2}"),
        "the report carries the guild card summary");
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
    planner.reset();
    lines = planner.update(snapshot, 1035);
    check(lines.size() == 1u, "a reset planner sends the next snapshot at once");
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

void write_utf16(game::Ram &ram, std::uint32_t at, const char *text);

void test_snapshot_pieces_and_quest_time() {
    game::BufferRam ram(kBase, kSize);
    write_utf16(ram, game::kCharacter, "Tester");
    ram.store8(game::kCharacter + game::kCharacterSex, 0u);
    for (std::uint8_t kind = 0u; kind <= 4u; ++kind) {
        const std::uint32_t piece = game::kCharacter + game::kCharacterArmor + kind * game::kEquipmentRecord;
        ram.store8(piece, 1u);
        ram.store8(piece + 1u, kind);
        ram.store16(piece + 2u, static_cast<std::uint16_t>(kind + 1u));
    }
    lobby::Snapshot snapshot;
    snapshot.hall = "MHP3QTEST";
    lobby::read_hunter(ram, snapshot);
    check(snapshot.equipment.size() == 5u && snapshot.equipment[0].slot == "chest" &&
            snapshot.equipment[1].slot == "arms" && snapshot.equipment[2].slot == "waist" &&
            snapshot.equipment[3].slot == "legs" && snapshot.equipment[4].slot == "head",
        "every armor slot gets its dashboard label");
    lobby::test_posted_quests = {{1u, 10509u, 5u, 3u, 500u, 6000u, 90'000u, "Deserted Island (Night)", "Warrior's Hunt",
        "Hunt the Zinogre", "Ludroth\nJaggia", "Guild Manager"}};
    lobby::read_hunter(ram, snapshot);
    check(
        snapshot.quests.size() == 1u && !snapshot.quests[0].started_at.empty() && snapshot.quests[0].poster == "Tester",
        "the snapshot dates the quest and names its poster");
    lobby::test_posted_quests.clear();
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

void write_ascii(game::Ram &ram, std::uint32_t at, const char *text) {
    for (std::size_t i = 0; text[i] != '\0'; ++i)
        ram.store8(at + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(text[i]));
}

void test_quest_file_objective_across_padding() {
    std::vector<std::uint8_t> file(8u, 0u);
    const auto put32 = [&](std::size_t at, std::uint32_t value) {
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            file[at + byte] = static_cast<std::uint8_t>(value >> (8u * byte));
    };
    put32(0u, 8u); // one record, starting after the offset list
    file.resize(8u + 0x48u, 0u);
    put32(8u + 4u, 500u);
    put32(8u + 8u, 8196u);
    put32(8u + 0x10u, 90'000u);
    file[8u + 0x1Cu] = 0x0Du;
    file[8u + 0x1Du] = 0x29u; // quest 10509
    file[8u + 0x1Eu] = 5u;
    file[8u + 0x20u] = 0x0Eu; // Deserted Island at night
    for (const char *text : {"Warrior's Hunt", "Hunt the Zinogre", "Reward Zero\nTime Over",
             "The thunder wyvern waits on the island.", "Ludroth\nJaggia", "Guild Manager"}) {
        for (const char *c = text; *c != '\0'; ++c) file.push_back(static_cast<std::uint8_t>(*c));
        file.push_back(0u);
        while (file.size() % 4u != 0u) file.push_back(0u);
    }
    file.insert(file.end(), {0x90u, 0x01u, 0u, 0u});
    const std::vector<lobby::QuestFileEntry> quests = lobby::parse_quest_file(file);
    check(quests.size() == 1u, "a quest list record is parsed");
    check(quests.size() == 1u && quests[0].id == 10509u && quests[0].stars == 5u, "the record's id and stars are read");
    check(quests.size() == 1u && quests[0].min_hr == 3u, "the star level gives the minimum hunter rank");
    check(quests.size() == 1u && quests[0].zone == "Deserted Island (Night)", "the map byte gives the quest zone");
    check(quests.size() == 1u && quests[0].name == "Warrior's Hunt", "the title is read from before the padded gap");
    check(quests.size() == 1u && quests[0].objective == "Hunt the Zinogre",
        "the objective is read past the padding between fields");
    check(quests.size() == 1u && quests[0].monsters == "Ludroth\nJaggia" && quests[0].client == "Guild Manager",
        "the later fields are read past their padding");
}

void test_quest_zones_and_ranks() {
    const char *const zones[] = {"Unknown", "Deserted Island (Day)", "Sandy Plains (Day)", "Flooded Forest (Day)",
        "Tundra (Day)", "Volcano (Day)", "Great Desert", "Lava Canyon", "Land Arena", "Small Arena", "Sacred Land",
        "Polar Zone", "Misty Peaks (Day)", "Mountain Summit", "Deserted Island (Night)", "Sandy Plains (Night)",
        "Flooded Forest (Night)", "Tundra (Night)", "Volcano (Night)", "Misty Peaks (Night)"};
    for (std::uint8_t id = 0u; id < 20u; ++id) check(lobby::zone_name(id) == zones[id], "a quest map has its name");
    check(lobby::zone_name(0xFFu) == "Unknown", "an unknown quest map falls back");
    check(lobby::minimum_rank(1u) == 1u && lobby::minimum_rank(2u) == 1u, "one and two stars need HR1");
    check(lobby::minimum_rank(3u) == 2u && lobby::minimum_rank(4u) == 2u, "three and four stars need HR2");
    check(lobby::minimum_rank(5u) == 3u && lobby::minimum_rank(6u) == 4u, "five and six stars need HR3 and HR4");
    check(lobby::minimum_rank(7u) == 5u && lobby::minimum_rank(8u) == 6u, "seven and eight stars need HR5 and HR6");
    check(lobby::minimum_rank(9u) == 6u, "a high star value keeps the highest gate");
}

void test_active_quest_id() {
    game::BufferRam record(lobby::kActiveQuestRecord, 0x100u);
    record.store16(lobby::kActiveQuestRecord + lobby::kActiveQuestIdOffset, 10509u);
    check(lobby::active_quest_id(record) == 10509u, "the board's selected quest id is read");
    check(lobby::active_quest_id(game::BufferRam(lobby::kActiveQuestRecord, 0x100u)) == 0u,
        "no selected record reads no active quest");
}

void test_latest_quest_id() {
    game::BufferRam board(lobby::kQuestBoardBase, lobby::kQuestBoardBytes);
    board.store16(lobby::kQuestBoardBase + 0x00u, 10101u);
    board.store16(lobby::kQuestBoardBase + 0x02u, 10522u);
    board.store16(lobby::kQuestBoardBase + 0x04u, 10101u); // duplicate
    board.store16(lobby::kQuestBoardBase + 0x06u, 10510u);
    check(lobby::latest_quest_id(board) == 10510u, "the last new quest id is the active one");
    check(lobby::latest_quest_id(game::BufferRam(lobby::kQuestBoardBase, 0x100u)) == 0u,
        "a missing board has no active quest");
}

void test_guild_stats() {
    game::BufferRam ram(kBase, kSize);
    const std::uint16_t counts[] = {12u, 34u, 56u, 7u, 8u};
    for (std::size_t i = 0u; i < 5u; ++i)
        ram.store16(lobby::kQuestCountsBase + static_cast<std::uint32_t>(i * 2u), counts[i]);
    ram.store32(lobby::kPlayTime, 3661u); // rounded down to 3660 by the reader
    ram.store16(lobby::kWeaponUsageBase + (5u - 5u) * 2u, 2u);
    ram.store16(lobby::kWeaponUsageBase + (6u - 5u) * 2u, 10u);
    ram.store16(lobby::kWeaponUsageBase + (16u - 5u) * 2u, 4u);
    const std::uint8_t levels[] = {6u, 6u, 3u, 4u, 4u};
    for (std::size_t i = 0u; i < 5u; ++i)
        ram.store8(lobby::kFelyneLevelsBase + static_cast<std::uint32_t>(i) * lobby::kFelyneLevelStride, levels[i]);
    write_ascii(ram, lobby::kRecentQuestsBase + 0x100u, "Hunting x2 Arzuros is Hard");
    write_ascii(ram, lobby::kRecentQuestsBase + 0x200u, "The Festival of Fear");

    lobby::GuildStats stats;
    lobby::guild_stats(ram, stats);
    check(stats.quests.size() == 5u && stats.quests[1].label == "Low Rank" && stats.quests[1].count == 34u,
        "the guild card quest counts are read");
    check(stats.fs_level == 23u, "the FS level sums the felyne levels");
    check(stats.play_seconds == 3660u, "the play time is read to the minute");
    check(stats.weapons.size() == 3u && stats.weapons[0].label == "Sword and Shield" && stats.weapons[0].count == 10u &&
            stats.weapons[1].label == "Dual Blades" && stats.weapons[1].count == 4u,
        "the weapon usage is read and sorted");
    check(stats.awards == 0u, "the award count is left unset until it is traced");
    check(stats.recent_quests.size() == 2u && stats.recent_quests[0] == "The Festival of Fear",
        "the recent quests are read newest first");
}

void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

bool wait_until(const std::function<bool()> &ready, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return ready();
}

// A loopback TCP server that keeps what the reporter sends. It exists only to
// exercise the network thread without a game or the ad hoc server.
struct TestServer {
    net::Socket listener{net::kNoSocket};
    std::uint16_t port{};
    std::atomic<bool> stop{};
    std::mutex mutex;
    std::string received;
    std::thread thread;

    TestServer() {
        listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == net::kNoSocket) return;
        net::set_option(listener, SOL_SOCKET, SO_REUSEADDR, 1);
        const net::Address address = net::Address::ipv4_address(htonl(INADDR_LOOPBACK), 0);
        if (::bind(listener, reinterpret_cast<const sockaddr *>(&address.storage), address.length) != 0 ||
            ::listen(listener, 1) != 0) {
            net::close_socket(listener);
            listener = net::kNoSocket;
            return;
        }
        net::Address bound;
        socklen_t length = sizeof(bound.storage);
        if (getsockname(listener, reinterpret_cast<sockaddr *>(&bound.storage), &length) != 0) {
            net::close_socket(listener);
            listener = net::kNoSocket;
            return;
        }
        port = bound.port();
        thread = std::thread([this] {
            const net::Socket peer = ::accept(listener, nullptr, nullptr);
            if (peer == net::kNoSocket) return;
            char buffer[4096];
            for (;;) {
                const int count = ::recv(peer, buffer, static_cast<int>(sizeof(buffer)), 0);
                if (count <= 0) break;
                std::lock_guard lock(mutex);
                received.append(buffer, static_cast<std::size_t>(count));
            }
            net::close_socket(peer);
        });
    }

    ~TestServer() {
        stop = true;
        if (listener != net::kNoSocket) net::close_socket(listener);
        if (thread.joinable()) thread.join();
    }

    [[nodiscard]] bool ready() const { return listener != net::kNoSocket && port != 0u; }
    [[nodiscard]] std::string text() {
        std::lock_guard lock(mutex);
        return received;
    }
};

void test_reporter_network() {
    net::WinsockSession winsock;
    TestServer server;
    check(server.ready(), "the reporter test server listens");
    if (!server.ready()) return;

    lobby::test::configure(true, "127.0.0.1:" + std::to_string(server.port), "");
    lobby::test::queue({lobby::report_line(sample(), 1000), lobby::leave_line("02:00:00:00:00:01", "", 1001)});
    const bool sent = wait_until([&] { return lobby::status().reports_sent >= 2u; });
    if (!sent) {
        const lobby::Status current = lobby::status();
        std::cerr << "reporter: enabled=" << current.enabled << " connected=" << current.connected
                  << " sent=" << current.reports_sent << " error=" << current.last_error << '\n';
    }
    check(sent, "the reporter sends queued lines");
    const bool received = wait_until([&] {
        const std::string text = server.text();
        return text.find("\"type\":\"report\"") != std::string::npos &&
            text.find("\"type\":\"leave\"") != std::string::npos;
    });
    check(received, "the test server receives the report and leave lines");
    lobby::shutdown();

    lobby::test::configure(true, "127.0.0.1:1", "");
    lobby::test::queue({"unreachable\n"});
    check(wait_until([] { return !lobby::status().last_error.empty(); }), "a refused reporter connection is reported");
    check(lobby::status().last_error.find("cannot connect") != std::string::npos,
        "a refused connection names the connection failure");
    lobby::shutdown();

    lobby::test::configure(true, "[127.0.0.1]:1", "");
    lobby::test::queue({"unreachable\n"});
    check(wait_until([] { return !lobby::status().last_error.empty(); }), "a bracketed server is parsed");
    lobby::shutdown();

    lobby::test::configure(false, "", "");
    check(!lobby::status().enabled, "the reporter can be disabled");
    lobby::shutdown();
}

void test_diagnostic_dump() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "yakumo-lobby-diag-test";
    const std::filesystem::path previous = std::filesystem::current_path();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    std::filesystem::current_path(root, error);
    check(!error, "the diagnostic test can enter its temporary folder");
    set_env("MHP3RD_LOBBY_DUMP", "1");
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
    for (const auto &entry : std::filesystem::directory_iterator(root / "logs")) {
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
    std::filesystem::current_path(previous, error);
    std::filesystem::remove_all(root, error);
}

} // namespace

int main() {
    test_json_string();
    test_report_line();
    test_leave_line();
    test_planner();
    test_read_hunter();
    test_snapshot_pieces_and_quest_time();
    test_hunter_rank_and_guild_card();
    test_posted_quest_ids();
    test_latest_quest_id();
    test_active_quest_id();
    test_quest_zones_and_ranks();
    test_quest_file_objective_across_padding();
    test_guild_stats();
    test_reporter_network();
    test_diagnostic_dump();
    if (failures == 0) std::cout << "lobby tests passed\n";
    return failures == 0 ? 0 : 1;
}
