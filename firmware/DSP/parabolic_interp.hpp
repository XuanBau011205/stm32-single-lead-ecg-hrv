#ifndef P05_DSP_PARABOLIC_INTERP_HPP
#define P05_DSP_PARABOLIC_INTERP_HPP

#include <cstdint>

namespace p05 {

// Result of 3-point parabolic interpolation around local extremum y[k-1], y[k], y[k+1].
// sub_sample_offset_q15 represents fractional sample shift delta in [-0.5, +0.5]
// scaled by 32768 (so [-16384, +16384]).
// Per PROPOSED-DSP-01 / OPEN-10c: if denominator == 0 (collinear/flat peak) or non-extremum,
// no division by zero is performed; offset is set to 0 and degenerate == true.
struct ParabolicInterpResult {
    int16_t sub_sample_offset_q15 = 0;
    bool degenerate = false;
};

// Fixed-point integer 3-point parabolic interpolation (Q15 output, zero float ops).
ParabolicInterpResult parabolic_interp_q15(int32_t y_prev,
                                           int32_t y_peak,
                                           int32_t y_next) noexcept;

// Soft-Float 3-point parabolic interpolation (Ablation 1 baseline, returns Q15 offset).
ParabolicInterpResult parabolic_interp_float(float y_prev,
                                             float y_peak,
                                             float y_next) noexcept;

// Converts a Q15 sample offset ([-16384, +16384]) to signed microseconds at fs_hz (250 or 500 Hz).
int32_t offset_q15_to_us(int16_t offset_q15, uint16_t fs_hz) noexcept;

}  // namespace p05

#endif  // P05_DSP_PARABOLIC_INTERP_HPP
