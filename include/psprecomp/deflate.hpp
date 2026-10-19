#pragma once

#include "psprecomp/guest_memory.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace psprecomp {

enum class RawDeflateStatus : std::uint8_t {
    Ok,
    OutputOverflow,
    InvalidData,
};

struct RawDeflateResult {
    RawDeflateStatus status{RawDeflateStatus::InvalidData};
    std::uint32_t output_size{};
    std::uint32_t input_consumed{};
};

// Decodes one RFC 1951 raw DEFLATE stream directly between PSP guest-memory
// ranges. The function stops at BFINAL, reports the first byte after the
// compressed bitstream, and uses GuestMemory's exact forward-overlap copy for
// LZ matches. It has no zlib/miniz dependency and is therefore identical on
// Linux and MSVC builds.
[[nodiscard]] RawDeflateResult inflate_raw_deflate(
    GuestMemory &memory, std::uint32_t output_address, std::uint32_t output_capacity, std::uint32_t input_address);

// The same decoder between host buffers: `output` gets the decoded bytes, at
// most `output_capacity` of them (OutputOverflow past that), for reading
// archives such as .zip without a guest in the way.
[[nodiscard]] RawDeflateResult inflate_raw_deflate(
    std::span<const std::uint8_t> input, std::vector<std::uint8_t> &output, std::uint32_t output_capacity);

} // namespace psprecomp
