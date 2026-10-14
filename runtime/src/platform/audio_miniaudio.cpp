#include "ff7cc/platform/audio.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <vector>

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include <miniaudio.h>

namespace ff7cc::platform {

namespace {

constexpr std::size_t kRingFrames = 16384;  // ~370 ms
constexpr std::size_t kMaxQueuedFrames = 44100 / 5;  // drop input beyond ~200 ms to bound latency

// Single producer (the guest audio threads, which never run concurrently) / single consumer (device callback).
struct Ring {
    std::vector<std::int16_t> data = std::vector<std::int16_t>(kRingFrames * 2);
    std::atomic<std::size_t> read{0}, write{0};

    std::size_t queued() const { return write.load(std::memory_order_acquire) - read.load(std::memory_order_acquire); }
    void push(const std::int16_t* frames, std::size_t count) {
        if (queued() + count > kMaxQueuedFrames) return;
        std::size_t w = write.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < count; ++i, ++w) {
            const std::size_t slot = (w % kRingFrames) * 2;
            data[slot] = frames[2 * i];
            data[slot + 1] = frames[2 * i + 1];
        }
        write.store(w, std::memory_order_release);
    }
    // Adds up to `count` frames into `acc`; missing frames are silence.
    void mix_into(std::int32_t* acc, std::size_t count) {
        std::size_t r = read.load(std::memory_order_relaxed);
        const std::size_t available = std::min(count, write.load(std::memory_order_acquire) - r);
        for (std::size_t i = 0; i < available; ++i, ++r) {
            const std::size_t slot = (r % kRingFrames) * 2;
            acc[2 * i] += data[slot];
            acc[2 * i + 1] += data[slot + 1];
        }
        read.store(r, std::memory_order_release);
    }
};

}  // namespace

struct AudioDevice::Impl {
    ma_device device{};
    bool ok = false;
    std::array<Ring, rt::AudioSink::kSources> rings;
    std::vector<std::int32_t> acc;
    std::atomic<std::int32_t> gain{256};  // master volume, 256 = unity

    static void callback(ma_device* device, void* output, const void*, ma_uint32 frames) {
        auto* self = static_cast<Impl*>(device->pUserData);
        self->acc.assign(static_cast<std::size_t>(frames) * 2, 0);
        for (Ring& ring : self->rings) ring.mix_into(self->acc.data(), frames);
        auto* out = static_cast<std::int16_t*>(output);
        const std::int32_t gain = self->gain.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < self->acc.size(); ++i)
            out[i] = static_cast<std::int16_t>(std::clamp((self->acc[i] * gain) >> 8, -32768, 32767));
    }
};

AudioDevice::AudioDevice() : impl_(std::make_unique<Impl>()) {
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
    std::fprintf(stderr, "Audio device: %s\n", impl_->device.playback.name);
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

void AudioDevice::submit(int source, const std::int16_t* frames, std::size_t count) {
    if (impl_->ok && source >= 0 && source < kSources) impl_->rings[static_cast<std::size_t>(source)].push(frames, count);
}

}  // namespace ff7cc::platform
