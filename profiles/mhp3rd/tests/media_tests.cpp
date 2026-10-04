#include "audio/sas_core.hpp"
#include "movie/psmf_demuxer.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
std::vector<std::uint8_t> pack(
    std::uint8_t stream, const std::vector<std::uint8_t> &payload, std::int64_t pts = -1, std::int64_t dts = -1) {
    std::vector<std::uint8_t> result(14u, 0u);
    result[2] = 1u;
    result[3] = 0xBAu;
    std::vector<std::uint8_t> body{0x80u, static_cast<std::uint8_t>(pts < 0 ? 0 : (dts < 0 ? 0x80u : 0xC0u)),
        static_cast<std::uint8_t>(pts < 0 ? 0 : (dts < 0 ? 5 : 10))};
    auto stamp = [&](std::int64_t value) {
        body.push_back(static_cast<std::uint8_t>(((value >> 30) & 7) << 1 | 1));
        body.push_back(static_cast<std::uint8_t>(value >> 22));
        body.push_back(static_cast<std::uint8_t>(((value >> 15) & 0x7F) << 1 | 1));
        body.push_back(static_cast<std::uint8_t>(value >> 7));
        body.push_back(static_cast<std::uint8_t>((value & 0x7F) << 1 | 1));
    };
    if (pts >= 0) stamp(pts);
    if (dts >= 0) stamp(dts);
    body.insert(body.end(), payload.begin(), payload.end());
    result.insert(result.end(),
        {0u, 0u, 1u, stream, static_cast<std::uint8_t>(body.size() >> 8), static_cast<std::uint8_t>(body.size())});
    result.insert(result.end(), body.begin(), body.end());
    return result;
}
void demux_contracts() {
    mhp3rd::movie::PsmfDemuxer demux;
    check(!demux.push_pack({}), "Empty packs must be rejected");
    std::vector<std::uint8_t> invalid(32u, 0xFFu);
    check(!demux.push_pack(invalid), "Invalid pack signature accepted");
    check(!demux.pop_video() && !demux.pop_audio(), "Empty queues must return no unit");
    const std::vector<std::uint8_t> first{0u, 0u, 0u, 1u, 9u, 0xF0u, 0x65u, 0xAAu};
    const std::vector<std::uint8_t> second{0u, 0u, 1u, 9u, 0xF0u, 0x41u, 0xBBu};
    check(demux.push_pack(pack(0xE0u, first, 90000, 87000)), "Video pack rejected");
    check(!demux.video_ready(), "Incomplete final picture must wait for delimiter");
    check(demux.push_pack(pack(0xE0u, {0u, 0u})), "Split delimiter prefix rejected");
    check(demux.push_pack(pack(0xE0u, {1u, 9u, 0xF0u, 0x41u, 0xBBu})), "Split delimiter tail rejected");
    auto video = demux.pop_video();
    check(video && video->data == first && video->pts == 90000 && video->dts == 87000,
        "Picture ownership or PES timestamps changed");
    demux.end_of_stream();
    video = demux.pop_video();
    check(video && video->data == second && video->pts == 93003 && video->dts == 90003,
        "Final picture or extrapolated timestamps changed");
    check(!demux.video_ready(), "Video queue did not drain");
    demux.reset();
    const std::vector<std::uint8_t> frame{0x0Fu, 0xD0u, 8u, 0u, 0u, 0u, 0u, 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    std::vector<std::uint8_t> audio{0u, 0u, 0u, 0u, 0xFFu};
    audio.insert(audio.end(), frame.begin(), frame.begin() + 10);
    demux.push_pack(pack(0xBDu, audio, 45000));
    check(!demux.audio_ready(), "Partial ATRAC frame must remain buffered");
    audio = {0u, 0u, 0u, 0u};
    audio.insert(audio.end(), frame.begin() + 10, frame.end());
    audio.insert(audio.end(), frame.begin(), frame.end());
    demux.push_pack(pack(0xBDu, audio));
    auto unit = demux.pop_audio();
    check(unit && unit->data == std::vector<std::uint8_t>(frame.begin() + 8, frame.end()) && unit->pts == 45000,
        "ATRAC resynchronization or payload ownership failed");
    check(demux.audio_frame_size() == 16u && demux.audio_channels() == 2u, "ATRAC header parameters changed");
    unit = demux.pop_audio();
    check(unit && unit->pts == 45000 + 2048 * 90000 / 44100 && unit->dts == unit->pts,
        "Audio timestamps must follow sample duration");
    check(!demux.pop_audio(), "Audio queue did not drain");
    demux.reset();
    check(demux.audio_frame_size() == 0u && demux.audio_channels() == 0u, "Reset leaked prior stream metadata");
    demux.push_pack(pack(0xBEu, {1u, 2u}));
    demux.push_pack(pack(0xBDu, {1u, 0u, 0u, 0u, 0u}));
    auto malformed = pack(0xE0u, {1u});
    malformed[22] = 255u;
    demux.push_pack(malformed);
    check(!demux.video_ready() && !demux.audio_ready(), "Unsupported/truncated PES created output");
    demux.push_pack(pack(0xE0u, second));
    demux.end_of_stream();
    video = demux.pop_video();
    check(video && video->pts == -1 && video->dts == -1, "Unknown timestamps must stay unknown");
}
void sas_contracts() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t address = 0x08804000u;
    mhp3rd::audio::SasCore core;
    core.init(0u, 1u, 3u);
    check(core.grain() == 256u && core.output_mode() == 3u, "SAS initialization defaults changed");
    std::array<std::int16_t, 512> output{};
    core.render(memory, output.data(), 256u);
    check(std::all_of(output.begin(), output.end(), [](auto sample) { return sample == 0; }), "Idle SAS is not silent");
    for (std::uint32_t i = 0; i < 128u; ++i) memory.store16(address + i * 2u, 12000u);
    core.set_voice_pcm(0u, address, 256u, 0);
    core.set_volume(0u, 0x1000, -0x1000);
    core.key_on(0u);
    core.render(memory, output.data(), 256u);
    check(output[0] > 11990 && output[1] < -11990 && (core.end_flag() & 1u) == 0u,
        "PCM looping, channel gain or signed volume changed");
    core.set_pause(1u, true);
    core.render(memory, output.data(), 4u);
    check(output[0] == 0 && output[1] == 0 && (core.end_flag() & 1u) == 0u, "Pause must preserve playing voice");
    core.set_pause(1u, false);
    core.key_off(0u);
    check((core.end_flag() & 1u) != 0u && core.envelope_height(0u) == 0, "Key off must stop a voice without ADSR");
    core.key_off(0u);
    core.init(128u, 2u, 0u);
    for (std::uint32_t voice = 0; voice < 2u; ++voice) {
        core.set_voice_pcm(voice, address, 256u, 0);
        core.set_volume(voice, 99999, 99999);
        core.key_on(voice);
    }
    core.render(memory, output.data(), 8u);
    check(output[0] > 23990 && output[0] < 24001, "Independent voices must add with bounded gain");
    memory.store16(address, 32767u);
    core.key_on(0u);
    core.key_on(1u);
    core.render(memory, output.data(), 1u);
    check(output[0] == 32767, "Mix saturation must not wrap");
    core.init(4096u, 0u, 0u);
    core.set_voice_pcm(0u, address, 4u, -1);
    core.key_on(0u);
    core.render(memory, output.data(), 8u);
    check((core.end_flag() & 1u) != 0u, "Finite PCM must end");
    core.set_voice_pcm(0u, 0u, 0u, 0);
    core.key_on(0u);
    check((core.end_flag() & 1u) != 0u, "Empty PCM must not play");
    core.set_voice_pcm(0u, 0xFFFFFFFFu, 16u, -1);
    core.key_on(0u);
    core.render(memory, output.data(), 8u);
    check(output[0] == 0, "Unavailable guest PCM must be silent");
    for (unsigned predictor = 0u; predictor < 6u; ++predictor) {
        std::array<std::uint8_t, 16> block{};
        block[0] = static_cast<std::uint8_t>((predictor << 4) | 15u);
        block[1] = 3u;
        std::fill(block.begin() + 2, block.end(), 0x11u);
        memory.copy_in(address, block);
        core.set_voice(0u, address, 16u, true);
        core.key_on(0u);
        core.render(memory, output.data(), 64u);
        check(output[0] > 0 && (core.end_flag() & 1u) == 0u, "Looping VAG predictor must produce signed PCM");
    }
    memory.store8(address + 1u, 7u);
    core.key_on(0u);
    core.render(memory, output.data(), 4u);
    check((core.end_flag() & 1u) != 0u, "VAG terminator must stop without samples");
    memory.store8(address + 1u, 1u);
    core.key_on(0u);
    core.render(memory, output.data(), 64u);
    check((core.end_flag() & 1u) != 0u, "Finite VAG must end after its final block");
    core.set_voice(0u, 0xFFFFFFFFu, 16u, false);
    core.key_on(0u);
    core.render(memory, output.data(), 4u);
    check((core.end_flag() & 1u) != 0u, "Unavailable VAG block must stop");
    core.set_voice_pcm(0u, address, 256u, 0);
    core.set_simple_adsr(0u, 0x000Fu, 0u);
    core.key_on(0u);
    check(core.envelope_height(0u) == 0, "ADSR attack must start from zero");
    core.render(memory, output.data(), 64u);
    check(core.envelope_height(0u) > 0, "ADSR attack/decay must enter sustain");
    core.key_off(0u);
    core.render(memory, output.data(), 64u);
    check(core.envelope_height(0u) == 0 && (core.end_flag() & 1u), "ADSR release must reach zero and stop");
    core.set_simple_adsr(0u, 0xFC00u, 0xFFE0u);
    core.key_on(0u);
    core.render(memory, output.data(), 256u);
    check(core.envelope_height(0u) >= 0, "Slow exponential ADSR must remain bounded");
    core.key_off(0u);
    core.render(memory, output.data(), 256u);
    core.set_pitch(0u, 0xFFFFu);
    core.set_voice(32u, 0u, 0u, false);
    core.set_voice_pcm(32u, 0u, 0u, 0);
    core.set_pitch(32u, 0u);
    core.set_volume(32u, 0, 0);
    core.set_simple_adsr(32u, 0u, 0u);
    core.key_on(32u);
    core.key_off(32u);
    check(core.envelope_height(32u) == 0, "Invalid voice index must be harmless");
    check(&mhp3rd::audio::sas_core(42u) == &mhp3rd::audio::sas_core(42u) &&
            &mhp3rd::audio::sas_core(42u) != &mhp3rd::audio::sas_core(43u),
        "SAS handles must preserve independent cores");
}
}
int main() {
    try {
        demux_contracts();
        sas_contracts();
        std::cout << "Media contracts passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
