// Performance overlay pixels and mode/log contracts with synthetic statistics.
#include "perf/perf_overlay.hpp"
#include "perf/frame_stats.hpp"
#include "settings/settings.hpp"
#include "install/user_data.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace mhp3rd;
int failures{};

void check(bool condition, const char *message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void mode_contracts() {
    using settings::PerfDisplay;
    struct Case {
        PerfDisplay mode;
        bool overlay;
        bool compact;
        bool log;
    };
    const Case cases[] = {{PerfDisplay::Off, false, false, false}, {PerfDisplay::Overlay, true, false, false},
        {PerfDisplay::OverlayAndLog, true, false, true}, {PerfDisplay::Log, false, false, true},
        {PerfDisplay::Compact, true, true, false}, {PerfDisplay::CompactAndLog, true, true, true}};
    for (const auto &c : cases) {
        settings::current().perf = c.mode;
        const auto selected = perf::options();
        check(selected.overlay == c.overlay && selected.compact == c.compact && selected.log == c.log,
            "each mode enables exactly its selected overlay style and log");
    }
}

void compact_pixels() {
    constexpr std::size_t size = perf::kOverlayWidth * perf::kCompactOverlayHeight;
    constexpr std::uint32_t guard = 0x12345678u;
    constexpr std::uint32_t background = 0xff101010u;
    constexpr std::uint32_t text = 0xffebebebu;
    std::vector<std::uint32_t> buffer(size + 16u, guard);
    auto *pixels = buffer.data() + 8u;
    perf::Summary summary;
    perf::draw_compact_overlay(pixels, summary, true);
    const std::vector<std::uint32_t> measuring(pixels, pixels + size);
    summary.valid = true;
    summary.fps = 30.0;
    summary.speed = 1.0;
    perf::draw_compact_overlay(pixels, summary, true);
    const std::vector<std::uint32_t> on(pixels, pixels + size);
    check(on != measuring, "the first valid summary replaces the measuring placeholders");
    check(pixels[7u * perf::kOverlayWidth + 45u] == text,
        "FPS is displayed with a decimal point in the single text line");
    summary.fps = 60.0;
    perf::draw_compact_overlay(pixels, summary, true);
    check(!std::equal(pixels, pixels + size, on.begin()), "FPS changes update the displayed number");
    summary.fps = 30.0;
    summary.speed = 0.5;
    perf::draw_compact_overlay(pixels, summary, true);
    check(!std::equal(pixels, pixels + size, on.begin()), "emulation speed changes update its percentage");
    summary.speed = 1.0;
    perf::draw_compact_overlay(pixels, summary, false);
    const std::vector<std::uint32_t> off(pixels, pixels + size);
    bool prefix_same = true;
    for (std::size_t y = 0; y < perf::kCompactOverlayHeight; ++y)
        prefix_same = prefix_same &&
            std::equal(on.begin() + y * perf::kOverlayWidth, on.begin() + y * perf::kOverlayWidth + 164u,
                off.begin() + y * perf::kOverlayWidth);
    check(prefix_same && on != off, "Vsync changes only the ON/OFF value after the FPS and speed fields");
    for (std::size_t y = 0; y < perf::kCompactOverlayHeight; ++y) {
        for (std::size_t x = 0; x < perf::kOverlayWidth; ++x) {
            const auto pixel = pixels[y * perf::kOverlayWidth + x];
            check(pixel == background || pixel == text, "compact has only text and the standard dark background");
            if (y < 2u || y >= 9u || x < 2u || x >= perf::kOverlayWidth - 2u)
                check(pixel == background, "the compact line retains the standard two-pixel internal padding");
        }
    }
    for (const double value : {0.0, 120.0, 1000000.0}) {
        summary.fps = value;
        summary.speed = value;
        perf::draw_compact_overlay(pixels, summary, false);
        check(std::all_of(buffer.begin(), buffer.begin() + 8u, [](auto p) { return p == guard; }) &&
                std::all_of(buffer.end() - 8u, buffer.end(), [](auto p) { return p == guard; }),
            "zero and long numbers never write beyond the compact pixel buffer");
    }
    for (const auto &[height, scale] :
        std::array<std::pair<unsigned, unsigned>, 5>{{{0u, 1u}, {359u, 1u}, {719u, 1u}, {720u, 2u}, {2160u, 6u}}})
        check(perf::overlay_scale(height) == scale, "both overlay styles keep the same readable integer scale");
}

void log_contracts() {
    for (const auto mode : {settings::PerfDisplay::Compact, settings::PerfDisplay::CompactAndLog}) {
        settings::current().perf = mode;
        std::ostringstream output;
        auto *previous = std::cout.rdbuf(output.rdbuf());
        perf::restart_measurement();
        perf::end_frame(100u);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        perf::end_frame(1100100u);
        std::cout.rdbuf(previous);
        const auto line = output.str();
        const auto first = line.find("[perf] fps ");
        if (mode == settings::PerfDisplay::CompactAndLog)
            check(first != std::string::npos && line.find("[perf] fps ", first + 1u) == std::string::npos &&
                    line.find(" game ") != std::string::npos && line.find(" speed ") != std::string::npos,
                "Compact and Log emits one full performance summary per measured second");
        else
            check(first == std::string::npos, "Compact alone does not emit performance logs");
        check(perf::last_second().valid, "statistics are collected with and without the log");
    }
}
} // namespace

int main(int argc, char **argv) {
    const auto sandbox = std::filesystem::temp_directory_path() /
        ("yakumo-perf-" + std::to_string(perf::Clock::now().time_since_epoch().count()));
    install::set_data_directory_override(sandbox);
    if (argc > 1)
        check(settings::current().perf == settings::from_entries({{"video.performance", argv[1]}}).perf,
            "the environment selects the requested compact mode before settings are edited");
    mode_contracts();
    compact_pixels();
    log_contracts();
    std::filesystem::remove_all(sandbox);
    std::cout << (failures == 0 ? "PASS" : "FAIL") << ": performance overlays (" << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
