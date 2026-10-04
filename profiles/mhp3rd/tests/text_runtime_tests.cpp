// Runtime contracts over synthetic archive metadata, encrypted invented bytes
// and guest RAM. The real decrypt/parser/frame paths run without a game image.
#include "text/translation.hpp"
#include "mods/mhp3rd_data_bin.hpp"
#include "mods/mhp3rd_mods.hpp"
#include "psprecomp/guest_memory.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
std::uint32_t entry = 4289;
std::uint64_t entry_size = 64;
constexpr std::uint64_t archive_start = 2048;
constexpr std::uint32_t ram = 0x08800000;

void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
void word(std::vector<std::uint8_t> &bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = static_cast<std::uint8_t>(value >> (i * 8));
}
}

namespace mhp3rd::mods {
// An isolated lookup seam: this test owns one synthetic archive entry.
std::optional<EntryAt> entry_at_offset(std::uint64_t offset) {
    if (offset < archive_start || offset - archive_start >= entry_size) return std::nullopt;
    return EntryAt{entry, offset - archive_start, entry_size};
}
}

int main() {
    namespace fs = std::filesystem;
    using namespace mhp3rd;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = fs::temp_directory_path() / ("yakumo-text-runtime-" + std::to_string(unique));
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{dir};
    try {
        fs::create_directories(dir);
        {
            std::ofstream out(dir / "test.lang");
            out << "language = test\n[4289]\n0:0 = Translated\n[2835]\n2:1 = New\n";
        }
        for (std::size_t n = 0; n < 16; ++n) {
            text::set_language("test", {dir});
            std::vector<std::uint8_t> bytes(n, 0);
            mods::p3rd::encrypt(bytes, 1, 0);
            text::translate_read(archive_start, bytes);
        }
        // A truncated ordinary header with no valid table must also be safe.
        entry = 2835;
        text::set_language("test", {dir});
        std::vector<std::uint8_t> header(12, 0);
        word(header, 0, 62);
        word(header, 4, 8);
        word(header, 8, 0xffffffffu);
        mods::p3rd::encrypt(header, 1, 0);
        text::translate_read(archive_start, header);
        entry = 4289;
        text::set_language("test", {dir});
        std::vector<std::uint8_t> clear(64, 0);
        word(clear, 0, 0);
        word(clear, 4, 16);
        word(clear, 8, 0xffffffffu);
        word(clear, 16, 0);
        word(clear, 20, 16);
        word(clear, 24, 0xffffffffu);
        const std::string original = "Original";
        std::copy(original.begin(), original.end(), clear.begin() + 32);
        auto encrypted = clear;
        mods::p3rd::encrypt(encrypted, 1, 0);
        psprecomp::GuestMemory memory(64u * 1024u * 1024u);
        memory.copy_in(ram, clear);
        unsigned allocations = 0;
        const text::ArenaAllocator allocate = [&](std::size_t size) {
            ++allocations;
            return std::optional<text::Arena>{{ram + 4096, ram + 4096 + static_cast<std::uint32_t>(size)}};
        };
        text::translate_read(archive_start, std::span(encrypted).first(2));
        text::frame(memory, allocate);
        require(allocations == 0, "a short prefix does not trigger translation");
        // Tail before the middle: no fabricated unread bytes reach the parser.
        text::translate_read(archive_start + 32, std::span(encrypted).subspan(32));
        text::frame(memory, allocate);
        require(allocations == 0, "a gap does not trigger translation");
        text::translate_read(archive_start + 2, std::span(encrypted).subspan(2, 30));
        text::frame(memory, allocate);
        require(allocations == 1 && text::applied_blocks().size() == 1, "split dialogue is applied once");
        const auto translated = ram + 16 + memory.load32(ram + 20);
        require(memory.load8(translated) == 'T', "the live dialogue points into the translated arena");
        memory.copy_in(ram, clear);
        text::translate_read(archive_start, encrypted);
        text::frame(memory, allocate);
        require(allocations == 1, "reloading a dialogue reuses its arena");
        require(memory.load8(ram + 16 + memory.load32(ram + 20)) == 'T', "reload reapplies translation");
        text::set_language("original", {dir});
        require(!text::active(), "original mode clears the translation state");
        text::translate_read(archive_start, encrypted);
        text::frame(memory, allocate);
        require(allocations == 1, "inactive mode performs no allocation");
        std::cout << "text runtime tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
