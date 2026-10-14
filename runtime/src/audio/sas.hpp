#pragma once

// sceSasCore software mixer: up to 32 voices of VAG ADPCM (or noise) with pitch, ADSR envelopes and per-voice
// stereo volume, mixed one grain at a time into 16-bit stereo. Reverb parameters are recorded but the wet path
// is not implemented yet (voices play dry).

#include <array>
#include <cstdint>

namespace ff7cc::rt {
class GuestMemory;
}

namespace ff7cc::rt::audio {

inline constexpr std::int64_t kEnvelopeMax = 0x40000000;

enum class Curve : int { LinearIncrease = 0, LinearDecrease = 1, LinearBent = 2, ExponentDecrease = 3, ExponentIncrease = 4, Direct = 5 };

struct Envelope {
    enum class Phase { Attack, Decay, Sustain, Release, Off } phase = Phase::Off;
    Curve attack_curve = Curve::LinearIncrease, decay_curve = Curve::ExponentDecrease;
    Curve sustain_curve = Curve::LinearDecrease, release_curve = Curve::LinearDecrease;
    std::int64_t attack_rate = kEnvelopeMax, decay_rate = 0, sustain_rate = 0, release_rate = kEnvelopeMax;
    std::int64_t sustain_level = kEnvelopeMax;
    std::int64_t height = 0;

    void key_on() {
        phase = Phase::Attack;
        height = 0;
    }
    void key_off() {
        if (phase != Phase::Off) phase = Phase::Release;
    }
    void step();
    // PS-style ADSR1/ADSR2 bitfields (sceSasSetSimpleADSR).
    void set_simple(std::uint32_t adsr1, std::uint32_t adsr2);
};

struct Voice {
    enum class Kind { None, Vag, Noise } kind = Kind::None;
    std::uint32_t addr = 0, size = 0, pos = 0, loop_pos = 0;
    bool loop = false, playing = false, paused = false, end_after_block = false;
    int pitch = 0x1000;
    int volume_left = 0x1000, volume_right = 0x1000, effect_left = 0, effect_right = 0;
    int noise_frequency = 0;
    Envelope envelope;

    // Decoder state.
    std::int16_t block[28] = {};
    int block_index = 28;
    int history1 = 0, history2 = 0;
    std::int32_t s0 = 0, s1 = 0;
    std::uint32_t frac = 0;
    std::uint32_t noise_lfsr = 1, noise_counter = 0;
};

class Sas {
public:
    void init(int grain, int voices) {
        grain_ = grain;
        max_voices_ = voices;
    }
    int grain() const { return grain_; }
    void set_grain(int grain) { grain_ = grain; }
    int output_mode = 0;

    Voice* voice(int v) { return v >= 0 && v < static_cast<int>(voices_.size()) ? &voices_[static_cast<std::size_t>(v)] : nullptr; }
    void set_vag(int v, std::uint32_t addr, std::uint32_t size, bool loop);
    void set_noise(int v, int frequency);
    void key_on(int v);
    void key_off(int v);
    std::uint32_t end_flags() const;
    std::uint32_t pause_flags() const;
    void set_pause(std::uint32_t mask, bool pause);

    // Reverb settings (stored; not rendered yet).
    int reverb_type = 0, reverb_delay = 0, reverb_feedback = 0, reverb_left = 0, reverb_right = 0;
    bool reverb_dry = true, reverb_wet = false;

    // Mixes one grain. `mix_in` (optional) is added after scaling by left/right volume (0x1000 = unity).
    void mix(const GuestMemory& memory, std::int16_t* out, const std::int16_t* mix_in, int left, int right);

private:
    std::int32_t next_source_sample(const GuestMemory& memory, Voice& v);

    int grain_ = 256, max_voices_ = 32;
    std::array<Voice, 32> voices_{};
};

}  // namespace ff7cc::rt::audio
