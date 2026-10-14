#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "audio/reverb.hpp"
#include "test_main.hpp"

using ff7cc::rt::audio::Reverb;

namespace {

// Feeds `send` through the reverb in grains of 256 frames; returns the wet output.
std::vector<std::int32_t> run(Reverb& reverb, const std::vector<std::int32_t>& send) {
    std::vector<std::int32_t> out(send.size(), 0);
    const int frames = static_cast<int>(send.size() / 2);
    for (int at = 0; at < frames; at += 256)
        reverb.process(send.data() + 2 * at, out.data() + 2 * at, std::min(256, frames - at), 0x1000, 0x1000);
    return out;
}

double energy(const std::vector<std::int32_t>& v, std::size_t from, std::size_t to) {
    double e = 0;
    for (std::size_t i = from; i < to; ++i) e += static_cast<double>(v[i]) * v[i];
    return e;
}

}  // namespace

TEST_CASE("reverb: off adds nothing") {
    Reverb reverb;
    reverb.set_type(0);
    std::vector<std::int32_t> send(44100 * 2, 10000);
    const auto out = run(reverb, send);
    CHECK(energy(out, 0, out.size()) == 0.0);
}

TEST_CASE("reverb: an impulse gives a delayed, decaying tail") {
    for (int type = 1; type <= 6; ++type) {
        Reverb reverb;
        reverb.set_type(type);
        std::vector<std::int32_t> send(44100 * 2 * 3, 0);  // 3 s
        send[0] = send[1] = send[2] = send[3] = 30000;
        const auto out = run(reverb, send);
        const std::size_t second = 44100 * 2;
        const double first = energy(out, 0, second), last = energy(out, 2 * second, 3 * second);
        CHECK(out[0] == 0 && out[1] == 0);  // nothing before the first reflection
        CHECK(first > 0.0);
        CHECK(last < first * 0.01);  // studio/hall tails die out within ~2 s
    }
}

TEST_CASE("reverb: full-scale noise stays bounded") {
    Reverb reverb;
    reverb.set_type(5);  // hall: the longest tail
    std::vector<std::int32_t> send(44100 * 2 * 2);
    std::srand(1);
    for (auto& s : send) s = (std::rand() % 65536) - 32768;
    const auto out = run(reverb, send);
    bool bounded = true;
    for (const auto s : out) bounded = bounded && std::abs(s) <= 32768;
    CHECK(bounded);
}
