#include "audio/reverb.hpp"

#include <algorithm>

namespace ff7cc::rt::audio {

namespace {

// Register order (SPU 1F801DC0h..1F801DFEh).
enum Reg {
    dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2,
    mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF,
    mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN,
};

std::int32_t mul(std::int32_t a, std::int32_t b) { return (a * b) >> 15; }
std::int32_t sat(std::int32_t v) { return std::clamp(v, -32768, 32767); }

}  // namespace

// The SPU reverb presets (psx-spx "SPU Reverb Examples"), by sceSasRevType. ECHO and PIPE are taken to be the
// SPU's "chaos echo" and "half echo" presets.
const Reverb::Preset& Reverb::preset(int type) {
    static const Preset kPresets[10] = {
        {{}, 0},  // off
        {{0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0,
          0x0227, 0x0374, 0x01EF, 0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
          0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000},
         0x26C0},  // room
        {{0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00, 0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4,
          0x02AF, 0x0372, 0x0266, 0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7, 0x018F, 0x00B5,
          0x00B4, 0x0080, 0x004C, 0x0026, 0x8000, 0x8000},
         0x1F40},  // studio small
        {{0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0, 0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824,
          0x065F, 0x07A2, 0x0616, 0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7, 0x042F, 0x0265,
          0x0264, 0x01B2, 0x0100, 0x0080, 0x8000, 0x8000},
         0x4840},  // studio medium
        {{0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09,
          0x0A3C, 0x0BD9, 0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2, 0x05EA, 0x031D,
          0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000},
         0x6FE0},  // studio large
        {{0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2,
          0x10BD, 0x11BC, 0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD, 0x09C2, 0x05C1,
          0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000},
         0xADE0},  // hall
        {{0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000, 0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14,
          0x183B, 0x1BC2, 0x16B2, 0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D, 0x1056, 0x0AE1,
          0x0AE0, 0x07A2, 0x0464, 0x0232, 0x8000, 0x8000},
         0xF6C0},  // space echo
        {{0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1FFF, 0x0FFF, 0x1005,
          0x0005, 0x0000, 0x0000, 0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
          0x1004, 0x1002, 0x0004, 0x0002, 0x8000, 0x8000},
         0x18040},  // echo
        {{0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1FFF, 0x0FFF, 0x1005,
          0x0005, 0x0000, 0x0000, 0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
          0x1004, 0x1002, 0x0004, 0x0002, 0x8000, 0x8000},
         0x18040},  // delay
        {{0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500, 0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5,
          0x01DF, 0x02B0, 0x01D7, 0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059, 0x01A0, 0x00E3,
          0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000},
         0x3C00},  // pipe
    };
    return kPresets[type >= 0 && type < 10 ? type : 0];
}

void Reverb::set_type(int type) {
    if (type < 0 || type > 9) type = 0;
    if (type == type_ && (type == 0 || !buffer_.empty())) return;
    type_ = type;
    preset_ = type ? &preset(type) : nullptr;
    buffer_.assign(preset_ ? preset_->bytes / 2 : 0, 0);
    position_ = 0;
    odd_ = false;
    held_left_ = held_right_ = out_left_ = out_right_ = prev_out_left_ = prev_out_right_ = 0;
}

std::int32_t Reverb::at(std::int32_t offset) const {
    const auto n = static_cast<std::int64_t>(buffer_.size());
    std::int64_t i = (static_cast<std::int64_t>(position_) + offset) % n;
    if (i < 0) i += n;
    return buffer_[static_cast<std::size_t>(i)];
}

void Reverb::put(std::int32_t offset, std::int32_t value) {
    const auto n = static_cast<std::int64_t>(buffer_.size());
    std::int64_t i = (static_cast<std::int64_t>(position_) + offset) % n;
    if (i < 0) i += n;
    buffer_[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(sat(value));
}

void Reverb::tick(std::int32_t in_left, std::int32_t in_right, std::int32_t& out_left, std::int32_t& out_right) {
    const auto r = [&](int reg) { return static_cast<std::int32_t>(static_cast<std::int16_t>(preset_->regs[reg])); };
    const auto addr = [&](int reg) { return static_cast<std::int32_t>(preset_->regs[reg]) * 4; };  // 8 bytes = 4 samples
    const std::int32_t lin = mul(r(vLIN), sat(in_left)), rin = mul(r(vRIN), sat(in_right));
    const std::int32_t iir = r(vIIR), wall = r(vWALL);

    // Same-side and cross-side reflections ("-2" bytes: the previous sample).
    const auto reflect = [&](int m, std::int32_t in, int d) {
        const std::int32_t previous = at(addr(m) - 1);
        put(addr(m), mul(in + mul(at(addr(d)), wall) - previous, iir) + previous);
    };
    reflect(mLSAME, lin, dLSAME);
    reflect(mRSAME, rin, dRSAME);
    reflect(mLDIFF, lin, dRDIFF);
    reflect(mRDIFF, rin, dLDIFF);

    // Early echo (comb taps), then two all-pass stages.
    std::int32_t l = mul(r(vCOMB1), at(addr(mLCOMB1))) + mul(r(vCOMB2), at(addr(mLCOMB2))) +
                     mul(r(vCOMB3), at(addr(mLCOMB3))) + mul(r(vCOMB4), at(addr(mLCOMB4)));
    std::int32_t rr = mul(r(vCOMB1), at(addr(mRCOMB1))) + mul(r(vCOMB2), at(addr(mRCOMB2))) +
                      mul(r(vCOMB3), at(addr(mRCOMB3))) + mul(r(vCOMB4), at(addr(mRCOMB4)));
    const auto allpass = [&](std::int32_t x, int m, int d, int v) {
        const std::int32_t delayed = at(addr(m) - addr(d));
        x = sat(x - mul(r(v), delayed));
        put(addr(m), x);
        return mul(x, r(v)) + delayed;
    };
    l = allpass(l, mLAPF1, dAPF1, vAPF1);
    rr = allpass(rr, mRAPF1, dAPF1, vAPF1);
    l = allpass(l, mLAPF2, dAPF2, vAPF2);
    rr = allpass(rr, mRAPF2, dAPF2, vAPF2);
    out_left = sat(l);
    out_right = sat(rr);
    position_ = (position_ + 1) % buffer_.size();
}

void Reverb::process(const std::int32_t* send, std::int32_t* out, int frames, int volume_left, int volume_right) {
    if (!preset_) return;
    for (int i = 0; i < frames; ++i) {
        // 22050 Hz output, delayed by one 44100 Hz frame: the midpoint on even frames, the sample on odd ones.
        std::int32_t wet_left, wet_right;
        if (!odd_) {
            wet_left = (prev_out_left_ + out_left_) >> 1;
            wet_right = (prev_out_right_ + out_right_) >> 1;
            held_left_ = send[2 * i];
            held_right_ = send[2 * i + 1];
        } else {
            wet_left = out_left_;
            wet_right = out_right_;
            prev_out_left_ = out_left_;
            prev_out_right_ = out_right_;
            tick((held_left_ + send[2 * i]) >> 1, (held_right_ + send[2 * i + 1]) >> 1, out_left_, out_right_);
        }
        odd_ = !odd_;
        out[2 * i] += (wet_left * volume_left) >> 12;
        out[2 * i + 1] += (wet_right * volume_right) >> 12;
    }
}

}  // namespace ff7cc::rt::audio
