// sceMpeg: PSMF movie playback (opening movie, cutscenes).
//
// The game streams a PSMF file (MPEG-PS, 2048-byte packs) into a ring buffer through a guest callback and pulls
// access units (AVC video, ATRAC audio) out of it. This module implements the ring buffer and a demuxer with
// real timestamps, so playback lasts as long as the movie and ends the way the game expects. Video frames are
// black and audio is silent until the media phase adds decoding of cooked movies (PORT_PLAN.md Phase 6).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string_view>
#include <vector>

#include "ff7cc/runtime/hle.hpp"
#include "ff7cc/runtime/kernel.hpp"

namespace ff7cc::rt {

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

struct AccessUnit {
    std::uint64_t pts = 0, dts = 0;
    std::uint64_t packet = 0;  // ring packet (cumulative index) in which the unit starts
    std::vector<std::uint8_t> data;
};

enum StreamType { kAvc = 0, kAtrac = 1, kPcm = 2 };

// Reassembles one elementary stream from PES payloads and cuts it into access units at a start marker
// (H.264 access unit delimiter, ATRAC3+ frame header). A PES timestamp belongs to the first unit that starts in
// that PES; later units are extrapolated by the unit duration.
struct EsSplitter {
    std::vector<std::uint8_t> buffer;  // bytes of the unit being assembled (starts at a marker once synced)
    bool synced = false;
    std::size_t scan = 0;
    bool pending = false;  // a PES timestamp waiting for the next unit start
    std::uint64_t pending_pts = 0, pending_dts = 0;
    std::uint64_t base_pts = 0, base_dts = 0, since_base = 0;  // extrapolation anchor
    std::uint64_t current_packet = 0;
    AccessUnit current;
    std::deque<AccessUnit> units;

    void reset() { *this = EsSplitter{}; }
};

struct Mpeg {
    std::uint32_t handle = 0, ringbuffer = 0;
    std::uint64_t written = 0, freed = 0;  // cumulative packet counts
    bool eof = false;
    EsSplitter es[2];  // [kAvc], [kAtrac]
    std::uint8_t atrac_sync[4] = {};
    bool registered[2] = {};
    int next_stream_id = 1;
    int stream_type[16] = {};
    std::uint32_t pixel_mode = 3;
} g_mpeg;

constexpr std::uint64_t kVideoFrameTicks = 3003;  // 29.97 fps in 90 kHz units

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
        const EsSplitter& es = g_mpeg.es[s];
        if (!g_mpeg.registered[s]) continue;
        if (!es.units.empty()) keep = std::min(keep, es.units.front().packet);
        else if (es.synced) keep = std::min(keep, es.current.packet);
    }
    g_mpeg.freed = std::max(g_mpeg.freed, keep);
    sync_ring();
}

std::uint64_t read_pts(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>((p[0] >> 1) & 7) << 30) | (static_cast<std::uint64_t>(p[1]) << 22) |
           (static_cast<std::uint64_t>(p[2] >> 1) << 15) | (static_cast<std::uint64_t>(p[3]) << 7) | (p[4] >> 1);
}

std::uint64_t unit_ticks(int stream, std::uint64_t units) {
    if (stream == kAvc) return units * kVideoFrameTicks;
    return (units * 2048 * 90000 + 22050) / 44100;  // ATRAC3+: 2048 samples per frame at 44.1 kHz
}

// Position of the next unit marker at or after `from`, or npos.
std::size_t find_marker(int stream, const std::vector<std::uint8_t>& b, std::size_t from) {
    const std::uint8_t* sync = g_mpeg.atrac_sync;
    for (std::size_t i = from; i + 4 <= b.size(); ++i) {
        if (stream == kAvc) {
            if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && b[i + 3] == 9) return i;  // access unit delimiter
        } else if (b[i] == 0x0F && b[i + 1] == 0xD0 && (sync[0] == 0 || (b[i + 2] == sync[2] && b[i + 3] == sync[3]))) {
            return i;  // ATRAC3+ frame header (same codec parameters as the first frame)
        }
    }
    return std::string_view::npos;
}

void begin_unit(EsSplitter& es, int stream, std::uint64_t packet) {
    es.current = AccessUnit{};
    es.current.packet = packet;
    if (es.pending) {
        es.base_pts = es.pending_pts;
        es.base_dts = es.pending_dts;
        es.since_base = 0;
        es.pending = false;
    } else {
        ++es.since_base;
    }
    es.current.pts = es.base_pts + unit_ticks(stream, es.since_base);
    es.current.dts = es.base_dts + unit_ticks(stream, es.since_base);
}

void feed(int stream, const std::uint8_t* data, std::size_t size, std::uint64_t packet, bool has_pts,
          std::uint64_t pts, std::uint64_t dts) {
    EsSplitter& es = g_mpeg.es[stream];
    if (has_pts) {
        es.pending = true;
        es.pending_pts = pts;
        es.pending_dts = dts;
    }
    const std::size_t old = es.buffer.size();
    es.buffer.insert(es.buffer.end(), data, data + size);
    std::size_t from = old >= 3 ? old - 3 : 0;  // a marker may straddle two payloads
    if (!es.synced) {
        const std::size_t i = find_marker(stream, es.buffer, from);
        if (i == std::string_view::npos) {
            es.buffer.erase(es.buffer.begin(), es.buffer.end() - static_cast<std::ptrdiff_t>(std::min<std::size_t>(es.buffer.size(), 3)));
            return;
        }
        es.buffer.erase(es.buffer.begin(), es.buffer.begin() + static_cast<std::ptrdiff_t>(i));
        if (stream == kAtrac && g_mpeg.atrac_sync[0] == 0) std::memcpy(g_mpeg.atrac_sync, es.buffer.data(), 4);
        es.synced = true;
        begin_unit(es, stream, packet);
        from = 1;
    }
    for (;;) {
        const std::size_t i = find_marker(stream, es.buffer, std::max<std::size_t>(from, 1));
        if (i == std::string_view::npos) break;
        const std::size_t cut = (stream == kAvc && es.buffer[i - 1] == 0) ? i - 1 : i;  // 4-byte start code
        if (cut == 0) {
            from = i + 1;
            continue;
        }
        es.current.data.assign(es.buffer.begin(), es.buffer.begin() + static_cast<std::ptrdiff_t>(cut));
        es.units.push_back(std::move(es.current));
        es.buffer.erase(es.buffer.begin(), es.buffer.begin() + static_cast<std::ptrdiff_t>(cut));
        begin_unit(es, stream, packet);
        from = i - cut + 1;
    }
}

