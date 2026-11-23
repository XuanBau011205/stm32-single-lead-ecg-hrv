#ifndef P05_DSP_IIR_FILTER_HPP
#define P05_DSP_IIR_FILTER_HPP

#include <array>
#include <cstdint>

#include "../HIL/trusted_types.hpp"

namespace p05 {

// Second-order IIR Biquad coefficients in Q14 fixed-point (scale = 2^14 = 16384).
// Q14 spans [-2.0, +1.99994], accommodating |a1| up to ~1.91 at 500 Hz (OPEN-10a).
struct BiquadCoeffQ14 {
    int16_t b0 = 16384;
    int16_t b1 = 0;
    int16_t b2 = 0;
    int16_t a1 = 0;
    int16_t a2 = 0;
};

struct BiquadCoeffFloat {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
};

// Single Q15-signal / Q14-coefficient biquad section.
// Uses Direct Form I with 64-bit accumulator (native SMLAL on Cortex-M3),
// first-order quantization error feedback (prevents deadbands near fs=500 Hz poles),
// and saturating output clamp to int16_t [-32768, 32767] (R-DSP-02).
class BiquadQ15 {
public:
    void configure(const BiquadCoeffQ14& coeff) noexcept;
    void reset() noexcept;
    int16_t process(int16_t x_q15) noexcept;

private:
    BiquadCoeffQ14 c_{};
    int16_t x1_ = 0;
    int16_t x2_ = 0;
    int16_t y1_ = 0;
    int16_t y2_ = 0;
    int32_t err_ = 0;  // 14-bit fractional remainder for error feedback
};

// Single Soft-Float biquad section using Direct Form II Transposed (Ablation 1 baseline).
class BiquadFloat {
public:
    void configure(const BiquadCoeffFloat& coeff) noexcept;
    void reset() noexcept;
    float process(float x) noexcept;

private:
    BiquadCoeffFloat c_{};
    float z1_ = 0.0f;
    float z2_ = 0.0f;
};

// 3-stage cascaded ECG preprocessor (SPEC section 3.1, OPEN-10a):
// Stage 0: Notch 50 Hz (2nd-order IIR, Q = 30)
// Stage 1: High-Pass 5 Hz (2nd-order Butterworth)
// Stage 2: Low-Pass 15 Hz (2nd-order Butterworth)
// Supports both 250 Hz and 500 Hz, and both Q15 and Soft-Float modes (Zero-Heap, no HAL).
class EcgIirFilterChain {
public:
    EcgIirFilterChain() noexcept;

    // Configures sampling rate (250 or 500 Hz) and math mode (Q15 or SoftFloat), then resets state.
    // Returns false if fs_hz is not 250 or 500.
    bool configure(uint16_t fs_hz, MathMode mode) noexcept;

    void reset() noexcept;

    // Processes one centered Q15 sample (int16_t in [-32768, 32767]) and returns filtered Q15.
    // In SoftFloat mode, converts Q15 -> float, runs float cascade, and clamps back to Q15.
    int16_t process_q15(int16_t x_q15) noexcept;

    // Processes one floating-point sample (valid in SoftFloat mode; also runs Q15 if configured).
    float process_float(float x) noexcept;

    // Converts raw 12-bit ADC sample [0..4095] (midpoint 2048) to centered Q15 with 6 dB headroom
    // ([-16384, +16376] = [-0.5, +0.5] Q15) so filter overshoot does not clip normal ECG.
    static constexpr int16_t adc_raw_to_q15(int16_t raw_adc) noexcept {
        return static_cast<int16_t>((static_cast<int32_t>(raw_adc) - kAdcMidpoint) * 8);
    }

    static constexpr float adc_raw_to_float(int16_t raw_adc) noexcept {
        return static_cast<float>(static_cast<int32_t>(raw_adc) - kAdcMidpoint) / 4096.0f;
    }

    uint16_t fs_hz() const noexcept { return fs_hz_; }
    MathMode math_mode() const noexcept { return mode_; }

private:
    static constexpr std::size_t kNumSections = 3U;

    uint16_t fs_hz_ = kDefaultWearableFsHz;
    MathMode mode_ = MathMode::Q15;
    std::array<BiquadQ15, kNumSections> q15_sections_{};
    std::array<BiquadFloat, kNumSections> float_sections_{};
};

}  // namespace p05

#endif  // P05_DSP_IIR_FILTER_HPP
