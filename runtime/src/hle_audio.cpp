// sceAudio, sceSasCore, and the remaining sceUtility stubs (module loading, message dialog status).
//
// Blocking outputs take as long as the samples they play (virtual time), and their samples go to the frontend's
// AudioSink. sceSasCore is a native mixer (audio/sas.cpp); sceAtrac lives in hle_atrac.cpp.

#include <algorithm>
#include <cstdio>
#include <vector>

#include "audio/sas.hpp"
#include "ff7cc/runtime/audio.hpp"
#include "ff7cc/runtime/hle.hpp"
#include "ff7cc/runtime/kernel.hpp"

namespace ff7cc::rt {

namespace {

GuestMemory& mem() { return kernel().memory(); }

AudioSink* g_sink = nullptr;

struct AudioChannel {
    bool reserved = false, mono = false;
    std::uint32_t samples = 0;
    int left = 0x8000, right = 0x8000;
};
AudioChannel g_channels[8];
std::uint32_t g_output2_samples = 0;

void play_for(std::uint32_t samples) { kernel().wait(WaitType::Audio, 0, 0, false, samples * 1000000ull / 44100); }

// Converts guest samples (mono or stereo s16) to stereo with volumes (0x8000 = unity) and submits them.
void submit(int source, std::uint32_t buffer, std::uint32_t samples, bool mono, int left, int right) {
    if (!g_sink || !buffer || !samples) return;
    static thread_local std::vector<std::int16_t> frames;
    frames.resize(static_cast<std::size_t>(samples) * 2);
    const auto* in = reinterpret_cast<const std::int16_t*>(mem().ptr(buffer));
    for (std::uint32_t i = 0; i < samples; ++i) {
        const int l = mono ? in[i] : in[2 * i], r = mono ? in[i] : in[2 * i + 1];
        frames[2 * i] = static_cast<std::int16_t>(std::clamp((l * left) >> 15, -32768, 32767));
        frames[2 * i + 1] = static_cast<std::int16_t>(std::clamp((r * right) >> 15, -32768, 32767));
    }
    g_sink->submit(source, frames.data(), samples);
}

// ---- sceAudio ----------------------------------------------------------------------------------

void ChReserve(Context& c) {  // (channel or -1, samples, format: 0 stereo, 0x10 mono)
    int ch = static_cast<int>(arg(c, 0));
    if (ch < 0)
        for (ch = 0; ch < 8 && g_channels[ch].reserved; ++ch) {}
    if (ch >= 8) return ret(c, 0x80260002);
    g_channels[ch] = AudioChannel{true, arg(c, 2) == 0x10, arg(c, 1)};
    ret(c, static_cast<std::uint32_t>(ch));
}
void ChRelease(Context& c) {
    if (arg(c, 0) < 8) g_channels[arg(c, 0)].reserved = false;
    ret(c, 0);
}
void output(Context& c, int left, int right, std::uint32_t buffer, bool blocking) {
    const int ch = static_cast<int>(arg(c, 0) & 7);
    AudioChannel& a = g_channels[ch];
    submit(ch, buffer, a.samples, a.mono, left, right);
    if (blocking) play_for(a.samples);
    ret(c, a.samples);
}
void OutputBlocking(Context& c) {  // (channel, volume, buffer)
    output(c, static_cast<int>(arg(c, 1)), static_cast<int>(arg(c, 1)), arg(c, 2), true);
}
void OutputPannedBlocking(Context& c) {  // (channel, left, right, buffer)
    output(c, static_cast<int>(arg(c, 1)), static_cast<int>(arg(c, 2)), arg(c, 3), true);
}
void OutputPanned(Context& c) { output(c, static_cast<int>(arg(c, 1)), static_cast<int>(arg(c, 2)), arg(c, 3), false); }
void SetChannelDataLen(Context& c) {
    g_channels[arg(c, 0) & 7].samples = arg(c, 1);
    ret(c, 0);
}
void ChangeChannelConfig(Context& c) {
    g_channels[arg(c, 0) & 7].mono = arg(c, 1) == 0x10;
    ret(c, 0);
}
void ChangeChannelVolume(Context& c) {
    g_channels[arg(c, 0) & 7].left = static_cast<int>(arg(c, 1));
    g_channels[arg(c, 0) & 7].right = static_cast<int>(arg(c, 2));
    ret(c, 0);
}
void Output2Reserve(Context& c) {
    g_output2_samples = arg(c, 0);
    ret(c, 0);
}
void Output2ChangeLength(Context& c) {
    g_output2_samples = arg(c, 0);
    ret(c, 0);
}
void Output2OutputBlocking(Context& c) {  // (volume, buffer)
    const int volume = static_cast<int>(arg(c, 0));
    submit(AudioSink::kOutput2, arg(c, 1), g_output2_samples, false, volume, volume);
    play_for(g_output2_samples);
    ret(c, g_output2_samples);
}
void ReturnZero(Context& c) { ret(c, 0); }

// ---- sceSasCore --------------------------------------------------------------------------------

constexpr std::uint32_t kSasInvalidVoice = 0x80420010;
audio::Sas g_sas;

audio::Voice* sas_voice(Context& c) {
    audio::Voice* v = g_sas.voice(static_cast<int>(arg(c, 1)));
    if (!v) ret(c, kSasInvalidVoice);
    return v;
}

void SasInit(Context& c) {  // (core, grain, max_voices, output_mode, sample_rate)
    g_sas = audio::Sas{};
    g_sas.init(static_cast<int>(arg(c, 1)), static_cast<int>(arg(c, 2)));
    g_sas.output_mode = static_cast<int>(arg(c, 3));
    ret(c, 0);
}
void SasCore(Context& c) {  // (core, out)
    g_sas.mix(mem(), reinterpret_cast<std::int16_t*>(mem().ptr(arg(c, 1))), nullptr, 0, 0);
    ret(c, 0);
}
void SasCoreWithMix(Context& c) {  // (core, inout, left, right)
    auto* buffer = reinterpret_cast<std::int16_t*>(mem().ptr(arg(c, 1)));
    static thread_local std::vector<std::int16_t> input;
    input.assign(buffer, buffer + 2 * g_sas.grain());
    g_sas.mix(mem(), buffer, input.data(), static_cast<int>(arg(c, 2)), static_cast<int>(arg(c, 3)));
    ret(c, 0);
}
void SasGetEndFlag(Context& c) { ret(c, g_sas.end_flags()); }
void SasGetGrain(Context& c) { ret(c, static_cast<std::uint32_t>(g_sas.grain())); }
void SasSetGrain(Context& c) {
    g_sas.set_grain(static_cast<int>(arg(c, 1)));
    ret(c, 0);
}
void SasGetOutputmode(Context& c) { ret(c, static_cast<std::uint32_t>(g_sas.output_mode)); }
void SasSetOutputmode(Context& c) {
    g_sas.output_mode = static_cast<int>(arg(c, 1));
    ret(c, 0);
}
void SasSetVoice(Context& c) {  // (core, voice, vag, size, loop)
    if (!sas_voice(c)) return;
    g_sas.set_vag(static_cast<int>(arg(c, 1)), arg(c, 2), arg(c, 3), arg(c, 4) != 0);
    ret(c, 0);
}
void SasSetNoise(Context& c) {  // (core, voice, frequency)
    if (!sas_voice(c)) return;
    g_sas.set_noise(static_cast<int>(arg(c, 1)), static_cast<int>(arg(c, 2)));
    ret(c, 0);
}
void SasSetPitch(Context& c) {
    if (audio::Voice* v = sas_voice(c)) {
        v->pitch = static_cast<int>(arg(c, 2));
        ret(c, 0);
    }
}
void SasSetVolume(Context& c) {  // (core, voice, left, right, effect_left, effect_right)
    if (audio::Voice* v = sas_voice(c)) {
        v->volume_left = static_cast<int>(arg(c, 2));
        v->volume_right = static_cast<int>(arg(c, 3));
        v->effect_left = static_cast<int>(arg(c, 4));
        v->effect_right = static_cast<int>(arg(c, 5));
        ret(c, 0);
    }
}
void SasSetKeyOn(Context& c) {
    if (!sas_voice(c)) return;
    g_sas.key_on(static_cast<int>(arg(c, 1)));
    ret(c, 0);
}
void SasSetKeyOff(Context& c) {
    if (!sas_voice(c)) return;
    g_sas.key_off(static_cast<int>(arg(c, 1)));
    ret(c, 0);
}
void SasSetSimpleADSR(Context& c) {  // (core, voice, adsr1, adsr2)
    if (audio::Voice* v = sas_voice(c)) {
        v->envelope.set_simple(arg(c, 2), arg(c, 3));
        ret(c, 0);
    }
}
void SasSetADSR(Context& c) {  // (core, voice, flags, attack, decay, sustain, release)
    if (audio::Voice* v = sas_voice(c)) {
        const std::uint32_t flags = arg(c, 2);
        auto& e = v->envelope;
        if (flags & 1) e.attack_rate = static_cast<std::int32_t>(arg(c, 3));
        if (flags & 2) e.decay_rate = static_cast<std::int32_t>(arg(c, 4));
        if (flags & 4) e.sustain_rate = static_cast<std::int32_t>(arg(c, 5));
        if (flags & 8) e.release_rate = static_cast<std::int32_t>(arg(c, 6));
        ret(c, 0);
    }
}
void SasSetADSRmode(Context& c) {  // (core, voice, flags, attack, decay, sustain, release)
    if (audio::Voice* v = sas_voice(c)) {
        const std::uint32_t flags = arg(c, 2);
        auto& e = v->envelope;
        const auto curve = [](std::uint32_t m) { return static_cast<audio::Curve>(std::min<std::uint32_t>(m, 5)); };
        if (flags & 1) e.attack_curve = curve(arg(c, 3));
        if (flags & 2) e.decay_curve = curve(arg(c, 4));
        if (flags & 4) e.sustain_curve = curve(arg(c, 5));
        if (flags & 8) e.release_curve = curve(arg(c, 6));
        ret(c, 0);
    }
}
void SasSetSL(Context& c) {
    if (audio::Voice* v = sas_voice(c)) {
        v->envelope.sustain_level = arg(c, 2);
        ret(c, 0);
    }
}
void SasGetEnvelopeHeight(Context& c) {
    if (audio::Voice* v = sas_voice(c)) ret(c, static_cast<std::uint32_t>(v->envelope.height));
}
void SasGetAllEnvelopeHeights(Context& c) {  // (core, heights*)
    for (int i = 0; i < 32; ++i)
        mem().write32(arg(c, 1) + static_cast<std::uint32_t>(i) * 4, static_cast<std::uint32_t>(g_sas.voice(i)->envelope.height));
    ret(c, 0);
}
void SasSetPause(Context& c) {  // (core, voice mask, pause)
    g_sas.set_pause(arg(c, 1), arg(c, 2) != 0);
    ret(c, 0);
}
void SasGetPauseFlag(Context& c) { ret(c, g_sas.pause_flags()); }
void SasRevType(Context& c) {
    g_sas.reverb_type = static_cast<int>(arg(c, 1));
    ret(c, 0);
}
void SasRevParam(Context& c) {
    g_sas.reverb_delay = static_cast<int>(arg(c, 1));
    g_sas.reverb_feedback = static_cast<int>(arg(c, 2));
    ret(c, 0);
}
void SasRevEVOL(Context& c) {
    g_sas.reverb_left = static_cast<int>(arg(c, 1));
    g_sas.reverb_right = static_cast<int>(arg(c, 2));
    ret(c, 0);
}
void SasRevVON(Context& c) {
    g_sas.reverb_dry = arg(c, 1) != 0;
    g_sas.reverb_wet = arg(c, 2) != 0;
    ret(c, 0);
}

constexpr HleBinding kBindings[] = {
    {"sceAudioChReserve", ChReserve},
    {"sceAudioChRelease", ChRelease},
    {"sceAudioOutputBlocking", OutputBlocking},
    {"sceAudioOutputPannedBlocking", OutputPannedBlocking},
    {"sceAudioOutputPanned", OutputPanned},
    {"sceAudioSetChannelDataLen", SetChannelDataLen},
    {"sceAudioChangeChannelConfig", ChangeChannelConfig},
    {"sceAudioChangeChannelVolume", ChangeChannelVolume},
    {"sceAudioGetChannelRestLength", ReturnZero},
    {"sceAudioOutput2Reserve", Output2Reserve},
    {"sceAudioOutput2Release", ReturnZero},
    {"sceAudioOutput2ChangeLength", Output2ChangeLength},
    {"sceAudioOutput2OutputBlocking", Output2OutputBlocking},
    {"sceAudioOutput2GetRestSample", ReturnZero},
    {"__sceSasInit", SasInit},
    {"__sceSasCore", SasCore},
    {"__sceSasCoreWithMix", SasCoreWithMix},
    {"__sceSasGetEndFlag", SasGetEndFlag},
    {"__sceSasGetGrain", SasGetGrain},
    {"__sceSasSetGrain", SasSetGrain},
    {"__sceSasGetOutputmode", SasGetOutputmode},
    {"__sceSasSetOutputmode", SasSetOutputmode},
    {"__sceSasSetVoice", SasSetVoice},
    {"__sceSasSetNoise", SasSetNoise},
    {"__sceSasSetPitch", SasSetPitch},
    {"__sceSasSetVolume", SasSetVolume},
    {"__sceSasSetKeyOn", SasSetKeyOn},
    {"__sceSasSetKeyOff", SasSetKeyOff},
    {"__sceSasSetSimpleADSR", SasSetSimpleADSR},
    {"__sceSasSetADSR", SasSetADSR},
    {"__sceSasSetADSRmode", SasSetADSRmode},
    {"__sceSasSetSL", SasSetSL},
    {"__sceSasGetEnvelopeHeight", SasGetEnvelopeHeight},
    {"__sceSasGetAllEnvelopeHeights", SasGetAllEnvelopeHeights},
    {"__sceSasSetPause", SasSetPause},
    {"__sceSasGetPauseFlag", SasGetPauseFlag},
    {"__sceSasRevType", SasRevType},
    {"__sceSasRevParam", SasRevParam},
    {"__sceSasRevEVOL", SasRevEVOL},
    {"__sceSasRevVON", SasRevVON},
    {"sceUtilityLoadModule", ReturnZero},
    {"sceUtilityUnloadModule", ReturnZero},
    {"sceUtilityMsgDialogGetStatus", ReturnZero},
    {"sceUtilityMsgDialogUpdate", ReturnZero},
};

}  // namespace

void hle_install_audio() { hle_bind(kBindings); }

void set_audio_sink(AudioSink* sink) { g_sink = sink; }

}  // namespace ff7cc::rt
