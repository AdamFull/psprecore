#include <cstdint>
#include <vector>

#include "platform/audio_ring.hpp"
#include "test_main.hpp"

using ff7cc::platform::AudioRing;

namespace {

constexpr std::size_t kPeriod = 441;  // 10 ms device callbacks

std::vector<std::int16_t> constant(std::size_t frames, std::int16_t value) {
    return std::vector<std::int16_t>(frames * 2, value);
}

// Runs `seconds` of a producer pushing `chunk` frames every `chunk / rate` seconds of device time (rate 1.0 =
// exactly the device clock) against 10 ms device callbacks; returns the output.
std::vector<std::int32_t> run(AudioRing& ring, double seconds, std::size_t chunk, double rate, std::int16_t value) {
    std::vector<std::int32_t> out;
    const auto pcm = constant(chunk, value);
    double produced = 0.0;  // in device frames
    const auto callbacks = static_cast<std::size_t>(seconds * 100.0);
    for (std::size_t c = 0; c < callbacks; ++c) {
        while (produced < static_cast<double>((c + 1) * kPeriod)) {
            ring.push(pcm.data(), chunk);
            produced += static_cast<double>(chunk) / rate;
        }
        std::vector<std::int32_t> acc(kPeriod * 2, 0);
        ring.mix_into(acc.data(), kPeriod);
        out.insert(out.end(), acc.begin(), acc.end());
    }
    return out;
}

}  // namespace

TEST_CASE("audio ring: a stream starts once the cushion is queued") {
    AudioRing ring;
    const auto pcm = constant(ring.target - 1, 100);
    ring.push(pcm.data(), pcm.size() / 2);
    std::vector<std::int32_t> acc(kPeriod * 2, 0);
    ring.mix_into(acc.data(), kPeriod);
    CHECK(acc[0] == 0 && ring.queued() == ring.target - 1);
    const auto one = constant(1, 100);
    ring.push(one.data(), 1);
    ring.mix_into(acc.data(), kPeriod);
    CHECK(acc[0] == 100 && acc[2 * kPeriod - 1] == 100);
    CHECK(ring.queued() == ring.target - kPeriod);
}

TEST_CASE("audio ring: a gap is silence and the stream primes again") {
    AudioRing ring;
    const auto pcm = constant(ring.target, 7);
    ring.push(pcm.data(), ring.target);
    std::vector<std::int32_t> acc;
    for (int i = 0; i < 10; ++i) {
        acc.assign(kPeriod * 2, 0);
        ring.mix_into(acc.data(), kPeriod);
    }
    CHECK(ring.underruns.load() == 1);
    CHECK(acc[0] == 0 && ring.queued() == 0);
    ring.push(pcm.data(), kPeriod);  // less than the cushion: still waiting
    acc.assign(kPeriod * 2, 0);
    ring.mix_into(acc.data(), kPeriod);
    CHECK(acc[0] == 0);
}

TEST_CASE("audio ring: clock drift is absorbed without gaps") {
    for (const double rate : {1.001, 0.999}) {  // producer 0.1% fast / slow against the device
        AudioRing ring;
        const auto out = run(ring, 300.0, 1024, rate, 1000);
        CHECK(ring.underruns.load() == 0);
        CHECK(ring.skipped_frames.load() == 0);
        CHECK(ring.queued() < 2 * ring.target + 1024);
        CHECK(ring.queued() > ring.target / 4);
        // Interpolating a constant gives the constant: no clicks from the rate changes.
        std::size_t first = 0;
        while (out[first] == 0) ++first;
        bool exact = true;
        for (std::size_t i = first; i < out.size(); ++i) exact = exact && out[i] == 1000;
        CHECK(exact);
    }
}

TEST_CASE("audio ring: a burst after a stall is cut back to the cushion") {
    AudioRing ring;
    const auto pcm = constant(5 * ring.target, 1);
    ring.push(pcm.data(), 5 * ring.target);
    std::vector<std::int32_t> acc(kPeriod * 2, 0);
    ring.mix_into(acc.data(), kPeriod);
    CHECK(ring.skipped_frames.load() == 4 * ring.target);
    CHECK(ring.queued() == ring.target - kPeriod);
}
