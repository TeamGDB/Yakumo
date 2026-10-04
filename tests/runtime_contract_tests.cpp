#include "psprecomp/decoder.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
constexpr std::uint32_t base = 0x08804000u;
constexpr std::uint32_t scratch = base + 0x1000u;
void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
constexpr std::uint32_t r(unsigned rs, unsigned rt, unsigned rd, unsigned shift, unsigned function) {
    return rs << 21u | rt << 16u | rd << 11u | shift << 6u | function;
}
constexpr std::uint32_t i(unsigned opcode, unsigned rs, unsigned rt, std::uint16_t immediate) {
    return opcode << 26u | rs << 21u | rt << 16u | immediate;
}
void step(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &cpu, std::uint32_t word) {
    runtime.memory().store32(base, word);
    cpu.pc = base;
    check(psprecomp::interpret_allegrex(runtime, cpu, 1u) == psprecomp::InterpreterExit::Budget,
        "Single instruction did not advance to its bounded next PC");
    check(cpu.pc == base + 4u && cpu.gpr[0] == 0u, "Instruction corrupted PC or zero register");
}
void integer_contracts() {
    psprecomp::Runtime runtime;
    psprecomp::AllegrexContext cpu{};
    struct Case {
        std::uint32_t word;
        std::uint32_t left;
        std::uint32_t right;
        std::uint32_t expected;
        unsigned destination;
    };
    const Case cases[] = {{r(8, 9, 10, 0, 0x20), 17, 3, 20, 10}, {r(8, 9, 10, 0, 0x21), 0xFFFFFFFF, 1, 0, 10},
        {r(8, 9, 10, 0, 0x22), 17, 3, 14, 10}, {r(8, 9, 10, 0, 0x23), 0, 1, 0xFFFFFFFF, 10},
        {r(8, 9, 10, 0, 0x24), 0xF0, 0x3C, 0x30, 10}, {r(8, 9, 10, 0, 0x25), 0xF0, 0x3C, 0xFC, 10},
        {r(8, 9, 10, 0, 0x26), 0xF0, 0x3C, 0xCC, 10}, {r(8, 9, 10, 0, 0x27), 0xF0, 0x3C, 0xFFFFFF03, 10},
        {r(8, 9, 10, 0, 0x2A), 0xFFFFFFFF, 1, 1, 10}, {r(8, 9, 10, 0, 0x2B), 0xFFFFFFFF, 1, 0, 10},
        {r(8, 9, 10, 0, 0x2C), 0xFFFFFFFF, 1, 1, 10}, {r(8, 9, 10, 0, 0x2D), 0xFFFFFFFF, 1, 0xFFFFFFFF, 10},
        {r(8, 9, 10, 0, 0x0A), 123, 0, 123, 10}, {r(8, 9, 10, 0, 0x0B), 123, 1, 123, 10},
        {r(0, 9, 10, 4, 0), 0, 0x123, 0x1230, 10}, {r(0, 9, 10, 4, 2), 0, 0x123, 0x12, 10},
        {r(0, 9, 10, 4, 3), 0, 0x80000000, 0xF8000000, 10}, {r(1, 9, 10, 4, 2), 0, 0x12345678, 0x81234567, 10},
        {r(8, 9, 10, 0, 4), 36, 0x123, 0x1230, 10}, {r(8, 9, 10, 0, 6), 36, 0x123, 0x12, 10},
        {r(8, 9, 10, 0, 7), 36, 0x80000000, 0xF8000000, 10}, {r(8, 9, 10, 1, 6), 36, 0x12345678, 0x81234567, 10},
        {r(8, 0, 10, 0, 0x16), 0x00FFFFFF, 0, 8, 10}, {r(8, 0, 10, 0, 0x17), 0xFF000000, 0, 8, 10},
        {i(9, 8, 10, 0xFFFF), 123, 0, 122, 10}, {i(10, 8, 10, 0), 0xFFFFFFFF, 0, 1, 10},
        {i(11, 8, 10, 0xFFFF), 1, 0, 1, 10}, {i(12, 8, 10, 0xFF), 0x1234, 0, 0x34, 10},
        {i(13, 8, 10, 0xFF), 0x1234, 0, 0x12FF, 10}, {i(14, 8, 10, 0xFF), 0x1234, 0, 0x12CB, 10},
        {i(15, 0, 10, 0x1234), 0, 0, 0x12340000, 10}, {(0x1Fu << 26) | r(8, 9, 7, 8, 0), 0x12345678, 0, 0x56, 9},
        {(0x1Fu << 26) | r(8, 9, 15, 8, 4), 0xAB, 0x12345678, 0x1234AB78, 9},
        {(0x1Fu << 26) | r(0, 9, 10, 16, 0x20), 0, 0x80, 0xFFFFFF80, 10},
        {(0x1Fu << 26) | r(0, 9, 10, 24, 0x20), 0, 0x8000, 0xFFFF8000, 10},
        {(0x1Fu << 26) | r(0, 9, 10, 20, 0x20), 0, 1, 0x80000000, 10},
        {(0x1Fu << 26) | r(0, 9, 10, 2, 0x20), 0, 0x12345678, 0x34127856, 10},
        {(0x1Fu << 26) | r(0, 9, 10, 3, 0x20), 0, 0x12345678, 0x78563412, 10}};
    for (const auto &test : cases) {
        cpu.gpr[8] = test.left;
        cpu.gpr[9] = test.right;
        cpu.gpr[10] = 0;
        step(runtime, cpu, test.word);
        if (cpu.gpr[test.destination] != test.expected) {
            std::cerr << "word=" << std::hex << test.word << " actual=" << cpu.gpr[test.destination]
                      << " expected=" << test.expected << '\n';
            check(false, "Integer instruction disagrees with architectural result");
        }
    }
    for (unsigned function : {0x0Au, 0x0Bu}) {
        cpu.gpr[8] = 123;
        cpu.gpr[9] = function == 0x0Au ? 1u : 0u;
        cpu.gpr[10] = 77;
        step(runtime, cpu, r(8, 9, 10, 0, function));
        check(cpu.gpr[10] == 77, "Conditional move must preserve destination when condition is false");
    }
    for (unsigned function : {0x20u, 0x22u}) {
        psprecomp::Runtime overflow;
        psprecomp::AllegrexContext state{};
        state.gpr[8] = 0x7FFFFFFFu;
        state.gpr[9] = function == 0x20u ? 1u : 0xFFFFFFFFu;
        overflow.memory().store32(base, r(8, 9, 10, 0, function));
        state.pc = base;
        check(psprecomp::interpret_allegrex(overflow, state, 1) == psprecomp::InterpreterExit::Stopped &&
                overflow.stop_reason().find("overflow") != std::string::npos,
            "Signed overflow must stop with diagnostic");
    }
    for (unsigned function : {0x1Au, 0x1Bu}) {
        for (auto pair :
            {std::pair{17u, 5u}, std::pair{0x80000000u, 0xFFFFFFFFu}, std::pair{17u, 0u}, std::pair{0xFFFFFFEFu, 0u}}) {
            cpu.gpr[8] = pair.first;
            cpu.gpr[9] = pair.second;
            step(runtime, cpu, r(8, 9, 0, 0, function));
            if (pair.second == 0)
                check(cpu.hi == pair.first &&
                        cpu.lo == (function == 0x1Bu || static_cast<std::int32_t>(pair.first) >= 0 ? 0xFFFFFFFFu : 1u),
                    "Divide zero result changed");
            else if (function == 0x1Au && pair.first == 0x80000000u)
                check(cpu.lo == 0x80000000u && cpu.hi == 0, "Signed division overflow result changed");
            else
                check(cpu.lo == pair.first / pair.second && cpu.hi == pair.first % pair.second,
                    "Division quotient or remainder changed");
        }
    }
    cpu.gpr[8] = 123;
    step(runtime, cpu, r(8, 0, 0, 0, 0x11));
    check(cpu.hi == 123, "MTHI failed");
    step(runtime, cpu, r(8, 0, 0, 0, 0x13));
    check(cpu.lo == 123, "MTLO failed");
    for (unsigned function : {0x1Cu, 0x1Du, 0x2Eu, 0x2Fu}) {
        cpu.gpr[8] = 3;
        cpu.gpr[9] = 5;
        cpu.hi = 0;
        cpu.lo = 20;
        step(runtime, cpu, r(8, 9, 0, 0, function));
        check(cpu.lo == (function == 0x1Cu || function == 0x1Du ? 35u : 5u) && cpu.hi == 0,
            "Multiply accumulator result changed");
    }
}
void memory_contracts() {
    psprecomp::Runtime runtime;
    psprecomp::AllegrexContext cpu{};
    cpu.gpr[8] = scratch;
    runtime.memory().store32(scratch, 0x87654380u);
    for (auto entry : {std::pair{0x20u, 0xFFFFFF80u}, std::pair{0x24u, 0x80u}, std::pair{0x21u, 0x4380u},
             std::pair{0x25u, 0x4380u}, std::pair{0x23u, 0x87654380u}}) {
        step(runtime, cpu, i(entry.first, 8, 9, 0));
        check(cpu.gpr[9] == entry.second, "Signed/unsigned guest load changed");
    }
    cpu.gpr[9] = 0xFEDCBA98;
    for (auto entry : {std::pair{0x28u, 0x98u}, std::pair{0x29u, 0xBA98u}, std::pair{0x2Bu, 0xFEDCBA98u}}) {
        runtime.memory().store32(scratch, 0);
        step(runtime, cpu, i(entry.first, 8, 9, 0));
        check(runtime.memory().load32(scratch) == entry.second, "Guest store wrote wrong width");
    }
    cpu.fpr[9] = 1.5f;
    step(runtime, cpu, i(0x39u, 8, 9, 0));
    cpu.fpr[9] = 0;
    step(runtime, cpu, i(0x31u, 8, 9, 0));
    check(cpu.fpr[9] == 1.5f, "COP1 memory transfer changed bits");
    step(runtime, cpu, i(0x2Fu, 8, 0, 0));
    step(runtime, cpu, r(0, 0, 0, 0, 0xFu));
}
void vector_contracts() {
    psprecomp::Runtime runtime;
    psprecomp::AllegrexContext cpu{};
    const auto vector = [](unsigned opcode, unsigned operation, unsigned destination = 2u) {
        return (opcode << 26u) | (operation << 23u) | (1u << 16u) | (0u << 8u) | destination;
    };
    const auto scalar = [](unsigned group, unsigned operation, unsigned destination = 2u) {
        return (0x34u << 26u) | (group << 21u) | (operation << 16u) | destination;
    };
    const auto source = [&](float left, float right) {
        cpu.eat_vfpu_prefixes();
        cpu.set_vfpu_scalar_bits(0u, std::bit_cast<std::uint32_t>(left));
        cpu.set_vfpu_scalar_bits(1u, std::bit_cast<std::uint32_t>(right));
    };
    const auto result = [&] { return std::bit_cast<float>(cpu.vfpu_scalar_bits(2u)); };
    struct Arithmetic {
        unsigned opcode;
        unsigned operation;
        float expected;
    };
    for (auto test : {Arithmetic{0x18, 0, 8}, Arithmetic{0x18, 1, 4}, Arithmetic{0x18, 7, 3}, Arithmetic{0x19, 0, 12},
             Arithmetic{0x19, 1, 12}, Arithmetic{0x19, 2, 12}, Arithmetic{0x19, 4, 2}, Arithmetic{0x1B, 2, 2},
             Arithmetic{0x1B, 3, 6}, Arithmetic{0x1B, 5, 1}, Arithmetic{0x1B, 6, 1}, Arithmetic{0x1B, 7, 0}}) {
        source(6, 2);
        step(runtime, cpu, vector(test.opcode, test.operation));
        check(result() == test.expected, "VFPU scalar arithmetic disagrees with known operands");
    }
    struct Unary {
        unsigned operation;
        float input;
        float expected;
    };
    for (auto test : {Unary{0, -2, -2}, Unary{1, -2, 2}, Unary{2, -2, 2}, Unary{4, -2, 0}, Unary{4, 2, 1},
             Unary{4, 0.5f, 0.5f}, Unary{5, -2, -1}, Unary{5, 2, 1}, Unary{5, 0.5f, 0.5f}, Unary{16, 2, 0.5f},
             Unary{17, 4, 0.5f}, Unary{18, 1, 1}, Unary{19, 0, 1}, Unary{20, 3, 8}, Unary{21, 8, 3}, Unary{22, 4, 2},
             Unary{23, 1, 1}, Unary{24, 2, -0.5f}, Unary{26, 1, -1}, Unary{28, 3, 0.125f}}) {
        source(test.input, 0);
        step(runtime, cpu, scalar(0, test.operation));
        check(std::abs(result() - test.expected) < 0.00001f, "VFPU unary known value changed");
    }
    for (unsigned op : {6u, 7u}) {
        source(123, 0);
        step(runtime, cpu, scalar(0, op));
        check(result() == (op == 6 ? 0 : 1), "VFPU fill changed");
    }
    source(0.25f, 0);
    step(runtime, cpu, scalar(2, 4));
    check(result() == 0.75f, "VFPU one complement changed");
    for (float value : {-2.0f, 0.0f, 2.0f}) {
        source(value, 0);
        step(runtime, cpu, scalar(2, 10));
        check(result() == value / 2.0f, "VFPU sign changed");
    }
    for (unsigned op : {6u, 7u}) {
        source(3, 0);
        step(runtime, cpu, scalar(2, op));
        check(result() == (op == 6u ? 3.0f : 0.0f), "VFPU scalar horizontal sum/average hardware weighting changed");
    }
    for (unsigned mode : {16u, 17u, 18u, 19u}) {
        for (float input : {3.75f, std::numeric_limits<float>::quiet_NaN(), 1.0e30f, -1.0e30f}) {
            source(input, 0);
            step(runtime, cpu, scalar(mode, 0));
            auto bits = cpu.vfpu_scalar_bits(2);
            check(bits ==
                    (std::isnan(input) || input > 1.0e20f ? 0x7FFFFFFFu
                            : input < -1.0e20f            ? 0x80000000u
                            : mode == 17u || mode == 19u  ? 3u
                                                          : 4u),
                "VFPU integer conversion rounding/saturation changed");
        }
    }
    cpu.eat_vfpu_prefixes();
    cpu.set_vfpu_scalar_bits(0, 6);
    step(runtime, cpu, scalar(20, 1));
    check(result() == 3, "VFPU scaled integer conversion changed");
    step(runtime, cpu, (0x37u << 26u) | (6u << 23u) | (2u << 16u) | 0xFFFDu);
    check(result() == -3, "VIIM sign extension changed");
    for (auto pair : {std::pair{0x3C00u, 1.0f}, std::pair{0x8000u, -0.0f}, std::pair{0x0001u, std::ldexp(1.0f, -24)}}) {
        cpu.eat_vfpu_prefixes();
        step(runtime, cpu, (0x37u << 26u) | (7u << 23u) | (2u << 16u) | pair.first);
        check(std::bit_cast<std::uint32_t>(result()) == std::bit_cast<std::uint32_t>(pair.second),
            "VFIM half expansion changed");
    }
    source(0, 0);
    step(runtime, cpu, scalar(3, 8));
    check(std::abs(result() - 1.5707963f) < 0.00001f, "VFPU hardware pi/2 constant changed");
    cpu.gpr[8] = scratch;
    runtime.memory().store32(scratch, 0x3FC00000);
    step(runtime, cpu, i(0x32, 8, 2, 0));
    check(result() == 1.5f, "LV.S did not load bits");
    step(runtime, cpu, i(0x3A, 8, 2, 4));
    check(runtime.memory().load32(scratch + 4) == 0x3FC00000u, "SV.S did not store bits");
    for (unsigned lane = 0; lane < 4; ++lane)
        runtime.memory().store32(scratch + lane * 4, std::bit_cast<std::uint32_t>(static_cast<float>(lane + 1)));
    step(runtime, cpu, i(0x36, 8, 0, 0));
    step(runtime, cpu, i(0x3E, 8, 0, 16));
    for (unsigned lane = 0; lane < 4; ++lane)
        check(runtime.memory().load32(scratch + 16 + lane * 4) == runtime.memory().load32(scratch + lane * 4),
            "Quad transfer changed lane ordering");
    for (unsigned control = 0; control < 3; ++control) {
        step(runtime, cpu, (0x37u << 26u) | (control << 24u) | 0xFFFFFu);
        check(cpu.vfpu_ctrl[control] == (control == 2u ? 0xFFFu : 0xFFFFFu), "VFPU prefix control mask changed");
    }
    cpu.eat_vfpu_prefixes();
    for (unsigned operation : {3u, 6u, 7u}) {
        step(runtime, cpu, (0x3Cu << 26u) | (28u << 21u) | (operation << 16u) | 0x8080u | 4u);
        float matrix[16]{};
        cpu.read_vfpu_matrix(matrix, 4u, 4u);
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned row = 0; row < 4; ++row)
                check(matrix[column * 4 + row] ==
                        (operation == 3u          ? (row == column ? 1.0f : 0.0f)
                                : operation == 7u ? 1.0f
                                                  : 0.0f),
                    "VFPU matrix initialization disagrees with identity/zero/one contract");
    }
    cpu.eat_vfpu_prefixes();
    step(runtime, cpu, (0x37u << 26u) | (2u << 24u) | 0xFFFu);
    step(runtime, cpu, 0xFFFF0000u);
    check(cpu.vfpu_ctrl[2] == 0xFFFu, "VFLUSH special encoding must preserve prefixes");
    step(runtime, cpu, 0xFC000000u);
    check(cpu.vfpu_ctrl[2] == 0u, "VFLUSH must consume ordinary prefixes");
}
void floating_contracts() {
    psprecomp::Runtime runtime;
    psprecomp::AllegrexContext cpu{};
    const auto cop = [](unsigned format, unsigned ft, unsigned fs, unsigned fd, unsigned fn) {
        return (0x11u << 26) | r(format, ft, fs, fd, fn);
    };
    for (auto entry : {std::pair{0u, 7.5f}, std::pair{1u, 4.5f}, std::pair{2u, 9.0f}, std::pair{3u, 4.0f},
             std::pair{4u, std::sqrt(6.0f)}, std::pair{5u, 6.0f}, std::pair{6u, 6.0f}, std::pair{7u, -6.0f}}) {
        cpu.fpr[8] = 6.0f;
        cpu.fpr[9] = 1.5f;
        step(runtime, cpu, cop(16, 9, 8, 10, entry.first));
        check(cpu.fpr[10] == entry.second, "COP1 arithmetic result changed");
    }
    for (auto entry :
        {std::pair{12u, 4u}, std::pair{13u, 3u}, std::pair{14u, 4u}, std::pair{15u, 3u}, std::pair{36u, 4u}}) {
        cpu.fpr[8] = 3.75f;
        step(runtime, cpu, cop(16, 0, 8, 10, entry.first));
        check(cpu.fpr_bits(10) == entry.second, "COP1 integer rounding changed");
    }
    cpu.set_fpr_bits(8, 0xFFFFFFFDu);
    step(runtime, cpu, cop(20, 0, 8, 10, 32));
    check(cpu.fpr[10] == -3.0f, "COP1 word conversion changed");
    cpu.gpr[9] = 0x3FC00000u;
    step(runtime, cpu, cop(4, 9, 8, 0, 0));
    check(cpu.fpr[8] == 1.5f, "MTC1 changed bits");
    step(runtime, cpu, cop(0, 10, 8, 0, 0));
    check(cpu.gpr[10] == 0x3FC00000u, "MFC1 changed bits");
    cpu.gpr[9] = 0xFFFFFFFFu;
    step(runtime, cpu, cop(6, 9, 31, 0, 0));
    step(runtime, cpu, cop(2, 10, 31, 0, 0));
    check(cpu.gpr[10] == 0x0181FFFFu, "FCR31 write mask changed");
    cpu.fpr[8] = std::numeric_limits<float>::infinity();
    cpu.fpr[9] = 0;
    step(runtime, cpu, cop(16, 9, 8, 10, 2));
    check(cpu.fpr_bits(10) == 0x7FC00000u, "Infinity times zero must produce architectural quiet NaN");
}
}
int main() {
    try {
        integer_contracts();
        memory_contracts();
        floating_contracts();
        vector_contracts();
        std::cout << "Runtime instruction contracts passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
