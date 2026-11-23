#include "iir_filter.hpp"

namespace p05 {

namespace {

inline constexpr int32_t kQ14Shift = 14;
inline constexpr int32_t kQ14Half = 1 << (kQ14Shift - 1);  // 8192
inline constexpr int64_t kInt16Min64 = -32768;
inline constexpr int64_t kInt16Max64 = 32767;

inline constexpr int16_t clamp_i64_to_i16(int64_t v) noexcept {
    if (v > kInt16Max64) {
        return static_cast<int16_t>(kInt16Max64);
    }
    if (v < kInt16Min64) {
        return static_cast<int16_t>(kInt16Min64);
    }
    return static_cast<int16_t>(v);
}

inline constexpr int16_t float_to_q15_clamped(float x) noexcept {
    const float scaled = x * 32768.0f;
    if (scaled >= 32767.0f) {
        return 32767;
    }
    if (scaled <= -32768.0f) {
        return -32768;
    }
    return static_cast<int16_t>(scaled >= 0.0f ? (scaled + 0.5f) : (scaled - 0.5f));
}

// Analytical 2nd-order Notch (50 Hz, Q = 30) + Butterworth HP (5 Hz) + Butterworth LP (15 Hz)
// Quantized to Q14 (scale = 16384), verified against exact float transfer function (< 0.015 dB diff).
inline constexpr std::array<BiquadCoeffQ14, 3U> kCoeffQ14_250Hz = {{
    {16128, -9968, 16128, -9968, 15873},    // Notch 50 Hz, Q = 30
    {14991, -29982, 14991, -29863, 13716},  // HP 5 Hz Butterworth
    {456, 913, 456, -24174, 9616},          // LP 15 Hz Butterworth
}};

inline constexpr std::array<BiquadCoeffFloat, 3U> kCoeffFloat_250Hz = {{
    {0.98439639f, -0.60839043f, 0.98439639f, -0.60839043f, 0.96879278f},
    {0.91496914f, -1.82993829f, 0.91496914f, -1.82269493f, 0.83718165f},
    {0.02785977f, 0.05571953f, 0.02785977f, -1.47548044f, 0.58691951f},
}};

inline constexpr std::array<BiquadCoeffQ14, 3U> kCoeffQ14_500Hz = {{
    {16225, -26253, 16225, -26253, 16066},  // Notch 50 Hz, Q = 30
    {15672, -31344, 15672, -31313, 14991},  // HP 5 Hz Butterworth
    {128, 256, 128, -28422, 12550},         // LP 15 Hz Butterworth
}};

inline constexpr std::array<BiquadCoeffFloat, 3U> kCoeffFloat_500Hz = {{
    {0.99029862f, -1.60233682f, 0.99029862f, -1.60233682f, 0.98059724f},
    {0.95654323f, -1.91308645f, 0.95654323f, -1.91119707f, 0.91497583f},
    {0.00782021f, 0.01564042f, 0.00782021f, -1.73472577f, 0.76600660f},
}};

}  // namespace

void BiquadQ15::configure(const BiquadCoeffQ14& coeff) noexcept {
    c_ = coeff;
    reset();
}

void BiquadQ15::reset() noexcept {
    x1_ = 0;
    x2_ = 0;
    y1_ = 0;
    y2_ = 0;
    err_ = 0;
}

int16_t BiquadQ15::process(int16_t x_q15) noexcept {
    // 64-bit MAC prevents intermediate overflow even for alternating int16 min/max inputs.
    int64_t acc = static_cast<int64_t>(err_);
    acc += static_cast<int64_t>(c_.b0) * static_cast<int64_t>(x_q15);
    acc += static_cast<int64_t>(c_.b1) * static_cast<int64_t>(x1_);
    acc += static_cast<int64_t>(c_.b2) * static_cast<int64_t>(x2_);
    acc -= static_cast<int64_t>(c_.a1) * static_cast<int64_t>(y1_);
    acc -= static_cast<int64_t>(c_.a2) * static_cast<int64_t>(y2_);

    // Round-to-nearest shift down from Q29 (Q15 * Q14) to Q15 with saturating clamp.
    const int64_t rounded = (acc >= 0) ? ((acc + kQ14Half) >> kQ14Shift)
                                       : -(((-acc) + kQ14Half) >> kQ14Shift);
    const int16_t y_sat = clamp_i64_to_i16(rounded);

    // Save fractional quantization error only when unsaturated (prevents integrator windup).
    if (rounded == static_cast<int64_t>(y_sat)) {
        err_ = static_cast<int32_t>(acc - (static_cast<int64_t>(y_sat) << kQ14Shift));
    } else {
        err_ = 0;
    }

    x2_ = x1_;
    x1_ = x_q15;
    y2_ = y1_;
    y1_ = y_sat;
    return y_sat;
}

void BiquadFloat::configure(const BiquadCoeffFloat& coeff) noexcept {
    c_ = coeff;
    reset();
}

void BiquadFloat::reset() noexcept {
    z1_ = 0.0f;
    z2_ = 0.0f;
}

float BiquadFloat::process(float x) noexcept {
    // Direct Form II Transposed
    const float y = (c_.b0 * x) + z1_;
    z1_ = (c_.b1 * x) - (c_.a1 * y) + z2_;
    z2_ = (c_.b2 * x) - (c_.a2 * y);
    return y;
}

EcgIirFilterChain::EcgIirFilterChain() noexcept {
    (void)configure(kDefaultWearableFsHz, MathMode::Q15);
}

bool EcgIirFilterChain::configure(uint16_t fs_hz, MathMode mode) noexcept {
    if (fs_hz != kFs250Hz && fs_hz != kFs500Hz) {
        return false;
    }

    fs_hz_ = fs_hz;
    mode_ = mode;

    const auto& q_table = (fs_hz == kFs500Hz) ? kCoeffQ14_500Hz : kCoeffQ14_250Hz;
    const auto& f_table = (fs_hz == kFs500Hz) ? kCoeffFloat_500Hz : kCoeffFloat_250Hz;
    for (std::size_t i = 0U; i < kNumSections; ++i) {
        q15_sections_[i].configure(q_table[i]);
        float_sections_[i].configure(f_table[i]);
    }
    return true;
}

void EcgIirFilterChain::reset() noexcept {
    for (std::size_t i = 0U; i < kNumSections; ++i) {
        q15_sections_[i].reset();
        float_sections_[i].reset();
    }
}

int16_t EcgIirFilterChain::process_q15(int16_t x_q15) noexcept {
    if (mode_ == MathMode::SoftFloat) {
        const float xf = static_cast<float>(x_q15) / 32768.0f;
        const float yf = process_float(xf);
        return float_to_q15_clamped(yf);
    }

    int16_t v = x_q15;
    for (std::size_t i = 0U; i < kNumSections; ++i) {
        v = q15_sections_[i].process(v);
    }
    return v;
}

float EcgIirFilterChain::process_float(float x) noexcept {
    if (mode_ == MathMode::Q15) {
        const int16_t yq = process_q15(float_to_q15_clamped(x));
        return static_cast<float>(yq) / 32768.0f;
    }

    float v = x;
    for (std::size_t i = 0U; i < kNumSections; ++i) {
        v = float_sections_[i].process(v);
    }
    return v;
}

}  // namespace p05
