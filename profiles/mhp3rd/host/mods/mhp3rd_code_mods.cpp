#include "mods/mhp3rd_code_mods.hpp"

#include "mods/mhp3rd_mod_format.hpp"

#include "psprecomp/decoder.hpp"
#include "psprecomp/jit.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdio>
#include <iostream>

namespace mhp3rd::mods::p3rd {
namespace {

std::string hex8(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

} // namespace

std::size_t retire_code(psprecomp::Runtime &runtime, std::uint32_t address, std::size_t length, const std::string &who,
    std::vector<std::string> &problems) {
    const std::uint32_t start = address & ~3u;
    const auto end = static_cast<std::uint32_t>(address + length);
    // Native code the JIT compiled from the old bytes goes too.
    psprecomp::jit_invalidate(runtime, start, end);
    // A jump or branch just before the change has the change's first word
    // compiled in as its delay slot, maybe in the unit before: it goes too.
    std::uint32_t first = start;
    if (start >= 4u && runtime.memory().contains(start - 4u, 4u) &&
        psprecomp::decode_allegrex(runtime.memory().load32(start - 4u)).has_delay_slot())
        first = start - 4u;
    bool code = false;
    for (std::uint32_t at = first; at < end && !code; at += 4u) code = runtime.has_function(at);
    if (!code) return 0u;
    const std::size_t units = runtime.retire_generated_code(first, end);
    // What is still registered there is the port's own: a function it
    // replaces, or an import. The game never runs those instructions.
    for (std::uint32_t at = start; at < end; at += 4u) {
        if (!runtime.has_function(at)) continue;
        problems.push_back(who + ": the block for " + hex8(address) +
            " changes code that Yakumo replaces with its own at " + hex8(at) + "; that part of it does nothing");
        break;
    }
    return units;
}

CodeReport write_code(psprecomp::Runtime &runtime, const std::vector<CodeFile> &files, const Reserve &reserve,
    const Place &place, bool trace) {
    CodeReport report;
    for (const CodeFile &file : files) {
        const Blocks read = read_blocks(file.source, place);
        for (const std::string &line : read.problems) report.problems.push_back(file.who + ": " + line);
        for (const Block &block : read.blocks) {
            const std::optional<std::uint32_t> guest = ram_range(block.address, block.bytes.size());
            if (!guest) {
                report.problems.push_back(
                    file.who + ": the block for " + hex8(block.address) + " is outside the game's memory");
                continue;
            }
            if (!block.expect.empty()) {
                std::vector<std::uint8_t> found(block.expect.size());
                for (std::size_t i = 0; i < found.size(); ++i)
                    found[i] = runtime.memory().load8(*guest + static_cast<std::uint32_t>(i));
                if (const auto wrong = mismatch(block.expect, found)) {
                    report.problems.push_back(
                        file.who + ": the block for " + hex8(block.address) + " is skipped: " + *wrong);
                    continue;
                }
            }
            if (reserve) {
                const Reservation held = reserve(*guest, static_cast<std::uint32_t>(block.bytes.size()));
                if (!held.problem.empty())
                    report.problems.push_back(file.who + ": the block for " + hex8(block.address) + " " + held.problem);
                if (!held.write) continue;
            }
            runtime.memory().copy_in(*guest, block.bytes);
            report.units += retire_code(runtime, *guest, block.bytes.size(), file.who, report.problems);
            if (block.run) report.run.push_back(*guest);
            ++report.blocks;
            report.bytes += block.bytes.size();
            if (trace)
                std::cout << "[mods] " << file.who << ": wrote " << block.bytes.size() << " bytes at " << hex8(*guest)
                          << "\n";
        }
    }
    return report;
}

} // namespace mhp3rd::mods::p3rd
