// The JIT against the interpreter: the same guest code, from the same
// registers and memory, must leave the same registers, FPU and VFPU state,
// HI/LO and memory whichever of the two runs it. Programs are generated:
// every instruction the decoder accepts outside control flow, then programs
// with branches of every kind, likely branches, delay slots, calls and loops.
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/decoder.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/jit.hpp"
#include "psprecomp/runtime.hpp"

#include <bit>
#include <chrono>
#include <exception>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

using psprecomp::AllegrexContext;
using psprecomp::OpcodeKind;
using psprecomp::Runtime;

int failures = 0;

void check(bool condition, const std::string &what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

constexpr std::uint32_t kSentinel = 0x08800000u; // where every program returns: lui ra, 0x0880
constexpr std::uint32_t kCode = 0x08A00000u;
constexpr std::uint32_t kData = 0x08B00000u;
constexpr std::uint32_t kDataSize = 0x10000u;
constexpr std::uint32_t kBase = 29u;   // points into the data area
constexpr std::uint32_t kCallee = 28u; // a subroutine's address
constexpr std::uint32_t kLink = 30u;   // jalr's link in generated calls

void done(Runtime &runtime, AllegrexContext &) {
    runtime.stop("program returned");
}

struct Setup {
    std::vector<std::uint32_t> code;
    std::uint32_t entry{kCode};
    AllegrexContext ctx{};
    std::vector<std::uint8_t> data;
};

struct Outcome {
    AllegrexContext ctx{};
    std::vector<std::uint8_t> data;
    bool stopped_normally{};
    bool fatal{}; // a guest memory error: an exception from the interpreter, a stop from the JIT
    std::string reason;
    psprecomp::JitStats jit{};
};

// One runtime per engine, reused: each program rewrites the code, the data
// and the registers, and drops what the JIT compiled for the last one.
Runtime &engine(bool jit) {
    static Runtime *runtimes[2]{};
    Runtime *&runtime = runtimes[jit ? 1 : 0];
    if (runtime == nullptr) {
        runtime = new Runtime(32u * 1024u * 1024u);
        runtime->register_function(kSentinel, &done, "test_sentinel");
    }
    return *runtime;
}

Outcome run(const Setup &setup, bool jit, std::uint64_t dispatches = 100000u) {
    psprecomp::set_jit_enabled(jit);
    const psprecomp::JitStats before = psprecomp::jit_stats();
    Runtime &runtime = engine(jit);
    psprecomp::jit_invalidate(runtime, kCode, kCode + 0x10000u);
    for (std::uint32_t at = 0; at < 0x1000u; at += 4u) runtime.memory().store32(kCode + at, 0u);
    for (std::size_t i = 0; i < setup.code.size(); ++i)
        runtime.memory().store32(kCode + static_cast<std::uint32_t>(i) * 4u, setup.code[i]);
    runtime.memory().copy_in(kData, setup.data);
    runtime.cpu() = setup.ctx;
    // Quiet: the interpreter reports its statistics when the run stops.
    std::ostringstream discard;
    std::streambuf *previous = std::cerr.rdbuf(discard.rdbuf());
    std::string fatal;
    try {
        runtime.run(setup.entry, dispatches);
    } catch (const std::exception &error) {
        fatal = error.what();
    }
    std::cerr.rdbuf(previous);
    if (std::getenv("PSPRECOMP_JIT_TEST_TRACE") != nullptr) std::fprintf(stderr, "%s", discard.str().c_str());
    Outcome out;
    out.fatal = !fatal.empty() || runtime.stop_reason().starts_with("guest code compiled at run time stopped");
    out.ctx = runtime.cpu();
    out.data.resize(kDataSize);
    runtime.memory().copy_out(kData, out.data);
    out.reason = runtime.stop_reason();
    out.stopped_normally = out.reason == "program returned";
    const psprecomp::JitStats after = psprecomp::jit_stats();
    out.jit.compiled = after.compiled - before.compiled;
    out.jit.instructions = after.instructions - before.instructions;
    out.jit.shared = after.shared - before.shared;
    return out;
}

std::string describe(const Setup &setup) {
    std::ostringstream text;
    for (std::size_t i = 0; i < setup.code.size(); ++i) {
        const auto d = psprecomp::decode_allegrex(setup.code[i]);
        char line[96];
        std::snprintf(line, sizeof(line), "\n      %08X  %08X  %s", kCode + static_cast<unsigned>(i) * 4u,
            setup.code[i], d.mnemonic.c_str());
        text << line;
    }
    return text.str();
}

// Compares everything an instruction can change. Returns the first difference.
std::string difference(const Outcome &a, const Outcome &b) {
    char text[160];
    for (unsigned r = 0; r < 32u; ++r)
        if (a.ctx.gpr[r] != b.ctx.gpr[r]) {
            std::snprintf(text, sizeof(text), "gpr %u: interpreter %08X, jit %08X", r, a.ctx.gpr[r], b.ctx.gpr[r]);
            return text;
        }
    if (a.ctx.hi != b.ctx.hi || a.ctx.lo != b.ctx.lo) return "hi/lo";
    if (a.fatal || b.fatal) {
        // Where a fatal error leaves ctx.pc differs: the interpreter throws
        // out of the instruction, the JIT returns from the function.
        if (a.fatal != b.fatal) return "only one of them failed: \"" + a.reason + "\" / \"" + b.reason + "\"";
        return {};
    }
    if (a.ctx.pc != b.ctx.pc) {
        std::snprintf(text, sizeof(text), "pc: interpreter %08X, jit %08X", a.ctx.pc, b.ctx.pc);
        return text;
    }
    for (unsigned r = 0; r < 32u; ++r)
        if (std::bit_cast<std::uint32_t>(a.ctx.fpr[r]) != std::bit_cast<std::uint32_t>(b.ctx.fpr[r]))
            return "fpr " + std::to_string(r);
    if (a.ctx.fcr31 != b.ctx.fcr31) return "fcr31";
    for (unsigned r = 0; r < 128u; ++r)
        if (std::bit_cast<std::uint32_t>(a.ctx.vfpu[r]) != std::bit_cast<std::uint32_t>(b.ctx.vfpu[r]))
            return "vfpu " + std::to_string(r);
    for (unsigned r = 0; r < 16u; ++r)
        if (a.ctx.vfpu_ctrl[r] != b.ctx.vfpu_ctrl[r]) return "vfpu_ctrl " + std::to_string(r);
    for (std::size_t i = 0; i < a.data.size(); ++i)
        if (a.data[i] != b.data[i]) {
            std::snprintf(text, sizeof(text), "memory at %08X", kData + static_cast<unsigned>(i));
            return text;
        }
    if (a.reason != b.reason) return "stop: interpreter \"" + a.reason + "\", jit \"" + b.reason + "\"";
    return {};
}

class Generator {
public:
    explicit Generator(std::uint32_t seed) : random_(seed) {}

    std::uint32_t word() { return static_cast<std::uint32_t>(random_()); }
    std::uint32_t below(std::uint32_t n) { return word() % n; }

    // An instruction that is not control flow and cannot leave the test's
    // memory or overwrite the registers the program relies on.
    std::uint32_t simple(bool keep_ra = false) {
        for (;;) {
            std::uint32_t w = word();
            const std::uint32_t op = w >> 26u;
            // Loads and stores of every unit address the data area through rs.
            const bool memory_op = (op >= 0x20u && op <= 0x2Eu) || op == 0x31u || op == 0x39u || op == 0x32u ||
                op == 0x3Au || op == 0x36u || op == 0x3Eu;
            if (memory_op) w = (w & ~(31u << 21u)) | (kBase << 21u);
            const psprecomp::DecodedInstruction d = psprecomp::decode_allegrex(w);
            if (d.is_control_flow() || d.has_delay_slot()) continue;
            switch (d.kind) {
            case OpcodeKind::Unsupported:
            case OpcodeKind::Syscall:
            case OpcodeKind::Break:
            case OpcodeKind::Vfpu:
                continue;
            default:
                break;
            }
            // Keep the base, the callee and jalr's link intact: whatever bits
            // a format puts there, any field naming them rejects the word.
            const std::uint32_t rt = (w >> 16u) & 31u;
            const std::uint32_t rd = (w >> 11u) & 31u;
            const auto reserved = [keep_ra](std::uint32_t r) {
                return r == kBase || r == kCallee || r == kLink || (keep_ra && r == 31u);
            };
            if (reserved(rt) || reserved(rd)) continue;
            return w;
        }
    }

    Setup start(std::vector<std::uint32_t> code) {
        Setup setup;
        setup.code = std::move(code);
        // Every program returns to the sentinel, whatever it did to $ra.
        setup.code.push_back(0x3C1F0880u); // lui ra, 0x0880
        setup.code.push_back(0x03E00008u); // jr ra
        setup.code.push_back(0u);          // nop
        for (unsigned r = 1; r < 32u; ++r) setup.ctx.gpr[r] = interesting();
        setup.ctx.gpr[kBase] = kData + kDataSize / 2u;
        setup.ctx.hi = word();
        setup.ctx.lo = word();
        for (float &f : setup.ctx.fpr) f = std::bit_cast<float>(interesting_float());
        for (float &f : setup.ctx.vfpu) f = std::bit_cast<float>(interesting_float());
        setup.ctx.fcr31 = word() & 0x0181FFFFu;
        setup.ctx.vfpu_ctrl[3] = word() & 0xFFu;
        setup.data.resize(kDataSize);
        for (std::uint8_t &b : setup.data) b = static_cast<std::uint8_t>(word());
        return setup;
    }

private:
    std::uint32_t interesting() {
        static constexpr std::uint32_t edges[] = {
            0u, 1u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 31u, 32u, 0xFFFF8000u, 0x00008000u};
        return below(4u) == 0u ? edges[below(std::size(edges))] : word();
    }
    std::uint32_t interesting_float() {
        static constexpr std::uint32_t edges[] = {
            0u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x3F800000u, 0xBF800000u, 0x00000001u, 0x4F000000u};
        if (below(4u) == 0u) return edges[below(std::size(edges))];
        // Ordinary magnitudes most of the time.
        return std::bit_cast<std::uint32_t>(static_cast<float>(static_cast<std::int32_t>(word())) / 65536.0f);
    }

    std::mt19937 random_;
};

bool compare(const Setup &setup, const std::string &what, bool expect_compiled = true) {
    if (std::getenv("PSPRECOMP_JIT_TEST_TRACE") != nullptr) {
        std::fprintf(stderr, "%s%s\n", what.c_str(), describe(setup).c_str());
        std::fflush(stderr);
    }
    const Outcome interpreted = run(setup, false);
    const Outcome compiled = run(setup, true);
    const std::string diff = difference(interpreted, compiled);
    check(diff.empty(), what + ": " + diff + describe(setup));
    if (expect_compiled) check(compiled.jit.compiled > 0u, what + ": the JIT compiled nothing");
    check(interpreted.stopped_normally || interpreted.reason == compiled.reason,
        what + ": stopped with \"" + interpreted.reason + "\"");
    return diff.empty();
}

// Every kind of straight-line instruction.
void straight_line(Generator &g) {
    for (unsigned program = 0; program < 400u; ++program) {
        std::vector<std::uint32_t> code;
        for (unsigned i = 0; i < 48u; ++i) code.push_back(g.simple());
        if (!compare(g.start(code), "straight line " + std::to_string(program))) return;
    }
}

// Single-precision arithmetic, conversions, moves and compares, which the JIT
// emits as native floating-point instructions, from values that include
// zeros of both signs, infinities, NaNs and denormals; then branches on the
// compares' condition.
void floating_point(Generator &g) {
    const auto cop1 = [&](std::uint32_t format, std::uint32_t funct) {
        return 0x46000000u | format << 21u | g.below(32u) << 16u | g.below(32u) << 11u | g.below(32u) << 6u | funct;
    };
    for (unsigned program = 0; program < 400u; ++program) {
        std::vector<std::uint32_t> code;
        for (unsigned i = 0; i < 48u; ++i) {
            switch (g.below(8u)) {
            case 0u:
            case 1u:
            case 2u:
                code.push_back(cop1(16u, g.below(8u))); // add sub mul div sqrt abs mov neg
                break;
            case 3u:
                code.push_back(cop1(16u, 0x30u | g.below(16u)) & ~(31u << 6u)); // c.cond.s
                break;
            case 4u:
                code.push_back(cop1(20u, 0x20u)); // cvt.s.w
                break;
            case 5u:
                code.push_back(cop1(16u, g.below(2u) == 0u ? 0x24u : 0x0Cu | g.below(4u))); // cvt.w round trunc...
                break;
            case 6u: // mtc1 / mfc1 with a free register
                code.push_back(
                    (g.below(2u) == 0u ? 0x44800000u : 0x44000000u) | (8u + g.below(8u)) << 16u | g.below(32u) << 11u);
                break;
            default:
                code.push_back(g.simple());
                break;
            }
        }
        // A branch on the last compare: bc1t or bc1f over an add.
        code.push_back(0x45000000u | g.below(2u) << 16u | 2u);
        code.push_back(cop1(16u, 0u));
        code.push_back(cop1(16u, 2u));
        if (!compare(g.start(code), "floating point " + std::to_string(program))) return;
    }
}

std::uint32_t encode_branch(Generator &g, std::uint32_t offset_words) {
    const std::uint32_t off = offset_words & 0xFFFFu;
    const std::uint32_t rs = g.below(32u);
    const std::uint32_t rt = g.below(32u);
    switch (g.below(14u)) {
    case 0u:
        return 0x10000000u | rs << 21u | rt << 16u | off; // beq
    case 1u:
        return 0x14000000u | rs << 21u | rt << 16u | off; // bne
    case 2u:
        return 0x18000000u | rs << 21u | off; // blez
    case 3u:
        return 0x1C000000u | rs << 21u | off; // bgtz
    case 4u:
        return 0x50000000u | rs << 21u | rt << 16u | off; // beql
    case 5u:
        return 0x54000000u | rs << 21u | rt << 16u | off; // bnel
    case 6u:
        return 0x58000000u | rs << 21u | off; // blezl
    case 7u:
        return 0x5C000000u | rs << 21u | off; // bgtzl
    case 8u: {                                // regimm: bltz bgez bltzl bgezl bltzal bgezal bltzall bgezall
        static constexpr std::uint32_t forms[] = {0u, 1u, 2u, 3u, 0x10u, 0x11u, 0x12u, 0x13u};
        return 0x04000000u | rs << 21u | forms[g.below(8u)] << 16u | off;
    }
    case 9u:
        return 0x45000000u | g.below(4u) << 16u | off; // bc1f/t, likely forms
    case 10u:
        return 0x49000000u | g.below(8u) << 18u | g.below(4u) << 16u | off; // bvf/t, likely forms
    case 11u:
        return 0x10000000u | off; // b (beq zero, zero)
    default:
        return 0x14000000u | rs << 21u | off; // bnez
    }
}

// Forward branches of every kind with delay slots, over random code.
void branches(Generator &g) {
    for (unsigned program = 0; program < 400u; ++program) {
        const unsigned n = 40u;
        std::vector<std::uint32_t> code(n);
        for (unsigned i = 0; i < n; ++i) code[i] = g.simple();
        for (unsigned i = 0; i + 2u < n; ++i) {
            if (g.below(4u) != 0u) continue;
            const unsigned target = i + 2u + g.below(n - i - 1u); // up to the return sequence
            const std::uint32_t offset = target - (i + 1u);
            if (g.below(6u) == 0u)
                code[i] = 0x08000000u | ((kCode + target * 4u) >> 2u & 0x03FFFFFFu); // j
            else
                code[i] = encode_branch(g, offset);
            ++i; // the next one is its delay slot
        }
        if (!compare(g.start(code), "branches " + std::to_string(program))) return;
    }
}

// A loop that runs many times inside one compiled function.
void loop(Generator &g) {
    std::vector<std::uint32_t> code = {
        0x24080032u, // addiu t0, zero, 50
        0x25290003u, // addiu t1, t1, 3
        0x2508FFFFu, // addiu t0, t0, -1
        0x1500FFFDu, // bne t0, zero, -3
        0x00094840u, // sll t1, t1, 1 (delay slot)
    };
    compare(g.start(code), "loop");
}

// A loop that never calls out: compiled, it returns to the dispatcher every
// so many times round, where the scheduler may switch threads, as the
// interpreter's bounded runs do; and leaves the same registers.
int g_boundaries = 0;
void count_boundary(Runtime &, AllegrexContext &) {
    ++g_boundaries;
}
void preemption(Generator &g) {
    const std::vector<std::uint32_t> code = {
        0x3C080004u, // lui t0, 4: 0x40000 times round
        0x25290003u, // addiu t1, t1, 3
        0x2508FFFFu, // addiu t0, t0, -1
        0x1500FFFDu, // bne t0, zero, -3
        0u,          // nop
    };
    const Setup setup = g.start(code);
    psprecomp::set_runtime_starvation_hook(&count_boundary, 1u);
    g_boundaries = 0;
    const Outcome jit = run(setup, true, 10000000u);
    const int compiled_boundaries = g_boundaries;
    psprecomp::set_runtime_starvation_hook(nullptr, 0u);
    // 0x40000 times round, 1024 jumps back a dispatch: about 256 of them.
    check(jit.stopped_normally && compiled_boundaries >= 200 && jit.ctx.gpr[9] == setup.ctx.gpr[9] + 3u * 0x40000u,
        "preemption: a compiled loop returns to the dispatcher every 1024 times round (" +
            std::to_string(compiled_boundaries) + " scheduler boundaries) and computes what it did");
}

// Guest code that rewrites a function and invalidates its instruction cache
// line (cache 0x08): the next call runs the new instruction, compiled or
// interpreted alike.
void self_modifying(Generator &g) {
    std::vector<std::uint32_t> code = {
        0x3C0808A0u, // lui t0, 0x08A0
        0x35080100u, // ori t0, t0, 0x0100: the callee
        0x3C092402u, // lui t1, 0x2402
        0x35290063u, // ori t1, t1, 0x0063: addiu v0, zero, 0x63
        0x0E280040u, // jal 0x08A00100: v0 = 5
        0u,          // nop
        0x0040A021u, // addu s4, v0, zero
        0xAD090000u, // sw t1, 0(t0)
        0xBD080000u, // cache 0x08, 0(t0)
        0x0E280040u, // jal 0x08A00100: now v0 = 0x63
        0u,          // nop
    };
    code.resize(0x100u / 4u, 0u);
    // The program's own end is appended after the callee by start(): jump
    // over the callee to it.
    code[11] = 0x0A280047u; // j 0x08A0011C
    code[12] = 0u;
    code.push_back(0x24020005u); // 0x08A00100: addiu v0, zero, 5
    code.push_back(0x03E00008u); // jr ra
    code.push_back(0u);
    code.push_back(0u);
    code.push_back(0u);
    code.push_back(0u);
    code.push_back(0u); // 0x08A00118
    const Setup setup = g.start(code);
    compare(setup, "self-modifying code after an icache line invalidate");
    const Outcome out = run(setup, true);
    check(out.stopped_normally && out.ctx.gpr[20] == 5u && out.ctx.gpr[2] == 0x63u,
        "self-modifying code: the compiled callee is dropped by cache 0x08 and the new instruction runs (v0 " +
            psprecomp::hex32(out.ctx.gpr[2]) + ")");
}

// A loop around a call, as games write them: the function the JIT compiles
// for the call's return address starts with the loop's backward branch.
void loop_with_call(Generator &g) {
    for (std::uint32_t iterations = 0u; iterations < 4u; ++iterations) {
        std::vector<std::uint32_t> code = {
            0x02002021u, // addu a0, s0, zero
            0x26100050u, // addiu s0, s0, 80
            0x0C000000u, // jal sub (patched below)
            0x00000000u,
            0x1611FFFBu, // bne s0, s1, -5 (back to the addu)
            0x00000000u,
            0x08000000u, // j over the subroutine (patched below)
            0x00000000u,
            0x24A50001u, // sub: addiu a1, a1, 1
            0x03E00008u, // jr ra
            0x00000000u,
        };
        code[2] = 0x0C000000u | ((kCode + 8u * 4u) >> 2u & 0x03FFFFFFu);
        code[6] = 0x08000000u | ((kCode + 11u * 4u) >> 2u & 0x03FFFFFFu);
        Setup setup = g.start(code);
        setup.ctx.gpr[16] = 0x0A1CC504u;                    // s0
        setup.ctx.gpr[17] = 0x0A1CC504u + 80u * iterations; // s1
        if (iterations == 0u) setup.ctx.gpr[17] += 80u;     // bne compares after the first add
        setup.ctx.gpr[5] = 0u;
        if (!compare(setup, "loop with call " + std::to_string(iterations))) return;
        // Entered at the backward branch, with the loop body not compiled
        // yet: the function's entry is not its lowest instruction.
        setup.entry = kCode + 16u;
        setup.ctx.gpr[16] += 80u;
        if (!compare(setup, "loop entered at its branch " + std::to_string(iterations))) return;
    }
}

// Calls: jal and jalr to a subroutine, which returns with jr.
void calls(Generator &g) {
    for (unsigned program = 0; program < 100u; ++program) {
        std::vector<std::uint32_t> code;
        for (unsigned i = 0; i < 6u; ++i) code.push_back(g.simple(true));
        const std::size_t jal_at = code.size();
        code.push_back(0u); // jal, patched below
        code.push_back(g.simple(true));
        for (unsigned i = 0; i < 4u; ++i) code.push_back(g.simple(true));
        code.push_back(kCallee << 21u | kLink << 11u | 0x09u); // jalr link, callee
        code.push_back(g.simple(true));
        for (unsigned i = 0; i < 4u; ++i) code.push_back(g.simple(true));
        // j over its delay slot and the two subroutines (5 and 4 words), to
        // the return sequence.
        code.push_back(0x08000000u |
            ((kCode + (static_cast<std::uint32_t>(code.size()) + 2u + 5u + 4u) * 4u) >> 2u & 0x03FFFFFFu));
        code.push_back(0u);
        // First subroutine (jal): returns with jr ra.
        const std::size_t first = code.size();
        for (unsigned i = 0; i < 3u; ++i) code.push_back(g.simple(true));
        code.push_back(0x03E00008u); // jr ra
        code.push_back(g.simple(true));
        // Second (jalr): returns with jr link.
        const std::size_t second = code.size();
        for (unsigned i = 0; i < 2u; ++i) code.push_back(g.simple(true));
        code.push_back(kLink << 21u | 0x08u); // jr link
        code.push_back(g.simple(true));
        code[jal_at] = 0x0C000000u | ((kCode + static_cast<std::uint32_t>(first) * 4u) >> 2u & 0x03FFFFFFu);
        Setup setup = g.start(code);
        setup.ctx.gpr[kCallee] = kCode + static_cast<std::uint32_t>(second) * 4u;
        if (!compare(setup, "calls " + std::to_string(program))) return;
    }
}

// Rewritten code is compiled again.
void invalidation(Generator &g) {
    Setup setup = g.start({0x24020001u}); // addiu v0, zero, 1
    psprecomp::set_jit_enabled(true);
    Runtime runtime(32u * 1024u * 1024u);
    runtime.register_function(kSentinel, &done, "test_sentinel");
    for (std::size_t i = 0; i < setup.code.size(); ++i)
        runtime.memory().store32(kCode + static_cast<std::uint32_t>(i) * 4u, setup.code[i]);
    runtime.cpu() = setup.ctx;
    std::ostringstream discard;
    std::streambuf *previous = std::cerr.rdbuf(discard.rdbuf());
    runtime.run(kCode, 1000u);
    const std::uint32_t first = runtime.cpu().gpr[2];
    runtime.memory().store32(kCode, 0x24020002u); // addiu v0, zero, 2
    psprecomp::jit_invalidate(runtime, kCode, kCode + 4u);
    runtime.run(kCode, 1000u);
    const std::uint32_t second = runtime.cpu().gpr[2];
    // A host function registered over compiled code takes over the address.
    runtime.register_function(kCode, &done, "test_override");
    runtime.cpu().gpr[2] = 0u;
    runtime.run(kCode, 1000u);
    std::cerr.rdbuf(previous);
    check(first == 1u && second == 2u, "invalidation: rewritten code is compiled again");
    check(runtime.cpu().gpr[2] == 0u, "invalidation: a host function registered over compiled code wins");
}

std::vector<std::uint32_t> g_hook_calls;
void count_hook(Runtime &, AllegrexContext &ctx, std::uint32_t address) {
    g_hook_calls.push_back(address);
    ctx.gpr[9] += 100u; // seen by the instruction after the hook
}

void step_registered(Runtime &, AllegrexContext &ctx) {
    ctx.gpr[10] = 0x5Au;
    ctx.pc = kSentinel;
}

// Instruction hooks, and code compiled for hooks that is registered nowhere.
void hooks(Generator &g) {
    psprecomp::set_jit_enabled(true);
    psprecomp::set_jit_instruction_hook(&count_hook);
    Runtime &runtime = engine(true);
    psprecomp::jit_invalidate(runtime, kCode, kCode + 0x10000u);
    const std::vector<std::uint32_t> code = {
        0x24090001u, // addiu t1, zero, 1
        0x25290002u, // addiu t1, t1, 2   <- hooked
        0x3C1F0880u, // lui ra, 0x0880
        0x03E00008u, // jr ra
        0u,
    };
    for (std::size_t i = 0; i < code.size(); ++i)
        runtime.memory().store32(kCode + static_cast<std::uint32_t>(i) * 4u, code[i]);
    Setup setup = g.start({});
    runtime.cpu() = setup.ctx;
    psprecomp::jit_hook_instruction(runtime, kCode + 4u);
    g_hook_calls.clear();
    std::ostringstream discard;
    std::streambuf *previous = std::cerr.rdbuf(discard.rdbuf());
    runtime.run(kCode, 1000u);
    check(g_hook_calls == std::vector<std::uint32_t>{kCode + 4u} && runtime.cpu().gpr[9] == 103u,
        "hooks: the instruction hook runs before its instruction, with its address");
    psprecomp::jit_unhook_instruction(runtime, kCode + 4u);
    runtime.cpu() = setup.ctx;
    runtime.run(kCode, 1000u);
    check(g_hook_calls.size() == 1u && runtime.cpu().gpr[9] == 3u, "hooks: a removed hook runs no more");

    // Detached: from the entry up to the next registered address.
    runtime.register_function(kCode + 8u, &step_registered, "test_step");
    const psprecomp::JitFunction detached = psprecomp::jit_compile_detached(runtime, kCode);
    check(detached != nullptr, "hooks: code compiles detached");
    if (detached != nullptr) {
        AllegrexContext ctx = setup.ctx;
        ctx.pc = kCode;
        detached(runtime, ctx);
        check(ctx.gpr[9] == 3u && ctx.pc == kCode + 8u, "hooks: detached code stops at the next registered address");
        check(!runtime.has_function(kCode), "hooks: detached code is registered nowhere");
    }
    runtime.unregister_functions(kCode + 8u, kCode + 12u);
    std::cerr.rdbuf(previous);
    psprecomp::set_jit_instruction_hook(nullptr);
}

// A hook on an instruction in a delay slot runs before it, and a jump keeps
// the target it read before the slot.
void slot_hooks(Generator &g) {
    psprecomp::set_jit_enabled(true);
    psprecomp::set_jit_instruction_hook(&count_hook);
    Runtime &runtime = engine(true);
    psprecomp::jit_invalidate(runtime, kCode, kCode + 0x10000u);
    const std::vector<std::uint32_t> code = {
        0x24090001u, // addiu t1, zero, 1
        0x10000002u, // b +2 (to kCode + 16)
        0x25290002u, // addiu t1, t1, 2   <- hooked, in the slot
        0u,
        0x3C1F0880u, // lui ra, 0x0880
        0x03E00008u, // jr ra
        0x25290010u, // addiu t1, t1, 16  <- hooked, in the slot
    };
    for (std::size_t i = 0; i < code.size(); ++i)
        runtime.memory().store32(kCode + static_cast<std::uint32_t>(i) * 4u, code[i]);
    Setup setup = g.start({});
    runtime.cpu() = setup.ctx;
    psprecomp::jit_hook_instruction(runtime, kCode + 8u);
    psprecomp::jit_hook_instruction(runtime, kCode + 24u);
    g_hook_calls.clear();
    std::ostringstream discard;
    std::streambuf *previous = std::cerr.rdbuf(discard.rdbuf());
    runtime.run(kCode, 1000u);
    std::cerr.rdbuf(previous);
    check(g_hook_calls == std::vector<std::uint32_t>{kCode + 8u, kCode + 24u} && runtime.cpu().gpr[9] == 219u &&
            runtime.stop_reason() == "program returned",
        "hooks: instructions in delay slots run their hooks");
    psprecomp::jit_unhook_instruction(runtime, kCode + 8u);
    psprecomp::jit_unhook_instruction(runtime, kCode + 24u);
    psprecomp::set_jit_instruction_hook(nullptr);
}

// A hook that removes itself drops the code that called it; the code must
// still be there when the hook returns into it.
void unhook_self(Runtime &runtime, AllegrexContext &ctx, std::uint32_t address) {
    g_hook_calls.push_back(address);
    ctx.gpr[9] += 100u;
    psprecomp::jit_unhook_instruction(runtime, address);
}

// Dropped code is kept, detached code says when it is stale, and a flush
// drops only what changed.
void retirement(Generator &g) {
    psprecomp::set_jit_enabled(true);
    Runtime &runtime = engine(true);
    psprecomp::jit_invalidate(runtime, kCode, kCode + 0x10000u);
    const std::vector<std::uint32_t> code = {
        0x24090001u, // addiu t1, zero, 1
        0x25290002u, // addiu t1, t1, 2   <- hooked
        0x3C1F0880u, // lui ra, 0x0880
        0x03E00008u, // jr ra
        0u,
    };
    for (std::size_t i = 0; i < code.size(); ++i)
        runtime.memory().store32(kCode + static_cast<std::uint32_t>(i) * 4u, code[i]);
    Setup setup = g.start({});
    std::ostringstream discard;
    std::streambuf *previous = std::cerr.rdbuf(discard.rdbuf());

    psprecomp::set_jit_instruction_hook(&unhook_self);
    psprecomp::jit_hook_instruction(runtime, kCode + 4u);
    g_hook_calls.clear();
    runtime.cpu() = setup.ctx;
    runtime.run(kCode, 1000u);
    check(runtime.stop_reason() == "program returned" && runtime.cpu().gpr[9] == 103u && g_hook_calls.size() == 1u,
        "retirement: a hook that removes itself returns into its code and the program finishes");
    runtime.cpu() = setup.ctx;
    runtime.run(kCode, 1000u);
    check(g_hook_calls.size() == 1u && runtime.cpu().gpr[9] == 3u, "retirement: the removed hook runs no more");
    psprecomp::set_jit_instruction_hook(nullptr);

    const psprecomp::JitFunction detached = psprecomp::jit_compile_detached(runtime, kCode);
    check(psprecomp::jit_detached_current(runtime, detached), "retirement: fresh detached code is current");
    const std::size_t retired = psprecomp::jit_stats().retired;
    psprecomp::jit_invalidate(runtime, 0u, 0xFFFFFFFFu);
    check(!psprecomp::jit_detached_current(runtime, detached), "retirement: invalidated detached code is stale");
    check(psprecomp::jit_stats().retired > retired, "retirement: invalidated code is kept, not freed");
    if (detached != nullptr) {
        AllegrexContext ctx = setup.ctx;
        ctx.pc = kCode;
        detached(runtime, ctx);
        check(ctx.pc == kSentinel && ctx.gpr[9] == 3u, "retirement: stale detached code still runs");
    }

    // A flush over unchanged code keeps it; over changed code drops it.
    runtime.cpu() = setup.ctx;
    runtime.run(kCode, 1000u);
    const std::uint64_t dropped = psprecomp::jit_stats().invalidated;
    psprecomp::jit_memory_changed(runtime, 0u, 0xFFFFFFFFu);
    check(psprecomp::jit_stats().invalidated == dropped && runtime.has_function(kCode),
        "retirement: a flush keeps code whose bytes did not change");
    runtime.memory().store32(kCode, 0x24090005u); // addiu t1, zero, 5
    psprecomp::jit_memory_changed(runtime, kCode, kCode + 4u);
    check(psprecomp::jit_stats().invalidated == dropped + 1u, "retirement: a flush drops code whose bytes changed");
    runtime.cpu() = setup.ctx;
    runtime.run(kCode, 1000u);
    check(runtime.cpu().gpr[9] == 7u, "retirement: the changed code is compiled again");
    std::cerr.rdbuf(previous);
}

} // namespace

