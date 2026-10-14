#include "ff7cc/formats/psmf.hpp"

#include <algorithm>
#include <cstring>

namespace ff7cc::psmf {

namespace {

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
std::uint64_t be48(const std::uint8_t* p) { return (std::uint64_t{be32(p)} << 16) | (std::uint64_t{p[4]} << 8) | p[5]; }

std::uint64_t read_pts(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>((p[0] >> 1) & 7) << 30) | (static_cast<std::uint64_t>(p[1]) << 22) |
           (static_cast<std::uint64_t>(p[2] >> 1) << 15) | (static_cast<std::uint64_t>(p[3]) << 7) | (p[4] >> 1);
}

std::uint64_t unit_ticks(int stream, std::uint64_t units) {
    if (stream == kVideo) return units * kVideoFrameTicks;
    return (units * 2048 * 90000 + 22050) / 44100;
}

}  // namespace

bool parse_header(std::span<const std::byte> data, Header& out) {
    if (data.size() < 0x60 || std::memcmp(data.data(), "PSMF", 4) != 0) return false;
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    out.data_offset = be32(p + 8);
    out.data_size = be32(p + 12);
    out.first_timestamp = be48(p + 0x54);
    out.last_timestamp = be48(p + 0x5A);
    return true;
}

bool Demuxer::assembling(Stream s, std::uint64_t& pack) const {
    if (!es_[s].synced) return false;
    pack = es_[s].current.pack;
    return true;
}

std::size_t Demuxer::find_marker(int stream, std::size_t from) const {
    const auto& b = es_[stream].buffer;
    for (std::size_t i = from; i + 4 <= b.size(); ++i) {
        if (stream == kVideo) {
            if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && b[i + 3] == 9) return i;  // access unit delimiter
        } else if (b[i] == 0x0F && b[i + 1] == 0xD0 &&
                   (atrac_params_ == 0 || ((b[i + 2] << 8) | b[i + 3]) == atrac_params_)) {
            return i;  // ATRAC3plus frame header with the stream's codec parameters
        }
    }
    return kNone;
}

void Demuxer::begin_unit(int stream, std::uint64_t pack) {
    Splitter& es = es_[stream];
    es.current = AccessUnit{};
    es.current.pack = pack;
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

void Demuxer::feed_es(int stream, const std::uint8_t* data, std::size_t size, std::uint64_t pack, bool has_pts,
                      std::uint64_t pts, std::uint64_t dts) {
    Splitter& es = es_[stream];
    if (has_pts) {
        es.pending = true;
        es.pending_pts = pts;
        es.pending_dts = dts;
    }
    const std::size_t old = es.buffer.size();
    es.buffer.insert(es.buffer.end(), data, data + size);
    std::size_t from = old >= 3 ? old - 3 : 0;  // a marker may straddle two payloads
    if (!es.synced) {
        const std::size_t i = find_marker(stream, from);
        if (i == kNone) {
            const std::size_t keep = std::min<std::size_t>(es.buffer.size(), 3);
            es.buffer.erase(es.buffer.begin(), es.buffer.end() - static_cast<std::ptrdiff_t>(keep));
            return;
        }
        es.buffer.erase(es.buffer.begin(), es.buffer.begin() + static_cast<std::ptrdiff_t>(i));
        if (stream == kAudio && atrac_params_ == 0)
            atrac_params_ = static_cast<std::uint16_t>((es.buffer[2] << 8) | es.buffer[3]);
        es.synced = true;
        begin_unit(stream, pack);
        from = 1;
    }
    for (;;) {
        const std::size_t i = find_marker(stream, std::max<std::size_t>(from, 1));
        if (i == kNone) break;
        const std::size_t cut = (stream == kVideo && es.buffer[i - 1] == 0) ? i - 1 : i;  // 4-byte start code
        if (cut == 0) {
            from = i + 1;
            continue;
        }
        es.current.data.assign(es.buffer.begin(), es.buffer.begin() + static_cast<std::ptrdiff_t>(cut));
        es.units.push_back(std::move(es.current));
        es.buffer.erase(es.buffer.begin(), es.buffer.begin() + static_cast<std::ptrdiff_t>(cut));
        begin_unit(stream, pack);
        from = i - cut + 1;
    }
}

void Demuxer::feed(std::span<const std::byte> bytes, std::uint64_t index) {
    const auto* pack = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const std::uint32_t pack_size = static_cast<std::uint32_t>(std::min<std::size_t>(bytes.size(), kPackSize));
    std::uint32_t pos = 0;
    while (pos + 6 <= pack_size) {
        const std::uint8_t* p = pack + pos;
        if (p[0] != 0 || p[1] != 0 || p[2] != 1) break;
        const std::uint8_t id = p[3];
        if (id == 0xBA) {  // MPEG-2 pack header: 14 bytes + stuffing
            if (pos + 14 > pack_size) break;
            pos += 14 + (p[13] & 7);
            continue;
        }
        if (id == 0xB9) break;  // program end
        const std::uint32_t length = (std::uint32_t{p[4]} << 8) | p[5];
        const std::uint32_t end = std::min(pos + 6 + length, pack_size);
        const int stream = (id & 0xF0) == 0xE0 ? kVideo : id == 0xBD ? kAudio : -1;
        if (stream >= 0 && pos + 9 <= pack_size) {
            const int flags = p[7] >> 6;
            std::uint32_t start = pos + 9 + p[8];
            if (stream == kAudio) start += 4;  // sub-header: stream id, unused, first-frame offset
            const bool has_pts = (flags & 2) && pos + 14 <= pack_size;
            const std::uint64_t pts = has_pts ? read_pts(p + 9) : 0;
            const std::uint64_t dts = (flags == 3 && pos + 19 <= pack_size) ? read_pts(p + 14) : pts;
            if (end > start) feed_es(stream, pack + start, end - start, index, has_pts, pts, dts);
        }
        pos = end;
    }
}

void Demuxer::finish() {
    for (Splitter& es : es_) {
        if (!es.synced || es.buffer.empty()) continue;
        es.current.data = std::move(es.buffer);
        es.units.push_back(std::move(es.current));
        es.buffer.clear();
        es.synced = false;
    }
}

}  // namespace ff7cc::psmf
