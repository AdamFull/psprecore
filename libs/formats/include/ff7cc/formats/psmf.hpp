#pragma once

// PSMF movies: a 2048-byte "PSMF" header followed by an MPEG-2 program stream in 2048-byte packs carrying
// H.264 video (stream 0xE0) and ATRAC3plus audio (private stream 1, 0xBD, with a 4-byte sub-header).
//
// The demuxer reassembles both elementary streams and cuts them into access units: H.264 at access unit
// delimiters, ATRAC3plus at its 8-byte frame headers (0F D0 xx xx 00 00 00 00). A PES timestamp belongs to the
// first unit that starts in that PES; later units are extrapolated (3003 ticks per video frame at 29.97 fps,
// 2048 samples per audio frame at 44.1 kHz).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace ff7cc::psmf {

inline constexpr std::uint32_t kPackSize = 2048;
inline constexpr std::uint64_t kVideoFrameTicks = 3003;

struct Header {
    std::uint32_t data_offset = 0;  // usually 2048
    std::uint32_t data_size = 0;
    std::uint64_t first_timestamp = 0, last_timestamp = 0;  // 90 kHz
};
bool parse_header(std::span<const std::byte> data, Header& out);

enum Stream : int { kVideo = 0, kAudio = 1 };

struct AccessUnit {
    std::uint64_t pts = 0, dts = 0;
    std::uint64_t pack = 0;  // index of the pack in which the unit starts
    std::vector<std::uint8_t> data;
};

class Demuxer {
public:
    // Feeds one 2048-byte pack; `index` is its position in the stream (used for AccessUnit::pack).
    void feed(std::span<const std::byte> pack, std::uint64_t index);
    // End of stream: the units still being assembled become complete.
    void finish();
    void reset() { *this = Demuxer{}; }

    std::deque<AccessUnit>& units(Stream s) { return es_[s].units; }
    // Pack where the unit currently being assembled started, if any.
    bool assembling(Stream s, std::uint64_t& pack) const;
    // ATRAC3plus codec parameters from the first audio frame header (0 until seen).
    std::uint16_t atrac_params() const { return atrac_params_; }

private:
    struct Splitter {
        std::vector<std::uint8_t> buffer;
        bool synced = false;
        bool pending = false;
        std::uint64_t pending_pts = 0, pending_dts = 0;
        std::uint64_t base_pts = 0, base_dts = 0, since_base = 0;
        AccessUnit current;
        std::deque<AccessUnit> units;
    };
    void feed_es(int stream, const std::uint8_t* data, std::size_t size, std::uint64_t pack, bool has_pts,
                 std::uint64_t pts, std::uint64_t dts);
    std::size_t find_marker(int stream, std::size_t from) const;
    void begin_unit(int stream, std::uint64_t pack);

    Splitter es_[2];
    std::uint16_t atrac_params_ = 0;
};

}  // namespace ff7cc::psmf
