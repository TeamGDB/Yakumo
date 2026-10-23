#include "mods/mhp3rd_pictures.hpp"

#include "mods/mhp3rd_added_files.hpp"
#include "mods/mhp3rd_mods.hpp"
#include "mods/mhp3rd_tables.hpp"

#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <utility>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace mhp3rd::mods::pictures {
namespace {

using psprecomp::Runtime;

// A row of monster.pictures: u16 x and u16 y of the picture's cell in the
// image, s16 its palette.
constexpr std::string_view kTable = "monster.pictures";
constexpr std::uint32_t kRowBytes = 6u;
// The image of pictures: 352 by 352, 4-bit indices, 64 palettes of 16
// RGBA8888 colours; the cells are a grid of 8 by 8.
constexpr std::uint32_t kImageSide = 352u;
constexpr std::uint32_t kFormatIndex4 = 4u;
constexpr std::uint32_t kPaletteRgba8888 = 3u;
constexpr int kPalettes = 64;
constexpr int kGrid = static_cast<int>(kImageSide) / kSide;
// TMH: ".TMH0.14", u32 images, u32 0; per image u32 entry size, 0, 1, u32
// has a palette, then the pixels (u32 size with this header, 1, format, u16
// width, u16 height) and the palette (u32 size with this header, 2, format,
// entries).
constexpr std::string_view kSignature = ".TMH0.14";

struct Given {
    std::uint8_t kind{};
    int column{}, row{}, palette{};
    Pixels pixels;
};

struct State {
    std::vector<Given> given;
    std::uint32_t rows_filled{kGameRows}; // rows past the game's given the unknown picture
    std::function<std::optional<std::vector<std::uint8_t>>(FileId)> reader;
    std::shared_ptr<const std::vector<std::uint8_t>> served;
    bool installed{};
};

State &state() {
    static State s;
    return s;
}

std::uint32_t u32(const std::vector<std::uint8_t> &b, std::size_t at) {
    std::uint32_t v;
    std::memcpy(&v, b.data() + at, 4u);
    return v;
}

std::uint16_t u16(const std::vector<std::uint8_t> &b, std::size_t at) {
    std::uint16_t v;
    std::memcpy(&v, b.data() + at, 2u);
    return v;
}

// The cells and palettes the table's rows use, mods' rows included.
std::pair<std::set<std::pair<int, int>>, std::set<int>> taken(
    const psprecomp::GuestMemory &memory, std::uint32_t table) {
    std::set<std::pair<int, int>> cells;
    std::set<int> palettes;
    for (std::uint32_t k = 0; k < kGameRows; ++k) {
        const std::uint32_t at = table + kRowBytes * k;
        cells.insert({memory.load16(at) / kSide, memory.load16(at + 2u) / kSide});
        palettes.insert(static_cast<std::int16_t>(memory.load16(at + 4u)));
    }
    for (const Given &g : state().given) {
        cells.insert({g.column, g.row});
        palettes.insert(g.palette);
    }
    return {cells, palettes};
}

} // namespace

std::optional<std::vector<std::uint8_t>> compose(
    const std::vector<std::uint8_t> &bank, int column, int row, int palette, const Pixels &pixels) {
    if (bank.size() < 16u || std::memcmp(bank.data(), kSignature.data(), kSignature.size()) != 0) return std::nullopt;
    if (column < 0 || row < 0 || column >= kGrid || row >= kGrid || palette < 0 || palette >= kPalettes)
        return std::nullopt;
    if (pixels.size() != static_cast<std::size_t>(kSide * kSide * 4)) return std::nullopt;
    const std::uint32_t images = u32(bank, 8u);
    std::size_t pos = 16u;
    for (std::uint32_t i = 0; i < images && i < kTextureImage; ++i) {
        if (pos + 4u > bank.size() || u32(bank, pos) == 0u) return std::nullopt;
        pos += u32(bank, pos);
    }
    if (images <= kTextureImage || pos + 32u > bank.size()) return std::nullopt;
    const std::uint32_t pixel_size = u32(bank, pos + 16u);
    if (u32(bank, pos + 12u) != 1u || u32(bank, pos + 24u) != kFormatIndex4 || u16(bank, pos + 28u) != kImageSide ||
        u16(bank, pos + 30u) != kImageSide || pixel_size < 16u + kImageSide * kImageSide / 2u)
        return std::nullopt;
    const std::size_t data = pos + 32u;
    const std::size_t palette_at = pos + 16u + pixel_size;
    if (palette_at + 16u > bank.size() || u32(bank, palette_at + 8u) != kPaletteRgba8888 ||
        u32(bank, palette_at + 12u) < static_cast<std::uint32_t>(kPalettes) * kColours ||
        palette_at + 16u + kPalettes * kColours * 4u > bank.size())
        return std::nullopt;

    // The picture's colours, in the order they first appear.
    std::vector<std::uint32_t> colours;
    std::vector<std::uint8_t> indices(static_cast<std::size_t>(kSide * kSide));
    for (std::size_t p = 0; p < indices.size(); ++p) {
        std::uint32_t c;
        std::memcpy(&c, pixels.data() + 4u * p, 4u);
        auto found = std::find(colours.begin(), colours.end(), c);
        if (found == colours.end()) {
            if (colours.size() == kColours) return std::nullopt;
            colours.push_back(c);
            found = colours.end() - 1;
        }
        indices[p] = static_cast<std::uint8_t>(found - colours.begin());
    }

    std::vector<std::uint8_t> out = bank;
    for (std::size_t k = 0; k < kColours; ++k) {
        const std::uint32_t c = k < colours.size() ? colours[k] : colours.back();
        std::memcpy(out.data() + palette_at + 16u + (static_cast<std::size_t>(palette) * kColours + k) * 4u, &c, 4u);
    }
    // The GE's swizzled layout: blocks 16 bytes wide and 8 rows high, left to
    // right, then top to bottom; two pixels a byte, the first in the low half.
    constexpr std::uint32_t kRowBytesImage = kImageSide / 2u;
    constexpr std::uint32_t kBlocksAcross = kRowBytesImage / 16u;
    for (int y = 0; y < kSide; ++y) {
        for (int x = 0; x < kSide; ++x) {
            const std::uint32_t px = static_cast<std::uint32_t>(column * kSide + x);
            const std::uint32_t py = static_cast<std::uint32_t>(row * kSide + y);
            const std::uint32_t byte = px / 2u;
            const std::size_t at =
                data + ((py / 8u) * kBlocksAcross + byte / 16u) * 128u + (py % 8u) * 16u + byte % 16u;
            const std::uint8_t index = indices[static_cast<std::size_t>(y * kSide + x)];
            out[at] = (px & 1u) != 0u ? static_cast<std::uint8_t>((out[at] & 0x0Fu) | (index << 4))
                                      : static_cast<std::uint8_t>((out[at] & 0xF0u) | index);
        }
    }
    return out;
}

