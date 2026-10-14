// sceMpeg: PSMF movie playback (opening movie, cutscenes).
//
// The game streams a PSMF file (MPEG-PS, 2048-byte packs) into a ring buffer through a guest callback and pulls
// access units (AVC video, ATRAC audio) out of it. This module implements the ring buffer and a demuxer with
// real timestamps, so playback lasts as long as the movie and ends the way the game expects. Pictures and
// sound come from the cooked movie (AV1 + Vorbis, see cccook); without it frames are black and audio silent.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "audio/vorbis.hpp"
#include "psprecore/formats/cooked.hpp"
#include "psprecore/formats/psmf.hpp"
#include "psprecore/runtime/hle.hpp"
#include "psprecore/runtime/kernel.hpp"
#include "media/av1_video.hpp"
#include "core/foundation/core/foundation.h"

namespace psprecore::rt {

namespace {

constexpr std::uint32_t kErrorNoData = 0x80618001;
constexpr std::uint32_t kPacketSize = 2048;
constexpr std::uint32_t kAtracOutputBytes = 8192;  // 2048 stereo s16 samples per ATRAC3+ frame
constexpr std::uint32_t kAtracEsSize = 2112;

GuestMemory& mem() { return kernel().memory(); }

// Guest SceMpegRingbuffer layout.
enum RingField : std::uint32_t {
    kPackets = 0, kPacketsRead = 4, kPacketsWritten = 8, kPacketsFree = 12, kPacketSizeField = 16, kData = 20,
    kCallback = 24, kCallbackArg = 28, kDataUpperBound = 32, kSema = 36, kMpeg = 40,
};

using psmf::AccessUnit;

enum StreamType { kAvc = psmf::kVideo, kAtrac = psmf::kAudio, kPcm = 2 };

struct Mpeg {
    std::uint32_t handle = 0, ringbuffer = 0;
    std::uint64_t written = 0, freed = 0;  // cumulative packet counts
    bool eof = false;
    psmf::Demuxer demux;
    bool registered[2] = {};
    int next_stream_id = 1;
    int stream_type[16] = {};
    std::uint32_t pixel_mode = 3;
    // Cooked media for the current movie (psprecore/formats/cooked.hpp).
    nx::unique_ptr<media::Av1Video> video;
    nx::unique_ptr<audio::VorbisStream> audio;
    nx::vector<std::uint32_t> frame;
    int frame_width = 0;
    std::uint64_t audio_base = 0;
    bool have_audio_base = false;
} g_mpeg;

void store16(std::uint8_t* p, std::uint32_t v) {
    const auto x = static_cast<std::uint16_t>(v);
    std::memcpy(p, &x, 2);
}

void sync_ring() {
    const std::uint32_t r = g_mpeg.ringbuffer;
    if (!r) return;
    const std::uint32_t packets = mem().read32(r + kPackets);
    mem().write32(r + kPacketsRead, static_cast<std::uint32_t>(g_mpeg.freed));
    mem().write32(r + kPacketsWritten, packets ? static_cast<std::uint32_t>(g_mpeg.written % packets) : 0);
    mem().write32(r + kPacketsFree, packets - static_cast<std::uint32_t>(g_mpeg.written - g_mpeg.freed));
}

// Frees packets every registered stream has moved past.
void release_packets() {
    std::uint64_t keep = g_mpeg.written;
    for (int s = 0; s < 2; ++s) {
        if (!g_mpeg.registered[s]) continue;
        const auto stream = static_cast<psmf::Stream>(s);
        const auto& units = g_mpeg.demux.units(stream);
        std::uint64_t pack = 0;
        if (!units.empty()) keep = std::min(keep, units.front().pack);
        else if (g_mpeg.demux.assembling(stream, pack)) keep = std::min(keep, pack);
    }
    g_mpeg.freed = std::max(g_mpeg.freed, keep);
    sync_ring();
}

// Pops the next complete unit of a stream, or returns false (no data yet / end of stream).
bool next_unit(int stream, AccessUnit& out) {
    auto& units = g_mpeg.demux.units(static_cast<psmf::Stream>(stream));
    if (units.empty() && g_mpeg.eof) g_mpeg.demux.finish();
    if (units.empty()) return false;
    out = std::move(units.front());
    units.pop_front();
    return true;
}

void write_au(std::uint32_t au, const AccessUnit& u) {
    mem().write32(au + 0, static_cast<std::uint32_t>(u.pts >> 32));
    mem().write32(au + 4, static_cast<std::uint32_t>(u.pts));
    mem().write32(au + 8, static_cast<std::uint32_t>(u.dts >> 32));
    mem().write32(au + 12, static_cast<std::uint32_t>(u.dts));
    mem().write32(au + 20, static_cast<std::uint32_t>(u.data.size()));
}

// ---- library and ring buffer ---------------------------------------------------------------------------

void Init(Context& c) { ret(c, 0); }
void Finish(Context& c) { ret(c, 0); }
void QueryMemSize(Context& c) { ret(c, 0x10000); }
void RingbufferQueryMemSize(Context& c) { ret(c, arg(c, 0) * (104 + kPacketSize)); }

// sceMpegRingbufferConstruct(ringbuffer, packets, data, size, callback, callback_arg)
void RingbufferConstruct(Context& c) {
    const std::uint32_t r = arg(c, 0), packets = arg(c, 1), data = arg(c, 2);
    mem().fill(r, 0, 48);
    mem().write32(r + kPackets, packets);
    mem().write32(r + kPacketsFree, packets);
    mem().write32(r + kPacketSizeField, kPacketSize);
    mem().write32(r + kData, data);
    mem().write32(r + kCallback, arg(c, 4));
    mem().write32(r + kCallbackArg, arg(c, 5));
    mem().write32(r + kDataUpperBound, data + packets * kPacketSize);
    mem().write32(r + kSema, 0xFFFFFFFF);
    ret(c, 0);
}
void RingbufferDestruct(Context& c) { ret(c, 0); }

// sceMpegCreate(mpeg, data, size, ringbuffer, frame_width, mode, ddr_top)
void Create(Context& c) {
    const std::uint32_t mpeg = arg(c, 0), data = arg(c, 1), ring = arg(c, 3);
    g_mpeg = Mpeg{};
    g_mpeg.handle = data + 0x30;
    g_mpeg.ringbuffer = ring;
    mem().write32(mpeg, g_mpeg.handle);
    mem().write_string(g_mpeg.handle, "LIBMPEG");
    mem().write_string(g_mpeg.handle + 8, "001");
    mem().write32(g_mpeg.handle + 12, 0xFFFFFFFF);
    if (ring) mem().write32(ring + kMpeg, mpeg);
    ret(c, 0);
}
void Delete(Context& c) { ret(c, 0); }

// sceMpegRingbufferPut(ringbuffer, packets, available): fills packets through the guest callback.
void RingbufferPut(Context& c) {
    const std::uint32_t r = arg(c, 0);
    std::uint32_t wanted = std::min(arg(c, 1), arg(c, 2));
    const std::uint32_t packets = mem().read32(r + kPackets), data = mem().read32(r + kData);
    const std::uint32_t callback = mem().read32(r + kCallback), callback_arg = mem().read32(r + kCallbackArg);
    g_mpeg.ringbuffer = r;
    std::uint32_t added = 0;
    while (wanted > 0 && packets && callback) {
        const std::uint32_t slot = static_cast<std::uint32_t>(g_mpeg.written % packets);
        const std::uint32_t chunk = std::min(wanted, packets - slot);
        const auto got = static_cast<std::int32_t>(kernel().call_guest(callback, {data + slot * kPacketSize, chunk, callback_arg}));
        if (got <= 0) {
            if (got == 0) g_mpeg.eof = true;
            break;
        }
        const std::uint32_t n = std::min(static_cast<std::uint32_t>(got), chunk);
        for (std::uint32_t i = 0; i < n; ++i)
            g_mpeg.demux.feed({reinterpret_cast<const std::byte*>(mem().ptr(data + (slot + i) * kPacketSize)), kPacketSize},
                              g_mpeg.written + i);
        g_mpeg.written += n;
        added += n;
        wanted -= n;
        if (n < chunk) break;
    }
    sync_ring();
    ret(c, added);
}

void RingbufferAvailableSize(Context& c) {
    const std::uint32_t r = arg(c, 0);
    ret(c, mem().read32(r + kPackets) - static_cast<std::uint32_t>(g_mpeg.written - g_mpeg.freed));
}

// ---- streams and access units --------------------------------------------------------------------------

// sceMpegQueryStreamOffset(mpeg, psmf_header, offset*): data starts at the big-endian offset at +8.
void QueryStreamOffset(Context& c) {
    psmf::Header header;
    if (!psmf::parse_header({reinterpret_cast<const std::byte*>(mem().ptr(arg(c, 1))), psmf::kPackSize}, header)) {
        ret(c, 0x80610002);  // invalid value
        return;
    }
    mem().write32(arg(c, 2), header.data_offset);

    // Identify the movie by its header and open the cooked streams.
    const std::uint64_t key = cooked::fnv1a64({reinterpret_cast<const std::byte*>(mem().ptr(arg(c, 1))), psmf::kPackSize});
    g_mpeg.video = media::Av1Video::open(cooked::movie_video_path(cooked_root(), key));
    g_mpeg.audio = audio::VorbisStream::open(cooked::movie_audio_path(cooked_root(), key));
    g_mpeg.frame.clear();
    g_mpeg.have_audio_base = false;
    if (!g_mpeg.video)
        std::fprintf(stderr, "sceMpeg: no cooked movie %s (run cccook); showing black\n",
                     cooked::movie_video_path(cooked_root(), key).string().c_str());
    ret(c, 0);
}

void RegistStream(Context& c) {
    const int type = static_cast<int>(arg(c, 1));
    const int id = g_mpeg.next_stream_id++;
    g_mpeg.stream_type[id & 15] = type;
    if (type == kAvc || type == kAtrac) g_mpeg.registered[type] = true;
    ret(c, static_cast<std::uint32_t>(id));
}
void UnRegistStream(Context& c) {
    const int type = g_mpeg.stream_type[arg(c, 1) & 15];
    if (type == kAvc || type == kAtrac) g_mpeg.registered[type] = false;
    release_packets();
    ret(c, 0);
}
void FlushAllStream(Context& c) {
    g_mpeg.demux.reset();
    g_mpeg.freed = g_mpeg.written;
    g_mpeg.eof = false;
    sync_ring();
    ret(c, 0);
}

void MallocAvcEsBuf(Context& c) { ret(c, 1); }
void FreeAvcEsBuf(Context& c) { ret(c, 0); }

// sceMpegQueryAtracEsSize(mpeg, es_size*, out_size*)
void QueryAtracEsSize(Context& c) {
    if (arg(c, 1)) mem().write32(arg(c, 1), kAtracEsSize);
    if (arg(c, 2)) mem().write32(arg(c, 2), kAtracOutputBytes);
    ret(c, 0);
}

// sceMpegInitAu(mpeg, es_buffer, au)
void InitAu(Context& c) {
    const std::uint32_t au = arg(c, 2);
    for (std::uint32_t i = 0; i < 16; i += 4) mem().write32(au + i, 0xFFFFFFFF);
    mem().write32(au + 16, arg(c, 1));
    mem().write32(au + 20, 0);
    ret(c, 0);
}

// sceMpegGetAvcAu / sceMpegGetAtracAu(mpeg, stream, au, attr*)
void get_au(Context& c, int stream) {
    AccessUnit u;
    if (!next_unit(stream, u)) {
        ret(c, kErrorNoData);
        return;
    }
    write_au(arg(c, 2), u);
    if (arg(c, 3)) mem().write32(arg(c, 3), stream == kAvc ? 1 : 0);
    release_packets();
    ret(c, 0);
}
void GetAvcAu(Context& c) { get_au(c, kAvc); }
void GetAtracAu(Context& c) { get_au(c, kAtrac); }
void GetPcmAu(Context& c) { ret(c, kErrorNoData); }

// sceMpegAvcDecodeMode(mpeg, mode*): mode = {unknown, pixel format}
void AvcDecodeMode(Context& c) {
    if (arg(c, 1)) g_mpeg.pixel_mode = mem().read32(arg(c, 1) + 4);
    ret(c, 0);
}

// sceMpegAvcDecode(mpeg, au, frame_width, buffer*, init*): the next frame of the cooked movie, in display order
// (black without cooked media). Writes 480x272 pixels in the pixel mode chosen by sceMpegAvcDecodeMode.
void AvcDecode(Context& c) {
    mark_guest_written_all();  // the decoded frame lands in the game's buffer
    const std::uint32_t width = arg(c, 2), buffer = arg(c, 3) ? mem().read32(arg(c, 3)) : 0;
    if (g_mpeg.video && g_mpeg.video->next(g_mpeg.frame)) g_mpeg.frame_width = g_mpeg.video->width();
    if (buffer && width) {
        const std::uint32_t bpp = g_mpeg.pixel_mode == 3 ? 4 : 2;
        const int fw = g_mpeg.frame_width;
        const bool have = !g_mpeg.frame.empty() && fw > 0;
        for (std::uint32_t y = 0; y < 272; ++y) {
            std::uint8_t* row = mem().ptr(buffer + y * width * bpp);
            for (std::uint32_t x = 0; x < 480; ++x) {
                const bool inside = have && static_cast<int>(x) < fw &&
                                    (y + 1) * static_cast<std::uint32_t>(fw) <= g_mpeg.frame.size();
                const std::uint32_t p = inside ? g_mpeg.frame[y * static_cast<std::uint32_t>(fw) + x] : 0xFF000000u;
                const std::uint32_t r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
                switch (g_mpeg.pixel_mode) {
                    case 0: store16(row + x * 2, (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11)); break;
                    case 1: store16(row + x * 2, (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | 0x8000); break;
                    case 2: store16(row + x * 2, (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | 0xF000); break;
                    default: std::memcpy(row + x * 4, &p, 4); break;
                }
            }
        }
    }
    if (arg(c, 4)) mem().write32(arg(c, 4), 1);
    ret(c, 0);
}
void AvcDecodeFlush(Context& c) { ret(c, 0); }

// sceMpegAtracDecode(mpeg, au, buffer, init): 2048 stereo samples of the cooked movie audio at the unit's
// timestamp (silence without cooked media).
void AtracDecode(Context& c) {
    const std::uint32_t au = arg(c, 1), buffer = arg(c, 2);
    if (!buffer) return ret(c, 0);
    mem().fill(buffer, 0, kAtracOutputBytes);
    if (g_mpeg.audio && au) {
        const std::uint64_t pts = (std::uint64_t{mem().read32(au)} << 32) | mem().read32(au + 4);
        if (!g_mpeg.have_audio_base) {
            g_mpeg.audio_base = pts;
            g_mpeg.have_audio_base = true;
        }
        const std::uint64_t ticks = pts >= g_mpeg.audio_base ? pts - g_mpeg.audio_base : 0;
        const std::uint64_t frame = (ticks * 44100 / 90000 + 1024) / 2048;  // nearest ATRAC frame
        auto* out = reinterpret_cast<std::int16_t*>(mem().ptr(buffer));
        if (g_mpeg.audio->channels() == 2) {
            g_mpeg.audio->read(frame * 2048, out, 2048);
        } else {
            std::int16_t mono[2048] = {};
            const std::uint32_t got = g_mpeg.audio->read(frame * 2048, mono, 2048);
            for (std::uint32_t i = 0; i < got; ++i) out[2 * i] = out[2 * i + 1] = mono[i];
        }
    }
    ret(c, 0);
}

constexpr HleBinding kBindings[] = {
    {"sceMpegInit", Init},
    {"sceMpegFinish", Finish},
    {"sceMpegQueryMemSize", QueryMemSize},
    {"sceMpegRingbufferQueryMemSize", RingbufferQueryMemSize},
    {"sceMpegRingbufferConstruct", RingbufferConstruct},
    {"sceMpegRingbufferDestruct", RingbufferDestruct},
    {"sceMpegRingbufferPut", RingbufferPut},
    {"sceMpegRingbufferAvailableSize", RingbufferAvailableSize},
    {"sceMpegCreate", Create},
    {"sceMpegDelete", Delete},
    {"sceMpegQueryStreamOffset", QueryStreamOffset},
    {"sceMpegRegistStream", RegistStream},
    {"sceMpegUnRegistStream", UnRegistStream},
    {"sceMpegFlushAllStream", FlushAllStream},
    {"sceMpegMallocAvcEsBuf", MallocAvcEsBuf},
    {"sceMpegFreeAvcEsBuf", FreeAvcEsBuf},
    {"sceMpegQueryAtracEsSize", QueryAtracEsSize},
    {"sceMpegInitAu", InitAu},
    {"sceMpegGetAvcAu", GetAvcAu},
    {"sceMpegGetAtracAu", GetAtracAu},
    {"sceMpegGetPcmAu", GetPcmAu},
    {"sceMpegAvcDecodeMode", AvcDecodeMode},
    {"sceMpegAvcDecode", AvcDecode},
    {"sceMpegAvcDecodeFlush", AvcDecodeFlush},
    {"sceMpegAtracDecode", AtracDecode},
};

}  // namespace

void hle_install_mpeg() { hle_bind(kBindings); }

}  // namespace psprecore::rt
