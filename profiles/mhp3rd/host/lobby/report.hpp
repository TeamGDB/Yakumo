#pragma once

// The lobby report: what the client sends to a lobby dashboard, and when. The
// dashboard (a separate service, see the profile README) shows who is in which
// gathering hall with the hunter's name and current equipment. The client
// reads its own data from guest memory and sends it over a plain TCP line
// protocol, so nothing here depends on the game's own network packets.
//
// This header is pure data and decision logic: the unit tests run it without
// the game, a socket or the settings.

#include "lobby/quests.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mhp3rd::lobby {

// One worn armor piece. `slot` is "head", "chest", "arms", "waist" or "legs";
// `name` is the game's name for the piece, empty while its text block is not
// loaded.
struct Piece {
    std::string slot;
    std::uint16_t id{};
    std::string name;

    [[nodiscard]] bool operator==(const Piece &other) const {
        return slot == other.slot && id == other.id && name == other.name;
    }
};

// The carried weapon, in the equipment kind the game uses (5 and up).
struct Weapon {
    std::uint8_t kind{};
    std::uint16_t id{};
    std::string name;

    [[nodiscard]] bool operator==(const Weapon &other) const {
        return kind == other.kind && id == other.id && name == other.name;
    }
};

// What the client knows about its own hunter at one moment. `hall` is empty
// while the player is not in a gathering hall, which is what ends a report
// session.
struct Snapshot {
    std::string mac;        // the virtual MAC the server knows this player by
    std::string nickname;   // the name other players see
    std::string product;    // the game's product code, for example ULJM05800
    std::string server;     // the ad hoc server in use, as configured
    std::string hall;       // group name, for example MHP3Q000
    std::string name;       // the hunter's name
    std::string sex;        // "male" or "female"
    int hr{};               // the hunter rank; 0 while it is not read yet
    std::string title;      // the guild card title; empty while not read yet
    std::string guild_card; // the guild card, when one is read; empty otherwise
    std::string token;      // the dashboard's shared secret, when one is set
    std::vector<Piece> equipment;
    std::optional<Weapon> weapon;
    std::vector<PostedQuest> quests;

    [[nodiscard]] bool operator==(const Snapshot &other) const {
        return mac == other.mac && nickname == other.nickname && product == other.product && server == other.server &&
            hall == other.hall && name == other.name && sex == other.sex && hr == other.hr && title == other.title &&
            guild_card == other.guild_card && token == other.token && equipment == other.equipment &&
            weapon == other.weapon && quests == other.quests;
    }
    [[nodiscard]] bool operator!=(const Snapshot &other) const { return !(*this == other); }
};

// One report line for the dashboard, ending in '\n'.
[[nodiscard]] std::string report_line(const Snapshot &snapshot, std::int64_t unix_seconds);
// The line that tells the dashboard the player left the hall.
[[nodiscard]] std::string leave_line(const std::string &mac, const std::string &token, std::int64_t unix_seconds);
// A JSON string literal, quotes included, with the escapes JSON requires.
[[nodiscard]] std::string json_string(const std::string &text);

// Decides which lines to send. `update` is called about once a second with the
// current state and returns the lines to write, so the caller owns the socket
// and the tests own the clock.
class Planner {
public:
    explicit Planner(std::int64_t heartbeat_seconds = 30) : heartbeat_seconds_(heartbeat_seconds) {}

    // A report when the player enters a hall, when the hall or the hunter's
    // data changes, or every heartbeat while in one; a leave line when the
    // hall ends. Nothing while nothing changed.
    [[nodiscard]] std::vector<std::string> update(const Snapshot &snapshot, std::int64_t unix_seconds);
    void reset();

private:
    std::optional<Snapshot> last_;
    std::int64_t last_sent_{};
    std::int64_t heartbeat_seconds_{30};
};

} // namespace mhp3rd::lobby
