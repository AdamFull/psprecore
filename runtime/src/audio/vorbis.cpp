#include "audio/vorbis.hpp"

#define STB_VORBIS_NO_PUSHDATA_API
#include <stb_vorbis.c>

// stb_vorbis defines single-letter macros (L, C, R) that clash with ordinary identifiers.
#undef L
#undef C
#undef R

namespace ff7cc::rt::audio {

std::unique_ptr<VorbisStream> VorbisStream::open(const std::filesystem::path& path) {
    int error = 0;
    stb_vorbis* v = stb_vorbis_open_filename(path.string().c_str(), &error, nullptr);
    if (!v) return nullptr;
    std::unique_ptr<VorbisStream> s(new VorbisStream);
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

}  // namespace ff7cc::rt::audio
