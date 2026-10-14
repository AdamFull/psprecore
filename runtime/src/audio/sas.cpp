#include "audio/sas.hpp"

#include <algorithm>

#include "ff7cc/runtime/memory.hpp"

namespace ff7cc::rt::audio {

namespace {

// ---- PS-style envelope rates ---------------------------------------------------------------------------
//
// The simple-ADSR bitfields use the PlayStation SPU encoding: a 7-bit rate is a shift (rate >> 2) and a step
// (rate & 3); the SPU adds `step << (11 - shift)` to a 15-bit level once every `1 << (shift - 11)` samples.
// Scaled to the SAS 30-bit envelope height that is `step << (26 - shift)` per sample.

std::int64_t spu_rate(int step, int shift) {
    const std::int64_t base = static_cast<std::int64_t>(step) << 26;
    return shift >= 0 ? base >> shift : base << -shift;
}
std::int64_t increase_rate(int rate7) {
    if ((rate7 & 0x7F) == 0x7F) return 0;  // "infinitely slow"
    return std::max<std::int64_t>(spu_rate(7 - (rate7 & 3), (rate7 & 0x7F) >> 2), 1);
}
std::int64_t decrease_rate(int rate7) {
    if ((rate7 & 0x7F) == 0x7F) return 0;
    return std::max<std::int64_t>(spu_rate(8 - (rate7 & 3), (rate7 & 0x7F) >> 2), 1);
}

void apply(Curve curve, std::int64_t rate, std::int64_t& h) {
    switch (curve) {
        case Curve::LinearIncrease: h += rate; break;
        case Curve::LinearDecrease: h -= rate; break;
        case Curve::LinearBent: h += h < kEnvelopeMax * 3 / 4 ? rate : rate / 4; break;  // SPU "exponential" attack
        case Curve::ExponentDecrease: h -= (h * rate) >> 30; break;
        case Curve::ExponentIncrease: h += ((kEnvelopeMax - h) * rate) >> 30; break;
        case Curve::Direct: h = rate; break;
    }
}

constexpr int kFilters[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

}  // namespace

void Envelope::set_simple(std::uint32_t adsr1, std::uint32_t adsr2) {
    attack_curve = (adsr1 & 0x8000) ? Curve::LinearBent : Curve::LinearIncrease;
    attack_rate = increase_rate(static_cast<int>((adsr1 >> 8) & 0x7F));
    decay_curve = Curve::ExponentDecrease;  // 4-bit shift, fixed step -8
    decay_rate = decrease_rate(static_cast<int>(((adsr1 >> 4) & 0xF) << 2));
    sustain_level = static_cast<std::int64_t>((adsr1 & 0xF) + 1) << 26;
    const bool sustain_exp = adsr2 & 0x8000, sustain_down = adsr2 & 0x4000;
    const int sustain = static_cast<int>((adsr2 >> 6) & 0x7F);
    if (sustain_down) {
        sustain_curve = sustain_exp ? Curve::ExponentDecrease : Curve::LinearDecrease;
        sustain_rate = decrease_rate(sustain);
    } else {
        sustain_curve = sustain_exp ? Curve::LinearBent : Curve::LinearIncrease;
        sustain_rate = increase_rate(sustain);
    }
    release_curve = (adsr2 & 0x20) ? Curve::ExponentDecrease : Curve::LinearDecrease;
    release_rate = decrease_rate(static_cast<int>((adsr2 & 0x1F) << 2));
}

void Envelope::step() {
    switch (phase) {
        case Phase::Attack:
            apply(attack_curve, attack_rate, height);
            if (height >= kEnvelopeMax || attack_curve == Curve::Direct) {
                height = std::min(height, kEnvelopeMax);
                phase = Phase::Decay;
            }
            break;
        case Phase::Decay:
            apply(decay_curve, decay_rate, height);
            if (height <= sustain_level) {
                height = sustain_level;
                phase = Phase::Sustain;
            }
            break;
        case Phase::Sustain: apply(sustain_curve, sustain_rate, height); break;
        case Phase::Release:
            apply(release_curve, release_rate, height);
            if (height <= 0) {
                height = 0;
                phase = Phase::Off;
            }
            break;
        case Phase::Off: break;
    }
    height = std::clamp<std::int64_t>(height, 0, kEnvelopeMax);
}

void Sas::set_vag(int index, std::uint32_t addr, std::uint32_t size, bool loop) {
    Voice* v = voice(index);
    if (!v) return;
    v->kind = Voice::Kind::Vag;
    v->addr = addr;
    v->size = size;
    v->loop = loop;
}

void Sas::set_noise(int index, int frequency) {
    Voice* v = voice(index);
    if (!v) return;
    v->kind = Voice::Kind::Noise;
    v->noise_frequency = frequency & 63;
}

void Sas::key_on(int index) {
    Voice* v = voice(index);
    if (!v || v->kind == Voice::Kind::None) return;
    v->playing = true;
    v->paused = false;
    v->pos = v->loop_pos = 0;
    v->end_after_block = false;
    v->block_index = 28;
    v->history1 = v->history2 = 0;
    v->s0 = v->s1 = 0;
    v->frac = 0;
    v->envelope.key_on();
}

void Sas::key_off(int index) {
    if (Voice* v = voice(index)) v->envelope.key_off();
}

std::uint32_t Sas::end_flags() const {
    std::uint32_t flags = 0;
    for (int i = 0; i < 32; ++i)
        if (!voices_[static_cast<std::size_t>(i)].playing) flags |= 1u << i;
    return flags;
}

std::uint32_t Sas::pause_flags() const {
    std::uint32_t flags = 0;
    for (int i = 0; i < 32; ++i)
        if (voices_[static_cast<std::size_t>(i)].paused) flags |= 1u << i;
    return flags;
}

void Sas::set_pause(std::uint32_t mask, bool pause) {
    for (int i = 0; i < 32; ++i)
        if (mask & (1u << i)) voices_[static_cast<std::size_t>(i)].paused = pause;
}

// Decodes the next source sample of a voice (VAG ADPCM at 28 samples per 16-byte block, or noise).
std::int32_t Sas::next_source_sample(const GuestMemory& memory, Voice& v) {
    if (v.kind == Voice::Kind::Noise) {
        // Noise clock as on the SPU: frequency = shift (bits 2-5) and step (bits 0-1).
        const int shift = v.noise_frequency >> 2, step = (v.noise_frequency & 3) + 4;
        v.noise_counter += static_cast<std::uint32_t>(step << shift);
        while (v.noise_counter >= 0x20000) {
            v.noise_counter -= 0x20000;
            const std::uint32_t bit = ((v.noise_lfsr >> 15) ^ (v.noise_lfsr >> 12) ^ (v.noise_lfsr >> 11) ^ (v.noise_lfsr >> 10) ^ 1) & 1;
            v.noise_lfsr = (v.noise_lfsr << 1) | bit;
        }
        return static_cast<std::int16_t>(v.noise_lfsr);
    }
    if (v.block_index >= 28) {
        if (v.end_after_block || v.pos + 16 > v.size) {
            v.playing = false;
            return 0;
        }
        const std::uint8_t* b = memory.ptr(v.addr + v.pos);
        int shift = b[0] & 15;
        if (shift > 12) shift = 9;  // SPU quirk: shifts 13-15 act like 9
        const int filter = std::min((b[0] >> 4) & 7, 4);
        const std::uint8_t flags = b[1];
        if (flags & 4) v.loop_pos = v.pos;
        for (int i = 0; i < 28; ++i) {
            const int nibble = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
            int s = static_cast<std::int16_t>(static_cast<std::uint16_t>(nibble << 12)) >> shift;
            s += (v.history1 * kFilters[filter][0] + v.history2 * kFilters[filter][1] + 32) >> 6;
            s = std::clamp(s, -32768, 32767);
            v.history2 = v.history1;
            v.history1 = s;
            v.block[i] = static_cast<std::int16_t>(s);
        }
        v.block_index = 0;
        if (flags & 1) {  // end of data: loop or stop after this block
            if ((flags & 2) || v.loop) v.pos = v.loop_pos;
            else v.end_after_block = true;
        } else {
            v.pos += 16;
        }
    }
    return v.block[v.block_index++];
}

void Sas::mix(const GuestMemory& memory, std::int16_t* out, const std::int16_t* mix_in, int left, int right) {
    const int n = grain_;
    static thread_local std::int32_t acc[2 * 4096];
    std::fill(acc, acc + 2 * n, 0);
    for (int vi = 0; vi < max_voices_ && vi < 32; ++vi) {
        Voice& v = voices_[static_cast<std::size_t>(vi)];
        if (!v.playing || v.paused) continue;
        for (int i = 0; i < n && v.playing; ++i) {
            const std::int32_t sample = v.s0 + (((v.s1 - v.s0) * static_cast<std::int32_t>(v.frac)) >> 12);
            v.frac += static_cast<std::uint32_t>(std::clamp(v.pitch, 0, 0x4000));
            while (v.frac >= 0x1000) {
                v.frac -= 0x1000;
                v.s0 = v.s1;
                v.s1 = next_source_sample(memory, v);
            }
            v.envelope.step();
            const std::int32_t enveloped = static_cast<std::int32_t>((sample * (v.envelope.height >> 15)) >> 15);
            acc[2 * i] += (enveloped * v.volume_left) >> 12;
            acc[2 * i + 1] += (enveloped * v.volume_right) >> 12;
            if (v.envelope.phase == Envelope::Phase::Off) v.playing = false;
        }
    }
    for (int i = 0; i < n; ++i) {
        std::int32_t l = acc[2 * i], r = acc[2 * i + 1];
        if (mix_in) {
            l += (mix_in[2 * i] * left) >> 12;
            r += (mix_in[2 * i + 1] * right) >> 12;
        }
        out[2 * i] = static_cast<std::int16_t>(std::clamp(l, -32768, 32767));
        out[2 * i + 1] = static_cast<std::int16_t>(std::clamp(r, -32768, 32767));
    }
}

}  // namespace ff7cc::rt::audio
