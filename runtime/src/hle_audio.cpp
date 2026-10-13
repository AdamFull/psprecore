// sceAudio, sceSasCore, sceAtrac3plus, sceMpeg, sceUtility — timing-correct stubs until Phase 6:
// blocking outputs take as long as the samples they would play, decoders report "finished".

#include <cstdio>

#include "ff7cc/runtime/hle.hpp"
#include "ff7cc/runtime/kernel.hpp"

namespace ff7cc::rt {

namespace {

GuestMemory& mem() { return kernel().memory(); }

struct AudioChannel {
    bool reserved = false;
    std::uint32_t samples = 0;
};
AudioChannel g_channels[8];
std::uint32_t g_output2_samples = 0;

void play_for(std::uint32_t samples) { kernel().wait(WaitType::Audio, 0, 0, false, samples * 1000000ull / 44100); }

// ---- sceAudio ----------------------------------------------------------------------------------

void ChReserve(Context& c) {  // (channel or -1, samples, format)
    int ch = static_cast<int>(arg(c, 0));
    if (ch < 0)
        for (ch = 0; ch < 8 && g_channels[ch].reserved; ++ch) {}
    if (ch >= 8) return ret(c, 0x80260002);
    g_channels[ch] = {true, arg(c, 1)};
    ret(c, static_cast<std::uint32_t>(ch));
}
void ChRelease(Context& c) {
    if (arg(c, 0) < 8) g_channels[arg(c, 0)].reserved = false;
    ret(c, 0);
}
void OutputBlocking(Context& c) {  // (channel, volume..., buffer)
    const std::uint32_t ch = arg(c, 0) & 7;
    play_for(g_channels[ch].samples);
    ret(c, g_channels[ch].samples);
}
void OutputNonBlocking(Context& c) { ret(c, g_channels[arg(c, 0) & 7].samples); }
void SetChannelDataLen(Context& c) {
    g_channels[arg(c, 0) & 7].samples = arg(c, 1);
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
void Output2OutputBlocking(Context& c) {
    play_for(g_output2_samples);
    ret(c, g_output2_samples);
}
void ReturnZero(Context& c) { ret(c, 0); }

// ---- sceSasCore --------------------------------------------------------------------------------

std::uint32_t g_sas_grain = 256;
void SasInit(Context& c) {  // (core, grain, maxVoices, outputMode, sampleRate)
    g_sas_grain = arg(c, 1);
    ret(c, 0);
}
void SasCore(Context& c) {  // (core, out): mixes one grain of stereo 16-bit samples
    mem().fill(arg(c, 1), 0, g_sas_grain * 4);
    ret(c, 0);
}
void SasCoreWithMix(Context& c) { ret(c, 0); }
void SasGetEndFlag(Context& c) { ret(c, 0xFFFFFFFF); }  // every voice idle
void SasGetGrain(Context& c) { ret(c, g_sas_grain); }
void SasSetGrain(Context& c) {
    g_sas_grain = arg(c, 1);
    ret(c, 0);
}

// ---- sceAtrac3plus -----------------------------------------------------------------------------

int g_next_atrac = 0;
void AtracSetDataAndGetID(Context& c) { ret(c, static_cast<std::uint32_t>(g_next_atrac++ & 3)); }
// (id, out, samplesOut*, end*, remainFrames*): report the end of the stream.
void AtracDecodeData(Context& c) {
    if (arg(c, 2)) mem().write32(arg(c, 2), 0);
    if (arg(c, 3)) mem().write32(arg(c, 3), 1);
    if (arg(c, 4)) mem().write32(arg(c, 4), 0);
    ret(c, 0);
}

// ---- sceUtility (save data dialog: report "no data" so the game proceeds) -----------------------

enum : std::uint32_t { kStatusNone = 0, kStatusFinished = 3, kStatusShutdown = 4 };
std::uint32_t g_savedata_status = kStatusNone, g_savedata_param = 0;
void SavedataInitStart(Context& c) {
    g_savedata_param = arg(c, 0);
    std::fprintf(stderr, "sceUtilitySavedataInitStart(mode %u) -> reporting no save data\n", mem().read32(g_savedata_param + 48));
    mem().write32(g_savedata_param + 28, 0x80110307);  // SceUtilityParamBase.result = SAVEDATA no data
    g_savedata_status = kStatusFinished;
    ret(c, 0);
}
void SavedataGetStatus(Context& c) { ret(c, g_savedata_status); }
void SavedataShutdownStart(Context& c) {
    g_savedata_status = kStatusNone;
    ret(c, 0);
}

constexpr HleBinding kBindings[] = {
    {"sceAudioChReserve", ChReserve},
    {"sceAudioChRelease", ChRelease},
    {"sceAudioOutputBlocking", OutputBlocking},
    {"sceAudioOutputPannedBlocking", OutputBlocking},
    {"sceAudioOutputPanned", OutputNonBlocking},
    {"sceAudioSetChannelDataLen", SetChannelDataLen},
    {"sceAudioChangeChannelConfig", ReturnZero},
    {"sceAudioChangeChannelVolume", ReturnZero},
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
    {"sceAtracSetDataAndGetID", AtracSetDataAndGetID},
    {"sceAtracDecodeData", AtracDecodeData},
    {"sceUtilityLoadModule", ReturnZero},
    {"sceUtilityUnloadModule", ReturnZero},
    {"sceUtilitySavedataInitStart", SavedataInitStart},
    {"sceUtilitySavedataGetStatus", SavedataGetStatus},
    {"sceUtilitySavedataShutdownStart", SavedataShutdownStart},
    {"sceUtilitySavedataUpdate", ReturnZero},
    {"sceUtilityMsgDialogGetStatus", ReturnZero},
    {"sceUtilityMsgDialogUpdate", ReturnZero},
};

}  // namespace

void hle_install_audio() { hle_bind(kBindings); }

}  // namespace ff7cc::rt
