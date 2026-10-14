#include "audio/vorbis.hpp"

#define STB_VORBIS_NO_PUSHDATA_API
#include <stb_vorbis.c>

// stb_vorbis defines single-letter macros (L, C, R) that clash with ordinary identifiers.
#undef L
#undef C
#undef R

namespace psprecore::rt::audio {

std::unique_ptr<VorbisStream> VorbisStream::open(nx::fs::path_view path) {
    auto file = nx::fs::file_read(path);
    if (!file.has_value() || file.value().size() > 0x7FFFFFFF) return nullptr;
    std::unique_ptr<VorbisStream> s(new VorbisStream);
    s->file_ = std::move(file.value());
    int error = 0;
    stb_vorbis* v = stb_vorbis_open_memory(s->file_.data(), static_cast<int>(s->file_.size()), &error, nullptr);
    if (!v) return nullptr;
    s->vorbis_ = v;
    const stb_vorbis_info info = stb_vorbis_get_info(v);
    s->channels_ = info.channels;
    s->sample_rate_ = info.sample_rate;
    s->length_ = stb_vorbis_stream_length_in_samples(v);
    return s;
}

VorbisStream::~VorbisStream() {
    if (vorbis_) stb_vorbis_close(vorbis_);
}

std::uint32_t VorbisStream::read(std::uint64_t position, std::int16_t* out, std::uint32_t frames) {
    if (position >= length_) return 0;
    if (position != cursor_) {
        if (!stb_vorbis_seek(vorbis_, static_cast<unsigned>(position))) return 0;
        cursor_ = position;
    }
    std::uint32_t done = 0;
    while (done < frames) {
        const int got = stb_vorbis_get_samples_short_interleaved(vorbis_, channels_, out + done * channels_,
                                                                 static_cast<int>((frames - done) * channels_));
        if (got <= 0) break;
        done += static_cast<std::uint32_t>(got);
    }
    cursor_ += done;
    return done;
}

}  // namespace psprecore::rt::audio