// A load from outside guest memory stops the runtime and names the guest
// instruction that made it, in compiled code as in the interpreter's report.
void memory_errors(Generator &g) {
    Setup setup = g.start({0x3C084C3Bu, 0x8D090004u}); // lui t0, 0x4C3B; lw t1, 4(t0)
    const Outcome out = run(setup, true);
    check(out.fatal && out.reason.find("(at guest pc " + psprecomp::hex32(kCode + 4u) + ")") != std::string::npos,
        "memory errors: compiled code names the instruction that read outside memory");
}

int main(int argc, char **argv) {
    Generator g(0x59414B55u);
    // --compile FILE BASE PC: compiles the code at PC of an image loaded at
    // BASE, for debugging the emitter on real code.
    if (argc == 5 && std::string(argv[1]) == "--compile") {
        std::FILE *file = std::fopen(argv[2], "rb");
        if (file == nullptr) return 2;
        std::vector<std::uint8_t> image;
        for (int c; (c = std::fgetc(file)) != EOF;) image.push_back(static_cast<std::uint8_t>(c));
        std::fclose(file);
        Runtime runtime(64u * 1024u * 1024u);
        const auto base = static_cast<std::uint32_t>(std::stoul(argv[3], nullptr, 16));
        runtime.memory().copy_in(base, image);
        psprecomp::set_jit_enabled(true);
        const bool compiled =
            psprecomp::jit_compile(runtime, static_cast<std::uint32_t>(std::stoul(argv[4], nullptr, 16)));
        std::printf("%s\n", compiled ? "compiled" : "not compiled");
        return compiled ? 0 : 1;
    }
    // --only N jit|interpreter: one straight-line program, for debugging.
    if (argc == 4 && std::string(argv[1]) == "--only") {
        const unsigned wanted = static_cast<unsigned>(std::stoul(argv[2]));
        for (unsigned program = 0;; ++program) {
            std::vector<std::uint32_t> code;
            for (unsigned i = 0; i < 48u; ++i) code.push_back(g.simple());
            Setup setup = g.start(code);
            if (program != wanted) continue;
            const Outcome out = run(setup, std::string(argv[3]) == "jit");
            std::printf("stopped: %s, pc %08X\n", out.reason.c_str(), out.ctx.pc);
            return 0;
        }
    }
    // --bench jit|interpreter: a loop of loads, integer and single-precision
    // arithmetic and stores, run a million times; prints nanoseconds per guest
    // instruction. PSPRECOMP_JIT_NO_REGISTER_CACHE and _NO_NATIVE_FPU compare
    // the JIT without its optimisations.
    if (argc == 3 && std::string(argv[1]) == "--bench") {
        Setup setup = g.start({
            0x3C080010u, // lui t0, 0x10: a million iterations
            0x8FA90000u, // lw t1, 0(sp)
            0x8FAA0004u, // lw t2, 4(sp)
            0x012A5821u, // addu t3, t1, t2
            0x000B6080u, // sll t4, t3, 2
            0x018B6826u, // xor t5, t4, t3
            0x46020000u, // add.s f0, f0, f2
            0x46040842u, // mul.s f1, f1, f4
            0xAFAD0008u, // sw t5, 8(sp)
            0x2508FFFFu, // addiu t0, t0, -1
            0x1500FFF6u, // bne t0, zero, -10
            0x01695021u, // addu t2, t3, t1 (delay slot)
        });
        constexpr double kInstructions = 11.0 * 0x100000;
        for (float &f : setup.ctx.fpr) f = 1.0f;
        const auto start = std::chrono::steady_clock::now();
        const Outcome out = run(setup, std::string(argv[2]) == "jit", 1000000000u);
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
        std::printf("%s: %.2f ns per guest instruction (%s)\n", argv[2], ns / kInstructions, out.reason.c_str());
        return out.stopped_normally ? 0 : 1;
    }
    straight_line(g);
    floating_point(g);
    branches(g);
    loop(g);
    preemption(g);
    self_modifying(g);
    loop_with_call(g);
    calls(g);
    invalidation(g);
    hooks(g);
    slot_hooks(g);
    retirement(g);
    memory_errors(g);
    const psprecomp::JitStats stats = psprecomp::jit_stats();
    std::printf("jit: %llu functions, %llu instructions, %llu through the shared path\n",
        static_cast<unsigned long long>(stats.compiled), static_cast<unsigned long long>(stats.instructions),
        static_cast<unsigned long long>(stats.shared));
    std::printf("%s\n", failures == 0 ? "all jit checks passed" : "jit checks FAILED");
    return failures == 0 ? 0 : 1;
}
