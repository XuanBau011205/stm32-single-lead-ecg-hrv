#include "parabolic_interp.hpp"

#include "../HIL/trusted_types.hpp"

namespace p05 {

namespace {

inline constexpr int32_t kHalfSampleQ15 = 16384;  // 0.5 * 32768
inline constexpr float kDegenerateEps = 1e-12f;

inline constexpr int16_t clamp_offset_q15(int64_t delta_q15) noexcept {
    if (delta_q15 > kHalfSampleQ15) {
        return static_cast<int16_t>(kHalfSampleQ15);
    }
    if (delta_q15 < -kHalfSampleQ15) {
        return static_cast<int16_t>(-kHalfSampleQ15);
    }
    return static_cast<int16_t>(delta_q15);
}

}  // namespace

ParabolicInterpResult parabolic_interp_q15(int32_t y_prev,
                                           int32_t y_peak,
                                           int32_t y_next) noexcept {
    // Vertex of parabola through (-1, y_prev), (0, y_peak), (+1, y_next):
    //   delta = (y_prev - y_next) / (2 * (y_prev - 2*y_peak + y_next))
    // Works for both local maximum (denom < 0) and local minimum (denom > 0).
    // Degenerate when denom == 0 (collinear / flat saturated top) or when y_peak
    // does not lie strictly outside/on one side of its neighbors.
    const int64_t second_diff =
        static_cast<int64_t>(y_prev) - (2 * static_cast<int64_t>(y_peak)) +
        static_cast<int64_t>(y_next);
    const int64_t denom = 2 * second_diff;

    if (denom == 0) {
        return ParabolicInterpResult{0, true};
    }

    // Verify local extremum: y_peak >= both neighbors (maximum) OR y_peak <= both neighbors (minimum)
    const bool is_max = (y_peak >= y_prev) && (y_peak >= y_next);
    const bool is_min = (y_peak <= y_prev) && (y_peak <= y_next);
    if (!is_max && !is_min) {
        return ParabolicInterpResult{0, true};
    }

    const int64_t num = static_cast<int64_t>(y_prev) - static_cast<int64_t>(y_next);
    // Scale numerator to Q15 with rounding to nearest integer
    const int64_t scaled_num = num * 32768;
    const int64_t abs_denom = (denom > 0) ? denom : -denom;
    const int64_t abs_num = (scaled_num >= 0) ? scaled_num : -scaled_num;
    const bool negative = (scaled_num < 0) != (denom < 0);

    const int64_t mag = (abs_num + (abs_denom / 2)) / abs_denom;
    const int64_t delta_q15 = negative ? -mag : mag;

    return ParabolicInterpResult{clamp_offset_q15(delta_q15), false};
}

ParabolicInterpResult parabolic_interp_float(float y_prev,
                                             float y_peak,
                                             float y_next) noexcept {
    const float denom = 2.0f * (y_prev - (2.0f * y_peak) + y_next);
    if ((denom > -kDegenerateEps) && (denom < kDegenerateEps)) {
        return ParabolicInterpResult{0, true};
    }

    const bool is_max = (y_peak >= y_prev) && (y_peak >= y_next);
    const bool is_min = (y_peak <= y_prev) && (y_peak <= y_next);
    if (!is_max && !is_min) {
        return ParabolicInterpResult{0, true};
    }

    const float delta = (y_prev - y_next) / denom;
    float clamped = delta;
    if (clamped > 0.5f) {
        clamped = 0.5f;
    } else if (clamped < -0.5f) {
        clamped = -0.5f;
    }

    const float scaled = clamped * 32768.0f;
    const int64_t q15 =
        static_cast<int64_t>((scaled >= 0.0f) ? (scaled + 0.5f) : (scaled - 0.5f));
    return ParabolicInterpResult{clamp_offset_q15(q15), false};
}

int32_t offset_q15_to_us(int16_t offset_q15, uint16_t fs_hz) noexcept {
    const int64_t ts_us = (fs_hz == kFs500Hz) ? 2000 : 4000;
    const int64_t prod = static_cast<int64_t>(offset_q15) * ts_us;
    if (prod >= 0) {
        return static_cast<int32_t>((prod + 16384) / 32768);
    }
    return static_cast<int32_t>(-(((-prod) + 16384) / 32768));
}

}  // namespace p05
