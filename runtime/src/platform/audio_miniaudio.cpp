#include "psprecore/platform/audio.hpp"

#include "platform/audio_ring.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include <miniaudio.h>

#include "core/foundation/core/foundation.h"

namespace psprecore::platform {

struct AudioDevice::Impl {
    ma_device device{};
    bool ok = false;
    std::array<AudioRing, rt::AudioSink::kSources> rings;
    nx::vector<std::int32_t> acc;
    std::atomic<std::int32_t> gain{256};  // master volume, 256 = unity

    static void callback(ma_device* device, void* output, const void*, ma_uint32 frames) {
        auto* self = static_cast<Impl*>(device->pUserData);
        self->acc.assign(static_cast<std::size_t>(frames) * 2, 0);
        for (AudioRing& ring : self->rings) ring.mix_into(self->acc.data(), frames);
        auto* out = static_cast<std::int16_t*>(output);
        const std::int32_t gain = self->gain.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < self->acc.size(); ++i)
            out[i] = static_cast<std::int16_t>(std::clamp((self->acc[i] * gain) >> 8, -32768, 32767));
    }
};

AudioDevice::AudioDevice() : impl_(nx::make_unique<Impl>()) {
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = 2;
    config.sampleRate = 44100;
    config.dataCallback = &Impl::callback;
    config.pUserData = impl_.get();
    config.periodSizeInMilliseconds = 10;
    impl_->acc.reserve(8192);
    if (ma_device_init(nullptr, &config, &impl_->device) != MA_SUCCESS) {
        std::fprintf(stderr, "audio: no playback device, running silent\n");
        return;
    }
    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        ma_device_uninit(&impl_->device);
        std::fprintf(stderr, "audio: failed to start the playback device, running silent\n");
        return;
    }
    impl_->ok = true;
    // The cushion covers a few device periods (AAudio and some WASAPI devices use large bursts).
    const std::size_t period = impl_->device.playback.internalPeriodSizeInFrames;
    for (AudioRing& ring : impl_->rings) ring.target = std::max(kMinTargetFrames, 3 * period);
    std::fprintf(stderr, "Audio device: %s (period %zu frames, cushion %.0f ms)\n", impl_->device.playback.name,
                 period, static_cast<double>(impl_->rings[0].target) / 44.1);
}

AudioDevice::~AudioDevice() {
    if (impl_->ok) ma_device_uninit(&impl_->device);
}

bool AudioDevice::ok() const { return impl_->ok; }

void AudioDevice::set_paused(bool paused) {
    if (!impl_->ok) return;
    if (paused) ma_device_stop(&impl_->device);
    else ma_device_start(&impl_->device);
}

void AudioDevice::set_volume(float volume) {
    impl_->gain.store(static_cast<std::int32_t>(std::clamp(volume, 0.0f, 1.0f) * 256.0f + 0.5f), std::memory_order_relaxed);
}

AudioDevice::Stats AudioDevice::take_stats() {
    Stats st;
    st.min_queued = SIZE_MAX;
    for (AudioRing& ring : impl_->rings) {
        st.underruns += ring.underruns.exchange(0, std::memory_order_relaxed);
        st.skipped_frames += ring.skipped_frames.exchange(0, std::memory_order_relaxed);
        st.dropped_frames += std::exchange(ring.dropped_frames, 0);
        if (std::exchange(ring.pushes, 0)) {
            st.min_queued = std::min(st.min_queued, ring.min_queued);
            st.max_queued = std::max(st.max_queued, ring.max_queued);
        }
        ring.min_queued = SIZE_MAX;
        ring.max_queued = 0;
    }
    if (st.min_queued == SIZE_MAX) st.min_queued = 0;
    return st;
}

void AudioDevice::submit(int source, const std::int16_t* frames, std::size_t count) {
    if (impl_->ok && source >= 0 && source < kSources) impl_->rings[static_cast<std::size_t>(source)].push(frames, count);
}

}  // namespace psprecore::platform
