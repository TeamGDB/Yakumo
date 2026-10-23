#include "lobby/report.hpp"

#include <cstdio>

namespace mhp3rd::lobby {

std::string json_string(const std::string &text) {
    std::string out;
    out.push_back('"');
    for (const unsigned char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20u) {
                char escape[7];
                std::snprintf(escape, sizeof(escape), "\\u%04x", c);
                out += escape;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
    return out;
}

namespace {

void append_string_field(std::string &out, const char *name, const std::string &value) {
    if (value.empty()) return;
    out += ",\"";
    out += name;
    out += "\":";
    out += json_string(value);
}

void append_counts(std::string &out, const char *name, const std::vector<StatCount> &counts, bool &first) {
    if (counts.empty()) return;
    if (!first) out.push_back(',');
    first = false;
    out += "\"";
    out += name;
    out += "\":[";
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (i > 0u) out.push_back(',');
        out += "{\"label\":" + json_string(counts[i].label) + ",\"count\":" + std::to_string(counts[i].count) + "}";
    }
    out.push_back(']');
}

} // namespace

std::string report_line(const Snapshot &snapshot, std::int64_t unix_seconds) {
    std::string out = "{\"v\":1,\"type\":\"report\",\"full\":true";
    append_string_field(out, "mac", snapshot.mac);
    append_string_field(out, "nickname", snapshot.nickname);
    append_string_field(out, "product", snapshot.product);
    append_string_field(out, "server", snapshot.server);
    append_string_field(out, "hall", snapshot.hall);
    append_string_field(out, "name", snapshot.name);
    append_string_field(out, "sex", snapshot.sex);
    if (snapshot.hr > 0) out += ",\"hr\":" + std::to_string(snapshot.hr);
    append_string_field(out, "title", snapshot.title);
    append_string_field(out, "guild_card", snapshot.guild_card);
    append_string_field(out, "token", snapshot.token);
    if (!snapshot.equipment.empty()) {
        out += ",\"equipment\":[";
        for (std::size_t i = 0; i < snapshot.equipment.size(); ++i) {
            const Piece &piece = snapshot.equipment[i];
            if (i > 0u) out.push_back(',');
            out += "{\"slot\":" + json_string(piece.slot) + ",\"id\":" + std::to_string(piece.id);
            append_string_field(out, "name", piece.name);
            out.push_back('}');
        }
        out.push_back(']');
    }
    if (snapshot.weapon) {
        out += ",\"weapon\":{\"kind\":" + std::to_string(snapshot.weapon->kind) +
            ",\"id\":" + std::to_string(snapshot.weapon->id);
        append_string_field(out, "name", snapshot.weapon->name);
        append_string_field(out, "type", snapshot.weapon->type);
        out.push_back('}');
    }
    if (!snapshot.quests.empty()) {
        out += ",\"quests\":[";
        for (std::size_t i = 0; i < snapshot.quests.size(); ++i) {
            const PostedQuest &quest = snapshot.quests[i];
            if (i > 0u) out.push_back(',');
            out += "{\"slot\":" + std::to_string(quest.slot) + ",\"id\":" + std::to_string(quest.id) +
                ",\"stars\":" + std::to_string(quest.stars) + ",\"fee\":" + std::to_string(quest.fee) +
                ",\"reward\":" + std::to_string(quest.reward) + ",\"time_limit\":" + std::to_string(quest.time_limit);
            append_string_field(out, "name", quest.name);
            append_string_field(out, "objective", quest.objective);
            append_string_field(out, "monsters", quest.monsters);
            append_string_field(out, "client", quest.client);
            append_string_field(out, "poster", quest.poster);
            append_string_field(out, "accepted", quest.accepted);
            append_string_field(out, "password", quest.password);
            append_string_field(out, "note", quest.note);
            append_string_field(out, "started_at", quest.started_at);
            out.push_back('}');
        }
        out.push_back(']');
    }
    if (!snapshot.stats.empty()) {
        out += ",\"stats\":{";
        bool first = true;
        append_counts(out, "quests", snapshot.stats.quests, first);
        if (snapshot.stats.fs_level > 0u) {
            if (!first) out.push_back(',');
            first = false;
            out += "\"fs_level\":" + std::to_string(snapshot.stats.fs_level);
        }
        if (snapshot.stats.play_seconds > 0u) {
            if (!first) out.push_back(',');
            first = false;
            out += "\"play_seconds\":" + std::to_string(snapshot.stats.play_seconds);
        }
        append_counts(out, "weapons", snapshot.stats.weapons, first);
        if (!snapshot.stats.recent_quests.empty()) {
            if (!first) out.push_back(',');
            first = false;
            out += "\"recent_quests\":[";
            for (std::size_t i = 0; i < snapshot.stats.recent_quests.size(); ++i) {
                if (i > 0u) out.push_back(',');
                out += json_string(snapshot.stats.recent_quests[i]);
            }
            out.push_back(']');
        }
        if (snapshot.stats.awards > 0u) {
            if (!first) out.push_back(',');
            first = false;
            out += "\"awards\":" + std::to_string(snapshot.stats.awards);
        }
        out.push_back('}');
    }
    out += ",\"ts\":" + std::to_string(unix_seconds) + "}\n";
    return out;
}

std::string leave_line(const std::string &mac, const std::string &token, std::int64_t unix_seconds) {
    std::string out = "{\"v\":1,\"type\":\"leave\"";
    append_string_field(out, "mac", mac);
    append_string_field(out, "token", token);
    out += ",\"ts\":" + std::to_string(unix_seconds) + "}\n";
    return out;
}

std::vector<std::string> Planner::update(const Snapshot &snapshot, std::int64_t unix_seconds) {
    std::vector<std::string> lines;
    if (snapshot.hall.empty()) {
        if (last_ && !last_->hall.empty()) {
            const std::string &mac = !snapshot.mac.empty() ? snapshot.mac : last_->mac;
            const std::string &token = !snapshot.token.empty() ? snapshot.token : last_->token;
            if (!mac.empty()) lines.push_back(leave_line(mac, token, unix_seconds));
            last_.reset();
        }
        return lines;
    }
    if (!last_ || last_->hall != snapshot.hall || *last_ != snapshot) {
        lines.push_back(report_line(snapshot, unix_seconds));
        last_ = snapshot;
        last_sent_ = unix_seconds;
        return lines;
    }
    if (unix_seconds - last_sent_ >= heartbeat_seconds_) {
        lines.push_back(report_line(snapshot, unix_seconds));
        last_sent_ = unix_seconds;
    }
    return lines;
}

void Planner::reset() {
    last_.reset();
    last_sent_ = 0;
}

} // namespace mhp3rd::lobby
