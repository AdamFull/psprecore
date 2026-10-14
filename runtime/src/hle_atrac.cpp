// sceAtrac3plus: music and voice clips. The game hands over a whole RIFF ATRAC3/ATRAC3plus file; we decode
// the matching cooked Ogg Vorbis stream (ff7cc/formats/cooked.hpp) instead of ATRAC. Without cooked media
// the same calls produce silence of the right length, so game timing does not change.
//
// Timelines: the cooked stream holds the raw decoder output; game sample g is raw sample g + delay (fact).
// Outputs are cut at raw codec-frame boundaries like the PSP library does, so the first call of a stream is
// shorter, and never cross the loop end.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "audio/vorbis.hpp"
#include "ff7cc/formats/atrac.hpp"
#include "ff7cc/formats/cooked.hpp"
#include "ff7cc/runtime/hle.hpp"
#include "ff7cc/runtime/kernel.hpp"
#include "core/foundation/core/foundation.h"

namespace ff7cc::rt {

namespace {

constexpr std::uint32_t kErrorBadId = 0x80630005;
constexpr std::uint32_t kErrorNoFreeId = 0x80630003;
constexpr std::uint32_t kErrorUnknownFormat = 0x80630006;
constexpr std::uint32_t kErrorAllDataDecoded = 0x80630024;
constexpr std::uint32_t kAllDataOnMemory = 0xFFFFFFFF;  // remainFrames when the whole file was given

std::filesystem::path g_cooked_root = "cooked";

GuestMemory& mem() { return kernel().memory(); }

struct Slot {
    bool used = false;
    atrac::Info info;
    std::uint64_t key = 0;
    std::unique_ptr<audio::VorbisStream> stream;
    std::uint32_t position = 0;  // next game-timeline sample
    int loops_left = 0;          // -1 = forever
    bool has_loop = false;
    std::uint32_t loop_start = 0, loop_end = 0;  // game timeline, inclusive end
    std::uint32_t end_sample = 0;                // last playable sample
    nx::vector<std::int16_t> scratch;
};
Slot g_slots[6];

Slot* slot(Context& c) {
    const std::uint32_t id = arg(c, 0);
    if (id >= std::size(g_slots) || !g_slots[id].used) {
        ret(c, kErrorBadId);
        return nullptr;
    }
    return &g_slots[id];
}

// sceAtracSetDataAndGetID(buffer, size)
void SetDataAndGetID(Context& c) {
    const std::uint32_t buffer = arg(c, 0), size = arg(c, 1);
    std::span<const std::byte> file(reinterpret_cast<const std::byte*>(mem().ptr(buffer)), size);
    atrac::Info info;
    if (!atrac::parse(file, info)) return ret(c, kErrorUnknownFormat);
    const auto it = std::find_if(std::begin(g_slots), std::end(g_slots), [](const Slot& s) { return !s.used; });
    if (it == std::end(g_slots)) return ret(c, kErrorNoFreeId);
    Slot& s = *it;
    s = Slot{};
    s.used = true;
    s.info = info;
    s.key = cooked::fnv1a64(file.first(std::min<std::size_t>(file.size(), std::size_t{info.data_offset} + info.data_size)));
    s.stream = audio::VorbisStream::open(cooked::audio_path(g_cooked_root, s.key));
    const std::uint32_t raw_samples = info.frames() * static_cast<std::uint32_t>(info.samples_per_frame());
    const std::uint32_t total = info.total_samples ? info.total_samples : raw_samples - std::min(raw_samples, info.delay);
    s.end_sample = total ? total - 1 : 0;
    if (!info.loops.empty() && info.loops[0].end >= info.loops[0].start) {
        s.has_loop = true;
        s.loop_start = info.loops[0].start - std::min(info.loops[0].start, info.delay);
        s.loop_end = std::min(info.loops[0].end - std::min(info.loops[0].end, info.delay), s.end_sample);
    }
    static int warnings = 0;
    if (!s.stream && warnings++ < 8)
        std::fprintf(stderr, "sceAtrac: no cooked audio %s (buffer %u bytes, RIFF data ends at %u); playing silence\n",
                     cooked::audio_path(g_cooked_root, s.key).string().c_str(), size, info.data_offset + info.data_size);
    ret(c, static_cast<std::uint32_t>(it - std::begin(g_slots)));
}

void ReleaseAtracID(Context& c) {
    if (Slot* s = slot(c)) {
        *s = Slot{};
        ret(c, 0);
    }
}

// sceAtracGetSoundSample(id, end_sample*, loop_start*, loop_end*)
void GetSoundSample(Context& c) {
    if (Slot* s = slot(c)) {
        if (arg(c, 1)) mem().write32(arg(c, 1), s->end_sample);
        if (arg(c, 2)) mem().write32(arg(c, 2), s->has_loop ? s->loop_start : 0xFFFFFFFF);
        if (arg(c, 3)) mem().write32(arg(c, 3), s->has_loop ? s->loop_end : 0xFFFFFFFF);
        ret(c, 0);
    }
}

void SetLoopNum(Context& c) {
    if (Slot* s = slot(c)) {
        s->loops_left = static_cast<int>(arg(c, 1));
        ret(c, 0);
    }
}

// sceAtracResetPlayPosition(id, sample, bytes_first_buffer, bytes_second_buffer)
void ResetPlayPosition(Context& c) {
    if (Slot* s = slot(c)) {
        s->position = std::min(arg(c, 1), s->end_sample + 1);
        ret(c, 0);
    }
}

// sceAtracDecodeData(id, out, samples*, end*, remain_frames*)
void DecodeData(Context& c) {
    Slot* s = slot(c);
    if (!s) return;
    const std::uint32_t out = arg(c, 1);
    const auto write = [&](std::uint32_t samples, bool end) {
        if (arg(c, 2)) mem().write32(arg(c, 2), samples);
        if (arg(c, 3)) mem().write32(arg(c, 3), end ? 1 : 0);
        if (arg(c, 4)) mem().write32(arg(c, 4), kAllDataOnMemory);
    };
    if (s->position > s->end_sample) {
        write(0, true);
        return ret(c, kErrorAllDataDecoded);
    }
    // One codec frame, cut at raw frame boundaries, the loop end and the stream end.
    const std::uint32_t frame = static_cast<std::uint32_t>(s->info.samples_per_frame());
    const std::uint32_t raw = s->position + s->info.delay;
    const bool looping = s->has_loop && s->loops_left != 0 && s->position <= s->loop_end;
    const std::uint32_t limit = looping ? s->loop_end : s->end_sample;
    const std::uint32_t count = std::min(frame - raw % frame, limit - s->position + 1);

    const int channels = s->info.channels;
    auto* pcm = reinterpret_cast<std::int16_t*>(mem().ptr(out));
    std::memset(pcm, 0, std::size_t{count} * static_cast<std::size_t>(channels) * 2);
    if (s->stream) {
        const int sc = s->stream->channels();
        s->scratch.resize(std::size_t{count} * static_cast<std::size_t>(sc));
        const std::uint32_t got = s->stream->read(raw, s->scratch.data(), count);
        for (std::uint32_t i = 0; i < got; ++i)
            for (int ch = 0; ch < channels; ++ch)
                pcm[i * static_cast<std::uint32_t>(channels) + static_cast<std::uint32_t>(ch)] =
                    s->scratch[i * static_cast<std::uint32_t>(sc) + static_cast<std::uint32_t>(std::min(ch, sc - 1))];
    }
    s->position += count;
    if (looping && s->position > s->loop_end) {
        s->position = s->loop_start;
        if (s->loops_left > 0) --s->loops_left;
    }
    write(count, s->position > s->end_sample);
    ret(c, 0);
}

void GetInternalErrorInfo(Context& c) {
    if (Slot* s = slot(c)) {
        (void)s;
        if (arg(c, 1)) mem().write32(arg(c, 1), 0);
        ret(c, 0);
    }
}

constexpr HleBinding kBindings[] = {
    {"sceAtracSetDataAndGetID", SetDataAndGetID},
    {"sceAtracReleaseAtracID", ReleaseAtracID},
    {"sceAtracGetSoundSample", GetSoundSample},
    {"sceAtracSetLoopNum", SetLoopNum},
    {"sceAtracResetPlayPosition", ResetPlayPosition},
    {"sceAtracDecodeData", DecodeData},
    {"sceAtracGetInternalErrorInfo", GetInternalErrorInfo},
};

}  // namespace

void hle_install_atrac() { hle_bind(kBindings); }

void set_cooked_root(const std::filesystem::path& root) { g_cooked_root = root; }
const std::filesystem::path& cooked_root() { return g_cooked_root; }

}  // namespace ff7cc::rt
