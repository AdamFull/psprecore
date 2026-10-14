#pragma once

// Host audio output (miniaudio): one ring buffer per guest audio source, mixed in the device callback.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "psprecore/runtime/audio.hpp"

namespace psprecore::platform {

class AudioDevice final : public rt::AudioSink {
public:
    AudioDevice();  // opens the default playback device at 44.1 kHz stereo; check ok()
    ~AudioDevice() override;
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    bool ok() const;
    void set_volume(float volume);  // master volume, 0..1
    void set_paused(bool paused);   // stops the device (e.g. while the app is in the background)
    void submit(int source, const std::int16_t* frames, std::size_t count) override;

    // Counters since the last call: gaps in a playing stream (including streams that end), frames skipped to cut
    // latency after a stall, frames dropped because a ring was full, and the lowest / highest queue level seen
    // when the guest submitted (frames).
    struct Stats {
        std::uint64_t underruns = 0, skipped_frames = 0, dropped_frames = 0;
        std::size_t min_queued = 0, max_queued = 0;
    };
    Stats take_stats();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace psprecore::platform
