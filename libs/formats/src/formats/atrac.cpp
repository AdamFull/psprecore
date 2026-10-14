#include "ff7cc/formats/atrac.hpp"

#include <cstring>

#include "core/foundation/core/foundation.h"

namespace ff7cc::atrac {

namespace {

constexpr std::uint8_t kAtrac3PlusGuid[16] = {0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44,
                                              0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62};

std::uint32_t u32(std::span<const std::byte> d, std::size_t at) {
    std::uint32_t v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}
std::uint16_t u16(std::span<const std::byte> d, std::size_t at) {
    std::uint16_t v;
    std::memcpy(&v, d.data() + at, 2);
    return v;
}

void put(nx::vector<std::byte>& out, const void* p, std::size_t n) {
    const auto* b = static_cast<const std::byte*>(p);
    out.insert(out.end(), b, b + n);
}
void put32(nx::vector<std::byte>& out, std::uint32_t v) { put(out, &v, 4); }
void put16(nx::vector<std::byte>& out, std::uint16_t v) { put(out, &v, 2); }

}  // namespace

bool parse(std::span<const std::byte> d, Info& out) {
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0)
        return false;
    out = Info{};
    bool have_fmt = false, have_data = false;
    for (std::size_t p = 12; p + 8 <= d.size();) {
        const std::uint32_t size = u32(d, p + 4);
        const std::size_t body = p + 8;
        if (body + size > d.size() && std::memcmp(d.data() + p, "data", 4) != 0) break;
        if (std::memcmp(d.data() + p, "fmt ", 4) == 0 && size >= 16) {
            const std::uint16_t tag = u16(d, body);
            out.channels = u16(d, body + 2);
            out.sample_rate = u32(d, body + 4);
            out.block_align = u16(d, body + 12);
            if (tag == 0x0270) {
                out.codec = Codec::Atrac3;
            } else if (tag == 0xFFFE && size >= 52 && std::memcmp(d.data() + body + 24, kAtrac3PlusGuid, 16) == 0) {
                out.codec = Codec::Atrac3Plus;
                out.codec_params = static_cast<std::uint16_t>((static_cast<unsigned>(d[body + 42]) << 8) |
                                                              static_cast<unsigned>(d[body + 43]));
            } else {
                return false;
            }
            have_fmt = true;
        } else if (std::memcmp(d.data() + p, "fact", 4) == 0 && size >= 8) {
            out.total_samples = u32(d, body);
            out.delay = u32(d, body + 4);
        } else if (std::memcmp(d.data() + p, "smpl", 4) == 0 && size >= 36) {
            const std::uint32_t count = u32(d, body + 28);
            for (std::uint32_t i = 0; i < count && body + 36 + i * 24 + 24 <= d.size(); ++i) {
                const std::size_t l = body + 36 + i * 24;
                out.loops.push_back({u32(d, l + 8), u32(d, l + 12)});
            }
        } else if (std::memcmp(d.data() + p, "data", 4) == 0) {
            out.data_offset = static_cast<std::uint32_t>(body);
            out.data_size = static_cast<std::uint32_t>(std::min<std::size_t>(size, d.size() - body));
            have_data = true;
            break;
        }
        p = body + size + (size & 1);
    }
    return have_fmt && have_data && out.block_align != 0;
}

nx::vector<std::byte> make_atrac3plus_riff(std::span<const std::byte> frames, std::uint16_t codec_params, int channels) {
    const std::uint32_t frame_bytes = ((codec_params & 0x3FFu) + 1) * 8;
    nx::vector<std::byte> out;
    out.reserve(frames.size() + 128);
    put(out, "RIFF", 4);
    put32(out, 0);  // patched below
    put(out, "WAVE", 4);
    put(out, "fmt ", 4);
    put32(out, 52);
    put16(out, 0xFFFE);
    put16(out, static_cast<std::uint16_t>(channels));
    put32(out, 44100);
    put32(out, frame_bytes * 44100 / 2048);
    put16(out, static_cast<std::uint16_t>(frame_bytes));
    put16(out, 0);       // bits per sample
    put16(out, 34);      // extension size
    put16(out, 2048);    // samples per block
    put32(out, channels == 1 ? 4u : 3u);  // channel mask
    put(out, kAtrac3PlusGuid, 16);
    const std::uint8_t extra[12] = {1, 0, static_cast<std::uint8_t>(codec_params >> 8),
                                    static_cast<std::uint8_t>(codec_params), 0, 0, 0, 0, 0, 0, 0, 0};
    put(out, extra, 12);
    put(out, "data", 4);
    put32(out, static_cast<std::uint32_t>(frames.size()));
    out.insert(out.end(), frames.begin(), frames.end());
    const auto riff_size = static_cast<std::uint32_t>(out.size() - 8);
    std::memcpy(out.data() + 4, &riff_size, 4);
    return out;
}

}  // namespace ff7cc::atrac
