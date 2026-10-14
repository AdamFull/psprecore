#pragma once

// Audio output boundary between the HLE layer and the host frontend. sceAudio channels and the SRC/Output2
// stream submit 44.1 kHz stereo 16-bit frames here; the frontend mixes and plays them (miniaudio).

#include <cstddef>
#include <cstdint>

namespace psprecore::rt {

class AudioSink {
public:
    static constexpr int kSources = 9;  // sceAudio channels 0-7, Output2/SRC = 8
    static constexpr int kOutput2 = 8;

    virtual ~AudioSink() = default;
    // Interleaved stereo frames; the frontend applies no further volume.
    virtual void submit(int source, const std::int16_t* frames, std::size_t count) = 0;
};

// The sink receives audio from then on; null (the default, e.g. headless) discards it.
void set_audio_sink(AudioSink* sink);

}  // namespace psprecore::rt
