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
        // Ordinary menu blocks are located by their header and probe string.
        entry = 2835;
        entry_size = 128;
        clear.assign(128, 0);
        word(clear, 0, 2);
        word(clear, 4, 8);
        word(clear, 8, 32);
        word(clear, 32, 12);
        word(clear, 36, 16);
        word(clear, 40, 0xffffffffu);
        const std::string menu = "Menu";
        std::copy(menu.begin(), menu.end(), clear.begin() + 48);
        memory.copy_in(ram, clear);
        encrypted = clear;
        mods::p3rd::encrypt(encrypted, 1, 0);
        text::set_language("test", {dir});
        text::translate_read(archive_start, encrypted);
        text::frame(memory, allocate);
        require(text::applied_blocks().size() == 1, "ordinary text blocks are located and applied");
        require(memory.load8(ram + 32 + memory.load32(ram + 36)) == 'N', "ordinary block points to translated text");
        memory.copy_in(ram, clear);
        text::forget_blocks();
        text::translate_read(archive_start, encrypted);
        text::frame(memory, allocate);
        require(memory.load8(ram + 32 + memory.load32(ram + 36)) == 'N', "forgotten blocks can be reapplied");
        {
            std::ofstream out(dir / "test.lang");
            out << "language = test\n[16]\n2:1 = Main\n";
        }
        text::set_language("test", {dir});
        memory.copy_in(text::kMainTextBlock, clear);
        text::frame(memory, allocate);
        require(text::applied_blocks().at(0).entry == 16, "main text needs no archive read hook");
        // Two invented quest records, copied as an archive buffer and as inline
        // fields. The parser must protect sentinels and patch both shapes.
        {
            std::ofstream out(dir / "test.lang");
            out << "language = test\n[4059]\n64:136 = NewTitle\n68:160 = Goal\n72:184 = Details\n";
        }
        entry = 4059;
        entry_size = 512;
        clear.assign(512, 0);
        word(clear, 0, 64);
        word(clear, 4, 256);
        const std::vector<std::string> fields{"FirstTitle", "Objective", "Description"};
        for (std::size_t record = 0; record < 2; ++record) {
            const auto base = record == 0 ? 64u : 256u;
            for (std::size_t i = 0; i < fields.size(); ++i) {
                const auto offset = base + 72 + i * 24;
                word(clear, base + i * 4, offset);
                std::copy(fields[i].begin(), fields[i].end(), clear.begin() + offset);
            }
            word(clear, base + 12, base);
        }
        memory.copy_in(ram, clear);
        memory.copy_in(ram + 1024, clear);
        // A separate inline structure preserves the source spacing.
        memory.copy_in(ram + 2048, std::span(clear).subspan(136, 72));
        encrypted = clear;
        mods::p3rd::encrypt(encrypted, 1, 0);
        text::set_language("test", {dir});
        text::translate_read(archive_start, encrypted);
        text::frame(memory, allocate);
        require(text::applied_blocks().at(0).applied == 6, "both quest buffer copies are repointed");
        require(memory.load32(ram + 76) == 64, "quest table sentinels are preserved");
        text::frame(memory, allocate);
        require(memory.load8(ram + 2048) == 'N', "inline quest fields are patched in their own slots");
        require(memory.load8(ram + 2072) == 'G', "the inline objective is translated");
        const auto current_allocations = allocations;
        text::set_language("original", {dir});
        require(!text::active(), "original mode clears the translation state");
        text::translate_read(archive_start, encrypted);
        text::frame(memory, allocate);
        require(allocations == current_allocations, "inactive mode performs no allocation");
        std::cout << "text runtime tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
