#include "media/av1_video.hpp"

#include <algorithm>
#include <cstring>

#include <dav1d/dav1d.h>

#include "core/foundation/core/foundation.h"

namespace ff7cc::rt::media {

namespace {

std::uint32_t le32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t{p[3]} << 24); }

int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// BT.601 YUV -> RGB in 16.16 fixed point, limited or full range.
void convert(const Dav1dPicture& pic, nx::vector<std::uint32_t>& rgba) {
    const int w = pic.p.w, h = pic.p.h;
    rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    const bool full = pic.seq_hdr && pic.seq_hdr->color_range;
    const auto* yp = static_cast<const std::uint8_t*>(pic.data[0]);
    const auto* up = static_cast<const std::uint8_t*>(pic.data[1]);
    const auto* vp = static_cast<const std::uint8_t*>(pic.data[2]);
    const std::ptrdiff_t ys = pic.stride[0], cs = pic.stride[1];
    const bool sub_x = pic.p.layout != DAV1D_PIXEL_LAYOUT_I444, sub_y = pic.p.layout == DAV1D_PIXEL_LAYOUT_I420;
    // Coefficients (x65536): full range 1.402, 0.344, 0.714, 1.772; limited range scaled by 255/219 and 255/224.
    const int ky = full ? 65536 : 76309, ky0 = full ? 0 : 16;
    const int krv = full ? 91881 : 104597, kgu = full ? 22554 : 25675, kgv = full ? 46802 : 53279, kbu = full ? 116130 : 132201;
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* row = yp + y * ys;
        const std::uint8_t* urow = up + (sub_y ? y / 2 : y) * cs;
        const std::uint8_t* vrow = vp + (sub_y ? y / 2 : y) * cs;
        std::uint32_t* out = rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        for (int x = 0; x < w; ++x) {
            const int c = (row[x] - ky0) * ky;
            const int u = urow[sub_x ? x / 2 : x] - 128, v = vrow[sub_x ? x / 2 : x] - 128;
            const int r = clamp255((c + krv * v + 32768) >> 16);
            const int g = clamp255((c - kgu * u - kgv * v + 32768) >> 16);
            const int b = clamp255((c + kbu * u + 32768) >> 16);
            out[x] = static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) |
                     (static_cast<std::uint32_t>(b) << 16) | 0xFF000000u;
        }
    }
}

}  // namespace

std::unique_ptr<Av1Video> Av1Video::open(const std::filesystem::path& path) {
#ifdef _WIN32
    std::FILE* f = _wfopen(path.c_str(), L"rb");
#else
    std::FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return nullptr;
    std::uint8_t header[32];
    if (std::fread(header, 1, 32, f) != 32 || std::memcmp(header, "DKIF", 4) != 0 || std::memcmp(header + 8, "AV01", 4) != 0) {
        std::fclose(f);
        return nullptr;
    }
    std::unique_ptr<Av1Video> v(new Av1Video);
    v->file_ = f;
    v->width_ = header[12] | (header[13] << 8);
    v->height_ = header[14] | (header[15] << 8);
    const std::uint32_t header_size = header[6] | (header[7] << 8);
    std::fseek(f, static_cast<long>(header_size), SEEK_SET);

    Dav1dSettings settings;
    dav1d_default_settings(&settings);
    settings.n_threads = 2;
    settings.max_frame_delay = 1;
    if (dav1d_open(&v->decoder_, &settings) < 0) return nullptr;
    return v;
}

Av1Video::~Av1Video() {
    if (decoder_) dav1d_close(&decoder_);
    if (file_) std::fclose(file_);
}

bool Av1Video::read_packet(nx::vector<std::uint8_t>& out) {
    std::uint8_t frame_header[12];
    if (std::fread(frame_header, 1, 12, file_) != 12) return false;
    out.resize(le32(frame_header));
    return std::fread(out.data(), 1, out.size(), file_) == out.size();
}

bool Av1Video::next(nx::vector<std::uint32_t>& rgba) {
    Dav1dPicture pic{};
    for (;;) {
        const int got = dav1d_get_picture(decoder_, &pic);
        if (got == 0) {
            convert(pic, rgba);
            dav1d_picture_unref(&pic);
            return true;
        }
        if (got != DAV1D_ERR(EAGAIN)) return false;
        if (pending_offset_ >= pending_.size()) {
            if (eof_ || !read_packet(pending_)) {
                if (eof_) return false;  // drained: no picture came out after the last packet
                eof_ = true;
                pending_.clear();
                continue;  // let the decoder flush its delayed pictures
            }
            pending_offset_ = 0;
        }
        Dav1dData data{};
        std::uint8_t* buffer = dav1d_data_create(&data, pending_.size() - pending_offset_);
        std::memcpy(buffer, pending_.data() + pending_offset_, data.sz);
        const int sent = dav1d_send_data(decoder_, &data);
        if (sent < 0 && sent != DAV1D_ERR(EAGAIN)) {
            dav1d_data_unref(&data);
            return false;
        }
        pending_offset_ += (pending_.size() - pending_offset_) - data.sz;  // bytes consumed
        dav1d_data_unref(&data);
    }
}

}  // namespace ff7cc::rt::media