std::optional<Pixels> from_png(std::span<const std::uint8_t> png) {
    int width = 0, height = 0, channels = 0;
    unsigned char *decoded = png.empty()
        ? nullptr
        : stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &width, &height, &channels, 4);
    if (decoded == nullptr) return std::nullopt;
    std::optional<Pixels> pixels;
    if (width == kSide && height == kSide) pixels = Pixels(decoded, decoded + kSide * kSide * 4);
    stbi_image_free(decoded);
    return pixels;
}

bool give(Runtime &runtime, std::uint8_t kind, std::uint8_t like, const Pixels *own, std::string *why) {
    State &s = state();
    const auto fail = [why](std::string reason) {
        if (why != nullptr) *why = std::move(reason);
        return false;
    };
    if (own != nullptr) {
        if (own->size() != static_cast<std::size_t>(kSide * kSide * 4))
            return fail("its picture is not " + std::to_string(kSide) + " by " + std::to_string(kSide) + " pixels");
        std::set<std::uint32_t> colours;
        for (std::size_t p = 0; p + 4u <= own->size(); p += 4u) {
            std::uint32_t c;
            std::memcpy(&c, own->data() + p, 4u);
            colours.insert(c);
        }
        if (colours.size() > kColours) return fail("its picture has more than 16 colours");
    }
    std::string reason;
    const std::optional<std::uint32_t> table =
        tables::extend(runtime, kTable, std::max<std::uint32_t>(kind + 1u, kGameRows), &reason);
    if (!table) return fail("the pictures' table cannot grow (" + reason + ")");
    psprecomp::GuestMemory &memory = runtime.memory();
    const auto copy_row = [&](std::uint32_t to, std::uint32_t from) {
        for (std::uint32_t b = 0; b < kRowBytes; b += 2u)
            memory.store16(*table + kRowBytes * to + b, memory.load16(*table + kRowBytes * from + b));
    };
    // Rows between the game's and this kind: the game's picture for a kind it
    // has none of (row 0, a question mark), as the kinds it leaves out show.
    for (; s.rows_filled < kind; ++s.rows_filled) copy_row(s.rows_filled, 0u);
    if (own == nullptr) {
        copy_row(kind, like < kGameRows ? like : 0u);
    } else {
        const auto [cells, palettes] = taken(memory, *table);
        std::optional<std::pair<int, int>> cell;
        for (int r = 0; r < kGrid && !cell; ++r)
            for (int c = 0; c < kGrid && !cell; ++c)
                if (cells.count({c, r}) == 0u) cell = std::pair{c, r};
        int palette = -1;
        for (int p = 0; p < kPalettes && palette < 0; ++p)
            if (palettes.count(p) == 0u) palette = p;
        if (!cell || palette < 0) return fail("the pictures' image has no cell or palette left");
        if (!s.installed) {
            if (!added::stand_in(runtime, [](Runtime &r, FileId file) { return serve(r, file); }))
                return fail("the game's file loader cannot be taken over");
            s.installed = true;
        }
        s.given.push_back({kind, cell->first, cell->second, palette, *own});
        s.served.reset();
        memory.store16(*table + kRowBytes * kind, static_cast<std::uint16_t>(cell->first * kSide));
        memory.store16(*table + kRowBytes * kind + 2u, static_cast<std::uint16_t>(cell->second * kSide));
        memory.store16(*table + kRowBytes * kind + 4u, static_cast<std::uint16_t>(palette));
    }
    s.rows_filled = std::max<std::uint32_t>(s.rows_filled, kind + 1u);
    return true;
}

std::shared_ptr<const std::vector<std::uint8_t>> serve(Runtime &, FileId file) {
    State &s = state();
    if (file != kTextureFile || s.given.empty()) return nullptr;
    if (s.served) return s.served;
    std::optional<std::vector<std::uint8_t>> bank = s.reader ? s.reader(file) : original_file(file);
    if (!bank) return nullptr;
    for (const Given &g : s.given) {
        std::optional<std::vector<std::uint8_t>> next = compose(*bank, g.column, g.row, g.palette, g.pixels);
        if (!next) return nullptr; // not the bank traced: the game's, untouched
        bank = std::move(next);
    }
    s.served = std::make_shared<const std::vector<std::uint8_t>>(std::move(*bank));
    return s.served;
}

void reset_for_tests() {
    state() = {};
}

void read_files_with_for_tests(std::function<std::optional<std::vector<std::uint8_t>>(FileId)> reader) {
    state().reader = std::move(reader);
}

} // namespace mhp3rd::mods::pictures
