#pragma once

// Ring buffer between a guest audio source and the device callback (platform/audio_miniaudio.cpp).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ff7cc::platform {

// The guest produces audio in virtual time (paced to the host clock) and the device consumes it on its own clock,
// so each source keeps a cushion between them: a stream starts playing once `target` frames are queued (again after
// every gap), and its playback rate is nudged by 0.2% while the queue sits well away from the target, which absorbs
// the drift between the two clocks. Exact samples are played otherwise.
constexpr std::size_t kRingFrames = 32768;            // ~740 ms of storage
constexpr std::size_t kMinTargetFrames = 44100 * 60 / 1000;
constexpr double kRateAdjust = 0.002;                 // ~3.5 cents, inaudible

// One audio source: single producer (the guest audio threads, which never run concurrently) / single consumer (the
// device callback).
struct AudioRing {
    std::vector<std::int16_t> data = std::vector<std::int16_t>(kRingFrames * 2);
    std::atomic<std::size_t> read{0}, write{0};
    // consumer state
    std::size_t target = kMinTargetFrames;
    bool playing = false;
    double level = 0.0;  // smoothed queue level (frames)
    double step = 1.0;   // input frames per output frame
    double frac = 0.0;   // position between `read` and the next frame while resampling
    std::atomic<std::uint64_t> underruns{0}, skipped_frames{0};
    // producer state
    std::uint64_t dropped_frames = 0;
    std::size_t pushes = 0, min_queued = SIZE_MAX, max_queued = 0;

    std::size_t queued() const { return write.load(std::memory_order_acquire) - read.load(std::memory_order_acquire); }

    void push(const std::int16_t* frames, std::size_t count) {
        const std::size_t level_now = queued();
        ++pushes;
        min_queued = std::min(min_queued, level_now);
        max_queued = std::max(max_queued, level_now);
        if (level_now + count + 4 > kRingFrames) {  // the device is not consuming (stopped)
            dropped_frames += count;
            return;
        }
        std::size_t w = write.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < count; ++i, ++w) {
            const std::size_t slot = (w % kRingFrames) * 2;
            data[slot] = frames[2 * i];
            data[slot + 1] = frames[2 * i + 1];
        }
        write.store(w, std::memory_order_release);
    }

    std::int32_t sample(std::size_t frame, int channel) const {
        return data[(frame % kRingFrames) * 2 + static_cast<std::size_t>(channel)];
    }

    // Adds `count` frames into `acc`; frames the stream does not have are silence.
    void mix_into(std::int32_t* acc, std::size_t count) {
        std::size_t r = read.load(std::memory_order_relaxed);
        const std::size_t w = write.load(std::memory_order_acquire);
        std::size_t queued_now = w - r;
        if (!playing) {
            if (queued_now < target) return;
            playing = true;
            level = static_cast<double>(queued_now);
            step = 1.0;
            frac = 0.0;
        }
        if (queued_now > 4 * target) {  // a burst after a long stall: drop the excess latency at once
            skipped_frames.fetch_add(queued_now - target, std::memory_order_relaxed);
            r = w - target;
            queued_now = target;
            level = static_cast<double>(target);
        }
        // Smoothed over ~0.5 s of callbacks, so the per-chunk sawtooth of the producer does not wobble the rate.
        level += (static_cast<double>(queued_now) - level) * 0.02;
        const double t = static_cast<double>(target);
        if (step == 1.0) {
            if (level > 1.5 * t) step = 1.0 + kRateAdjust;
            else if (level < 0.5 * t) step = 1.0 - kRateAdjust;
        }
        const bool settle = (step > 1.0 && level <= t) || (step < 1.0 && level >= t);
        for (std::size_t i = 0; i < count; ++i) {
            if (step == 1.0) {
                if (r == w) return underrun(r);
                acc[2 * i] += sample(r, 0);
                acc[2 * i + 1] += sample(r, 1);
                ++r;
                continue;
            }
            if (w - r < 3) return underrun(r);
            // Catmull-Rom between frames r and r+1 (r-1 is the frame played before, still in the ring).
            const double x = frac, x2 = x * x, x3 = x2 * x;
            const double c0 = -0.5 * x3 + x2 - 0.5 * x, c1 = 1.5 * x3 - 2.5 * x2 + 1.0;
            const double c2 = -1.5 * x3 + 2.0 * x2 + 0.5 * x, c3 = 0.5 * x3 - 0.5 * x2;
            for (int ch = 0; ch < 2; ++ch) {
                const double v = c0 * sample(r - 1, ch) + c1 * sample(r, ch) + c2 * sample(r + 1, ch) +
                                 c3 * sample(r + 2, ch);
                acc[2 * i + static_cast<std::size_t>(ch)] += static_cast<std::int32_t>(std::lround(v));
            }
            frac += step;
            while (frac >= 1.0) {
                frac -= 1.0;
                ++r;
            }
            // Back at the target: return to exact samples where the position is (nearly) on a frame.
            const double d = std::abs(step - 1.0) + 1e-9;
            if (settle && (frac < d || frac > 1.0 - d)) {
                if (frac > 0.5) ++r;
                step = 1.0;
                frac = 0.0;
            }
        }
        read.store(r, std::memory_order_release);
    }

    void underrun(std::size_t r) {
        read.store(r, std::memory_order_release);
        playing = false;
        underruns.fetch_add(1, std::memory_order_relaxed);
    }
};

}  // namespace ff7cc::platform