// Splits one 2048-byte pack into PES packets and feeds their payloads to the stream splitters.
void demux_packet(const std::uint8_t* pack, std::uint64_t index) {
    std::uint32_t pos = 0;
    while (pos + 6 <= kPacketSize) {
        const std::uint8_t* p = pack + pos;
        if (p[0] != 0 || p[1] != 0 || p[2] != 1) break;
        const std::uint8_t id = p[3];
        if (id == 0xBA) {  // MPEG-2 pack header: 14 bytes + stuffing
            if (pos + 14 > kPacketSize) break;
            pos += 14 + (p[13] & 7);
            continue;
        }
        if (id == 0xB9) break;  // program end
        const std::uint32_t length = (static_cast<std::uint32_t>(p[4]) << 8) | p[5];
        const std::uint32_t end = std::min(pos + 6 + length, kPacketSize);
        const int stream = (id & 0xF0) == 0xE0 ? kAvc : id == 0xBD ? kAtrac : -1;
        if (stream >= 0 && pos + 9 <= kPacketSize) {
            const int flags = p[7] >> 6;
            const std::uint32_t header = 9u + p[8];
            std::uint32_t start = pos + header;
            if (stream == kAtrac) start += 4;  // private stream 1 sub-header: stream id, unused, first-frame offset
            const bool has_pts = (flags & 2) && pos + 14 <= kPacketSize;
            const std::uint64_t pts = has_pts ? read_pts(p + 9) : 0;
            const std::uint64_t dts = (flags == 3 && pos + 19 <= kPacketSize) ? read_pts(p + 14) : pts;
            if (end > start) feed(stream, pack + start, end - start, index, has_pts, pts, dts);
        }
        pos = end;
    }
}

void finish_current(EsSplitter& es) {
    es.current.data = std::move(es.buffer);
    es.units.push_back(std::move(es.current));
    es.buffer.clear();
    es.synced = false;
}

// Pops the next complete unit of a stream, or returns false (no data yet / end of stream).
bool next_unit(int stream, AccessUnit& out) {
    EsSplitter& es = g_mpeg.es[stream];
    if (es.units.empty() && g_mpeg.eof && es.synced && !es.buffer.empty()) finish_current(es);
    if (es.units.empty()) return false;
    out = std::move(es.units.front());
    es.units.pop_front();
    return true;
}

void write_au(std::uint32_t au, const AccessUnit& u) {
    mem().write32(au + 0, static_cast<std::uint32_t>(u.pts >> 32));
    mem().write32(au + 4, static_cast<std::uint32_t>(u.pts));
    mem().write32(au + 8, static_cast<std::uint32_t>(u.dts >> 32));
    mem().write32(au + 12, static_cast<std::uint32_t>(u.dts));
    mem().write32(au + 20, static_cast<std::uint32_t>(u.data.size()));
}

std::uint32_t read_be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
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
        for (std::uint32_t i = 0; i < n; ++i) demux_packet(mem().ptr(data + (slot + i) * kPacketSize), g_mpeg.written + i);
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
    const std::uint8_t* h = mem().ptr(arg(c, 1));
    if (std::memcmp(h, "PSMF", 4) != 0) {
        ret(c, 0x80610002);  // invalid value
        return;
    }
    mem().write32(arg(c, 2), read_be32(h + 8));
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
    for (auto& es : g_mpeg.es) es.reset();
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

// sceMpegAvcDecode(mpeg, au, frame_width, buffer*, init*): no decoder yet; outputs a black 480x272 frame.
void AvcDecode(Context& c) {
    const std::uint32_t width = arg(c, 2), buffer = arg(c, 3) ? mem().read32(arg(c, 3)) : 0;
    if (buffer && width) {
        const std::uint32_t bpp = g_mpeg.pixel_mode == 3 ? 4 : 2;
        for (std::uint32_t y = 0; y < 272; ++y) {
            std::uint8_t* row = mem().ptr(buffer + y * width * bpp);
            if (bpp == 4) {
                for (std::uint32_t x = 0; x < 480; ++x) std::memcpy(row + x * 4, "\0\0\0\xFF", 4);
            } else {
                std::memset(row, 0, 480 * 2);
            }
        }
    }
    if (arg(c, 4)) mem().write32(arg(c, 4), 1);
    ret(c, 0);
}
void AvcDecodeFlush(Context& c) { ret(c, 0); }

// sceMpegAtracDecode(mpeg, au, buffer, init): silence.
void AtracDecode(Context& c) {
    if (arg(c, 2)) mem().fill(arg(c, 2), 0, kAtracOutputBytes);
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

}  // namespace ff7cc::rt
