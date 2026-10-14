#pragma once

// Host audio output (miniaudio): one ring buffer per guest audio source, mixed in the device callback.

#include <memory>

#include "ff7cc/runtime/audio.hpp"

namespace ff7cc::platform {

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ff7cc::platform
