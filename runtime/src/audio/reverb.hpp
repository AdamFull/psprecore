#pragma once

// SAS reverb: the PlayStation SPU reverb (the PSP's sceSasCore uses the same design and preset types), written from
// the public hardware description (psx-spx, "SPU Reverb Formula" and "SPU Reverb Examples").
//
// The reverb runs at 22050 Hz on a ring buffer of 16-bit samples: same-side and cross-side reflections through a
// one-pole filter (vIIR) with wall absorption (vWALL), four comb taps, and two all-pass stages. The preset registers
// give buffer offsets in units of 8 bytes (4 samples). Input is the voices' effect sends, averaged in pairs down to
// 22050 Hz; output is interpolated back to 44100 Hz. (The hardware resamples with a 39-tap filter instead.)

#include <array>
#include <cstdint>
#include <vector>

namespace ff7cc::rt::audio {

class Reverb {
public:
    // sceSasRevType: 0 off, 1 room, 2 studio small, 3 studio medium, 4 studio large, 5 hall, 6 space echo,
    // 7 echo, 8 delay, 9 pipe. Changing the type clears the buffer.
    void set_type(int type);
    int type() const { return type_; }

    // Adds the wet signal for `frames` stereo frames of effect send (`send`, 16-bit scale) to `out`, scaled by the
    // return volumes (0x1000 = unity).
    void process(const std::int32_t* send, std::int32_t* out, int frames, int volume_left, int volume_right);

private:
    struct Preset {
        std::uint16_t regs[32];
        std::uint32_t bytes;  // work area size
    };
    static const Preset& preset(int type);

    std::int32_t at(std::int32_t offset) const;  // buffer sample relative to the current position
    void put(std::int32_t offset, std::int32_t value);
    void tick(std::int32_t in_left, std::int32_t in_right, std::int32_t& out_left, std::int32_t& out_right);

    int type_ = 0;
    const Preset* preset_ = nullptr;
    std::vector<std::int16_t> buffer_;
    std::size_t position_ = 0;
    // 44100 <-> 22050 Hz
    bool odd_ = false;
    std::int32_t held_left_ = 0, held_right_ = 0;           // first input frame of the pair
    std::int32_t out_left_ = 0, out_right_ = 0;             // latest 22050 Hz output
    std::int32_t prev_out_left_ = 0, prev_out_right_ = 0;   // the one before
};

}  // namespace ff7cc::rt::audio
