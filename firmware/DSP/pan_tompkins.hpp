#ifndef P05_DSP_PAN_TOMPKINS_HPP
#define P05_DSP_PAN_TOMPKINS_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "../HIL/trusted_types.hpp"
#include "iir_filter.hpp"
#include "parabolic_interp.hpp"

namespace p05 {

inline constexpr std::size_t kMaxPeaksPerBatch = 16U;

struct DspConfig {
    uint16_t fs_hz = kDefaultWearableFsHz;  // 250 or 500 Hz (Ablation 2)
    MathMode math_mode = MathMode::Q15;     // Q15 or SoftFloat (Ablation 1)
    bool interp_enable = true;              // 3-point parabolic interpolation on/off (Ablation 2)
};

struct DetectedRPeak {
    uint32_t peak_sample_idx = 0U;
    int16_t sub_sample_offset_q15 = 0;
    uint32_t rr_interval_us = 0U;
    bool has_rr = false;           // false on first beat or first beat after gap/leads-off (R2)
    bool used_search_back = false;
    bool degenerate_peak = false;  // true if 3-point parabolic denominator was 0 (PROPOSED-DSP-01)
};

struct DspSampleOutput {
    int16_t filtered_q15 = 0;
    bool r_peak_detected = false;
    DetectedRPeak r_peak{};
};

struct DspBatchOutput {
    uint16_t sample_count = 0U;
    std::array<int16_t, kSdMaxRawSamples> filtered_samples{};
    uint8_t r_peak_count = 0U;
    std::array<DetectedRPeak, kMaxPeaksPerBatch> peaks{};
};

// Zero-Heap, chip-independent Pan-Tompkins (1985) R-peak detector with:
// - 5-point derivative -> squaring -> 150 ms Moving-Window Integration (38 @250Hz, 75 @500Hz)
// - Dual adaptive thresholds (SPKI/NPKI, THR1, THR2) + 166% 8-beat average RR search-back
// - 200 ms refractory period (50 @250Hz, 100 @500Hz)
// - 3-point parabolic sub-sample interpolation (< 0.5 ms localization at 250 Hz)
// - Supports both Q15 integer path and Soft-Float path (OPEN-10b, OPEN-10c, OPEN-15)
class PanTompkinsDetector {
public:
    PanTompkinsDetector() noexcept;

    bool configure(const DspConfig& cfg) noexcept;
    void reset() noexcept;

    // Resets detector and blocks R-peak emission for 2 seconds while re-learning thresholds
    // (OPEN-15 gap policy).
    void reset_with_gap_blanking() noexcept;

    // Breaks RR continuity without wiping learned thresholds (e.g., short sensor interruption).
    void break_rr_continuity() noexcept;

    // Feeds one filtered sample (Q15 or float depending on configured math_mode) at sample_idx.
    DspSampleOutput process_filtered(int16_t filtered_q15,
                                     float filtered_float,
                                     uint32_t sample_idx) noexcept;

    bool in_gap_blanking() const noexcept { return blanking_remaining_ > 0U; }
    uint32_t blanking_samples_remaining() const noexcept { return blanking_remaining_; }
    const DspConfig& config() const noexcept { return cfg_; }

private:
    static constexpr std::size_t kMaxMwiLen = 75U;      // 150 ms @ 500 Hz
    static constexpr std::size_t kBpHistSize = 128U;    // power-of-2 ring for fine peak search
    static constexpr std::size_t kBpHistMask = kBpHistSize - 1U;
    static constexpr std::size_t kRrHistLen = 8U;       // 8-beat rolling average (OPEN-10b)

    struct CandidatePeak {
        uint32_t mwi_idx = 0U;
        uint32_t refined_idx = 0U;
        int16_t offset_q15 = 0;
        bool degenerate = false;
        int8_t polarity = 1;  // +1 positive peak, -1 negative peak
        uint32_t val_q = 0U;
        float val_f = 0.0f;
    };

    void update_thresholds_q(uint32_t peaki, bool is_signal, bool is_searchback) noexcept;
    void update_thresholds_f(float peaki, bool is_signal, bool is_searchback) noexcept;
    CandidatePeak refine_peak(uint32_t cand_mwi_idx, uint32_t val_q, float val_f) const noexcept;
    bool commit_peak(const CandidatePeak& cand,
                     bool used_search_back,
                     DetectedRPeak& out_peak) noexcept;
    uint32_t average_rr_samples() const noexcept;

    DspConfig cfg_{};
    std::size_t mwi_len_ = 38U;
    uint32_t refractory_samples_ = 50U;
    uint32_t settle_samples_ = 62U;  // 250 ms IIR step-response settle after reset
    uint32_t init_floor_q_ = 8000U;
    float init_floor_f_ = 7.5e-6f;
    uint32_t blanking_remaining_ = 0U;
    uint32_t samples_seen_ = 0U;
    int8_t locked_polarity_ = 0;  // 0 = unlocked, +1 = positive R-peak, -1 = negative R-peak

    // 5-point derivative delay lines (newest at index 0)
    std::array<int16_t, 5U> deriv_q_{};
    std::array<float, 5U> deriv_f_{};

    // Moving-Window Integration ring buffers
    std::array<uint32_t, kMaxMwiLen> mwi_buf_q_{};
    std::array<float, kMaxMwiLen> mwi_buf_f_{};
    std::size_t mwi_pos_ = 0U;
    uint64_t mwi_sum_q_ = 0U;
    float mwi_sum_f_ = 0.0f;

    // 3-point MWI peak tracker
    uint32_t mwi_prev2_q_ = 0U;
    uint32_t mwi_prev1_q_ = 0U;
    float mwi_prev2_f_ = 0.0f;
    float mwi_prev1_f_ = 0.0f;

    // Bandpass history ring for fine QRS extremum + 3-point parabolic interpolation
    std::array<int16_t, kBpHistSize> bp_hist_q_{};
    std::array<float, kBpHistSize> bp_hist_f_{};
    std::array<uint32_t, kBpHistSize> bp_idx_hist_{};
    std::size_t bp_head_ = 0U;
    std::size_t bp_count_ = 0U;

    // Adaptive thresholds (Q path and Soft-Float path)
    bool thresh_init_ = false;
    uint32_t spki_q_ = 0U;
    uint32_t npki_q_ = 0U;
    uint32_t thr1_q_ = 0U;
    uint32_t thr2_q_ = 0U;

    float spki_f_ = 0.0f;
    float npki_f_ = 0.0f;
    float thr1_f_ = 0.0f;
    float thr2_f_ = 0.0f;

    // Last accepted peak & RR history for search-back (166% of 8-beat average)
    bool has_last_peak_ = false;
    uint32_t last_mwi_peak_idx_ = 0U;
    uint32_t last_refined_peak_idx_ = 0U;
    int16_t last_offset_q15_ = 0;

    std::array<uint32_t, kRrHistLen> rr_samples_hist_{};
    std::size_t rr_hist_pos_ = 0U;
    std::size_t rr_hist_count_ = 0U;

    // Best candidate between THR2 and THR1 awaiting potential search-back
    bool has_sb_cand_ = false;
    CandidatePeak sb_cand_{};
};

// Complete Portable Core DSP pipeline combining IIR cascade + Pan-Tompkins + Parabolic Interp.
// Only accepts Trusted inputs from Boundary Adapter (Task Card 1) and propagates failures
// without ever generating fake 0 samples or RR = 0 (R-BND-02, R-DSP-01..04, OPEN-15).
class EcgDspCore {
public:
    EcgDspCore() noexcept = default;

    bool configure(const DspConfig& cfg) noexcept;
    void reset() noexcept;
    void reset_with_gap_blanking() noexcept;

    // Processes a single trusted 12-bit ADC sample [0..4095] at sample_idx.
    DspSampleOutput process_trusted_adc_sample(int16_t raw_adc, uint32_t sample_idx) noexcept;

    // Processes a HIL TrustedSampleBatch Result; propagates error states and enforces OPEN-15.
    Result<DspBatchOutput> process_hil_batch(const Result<TrustedSampleBatch>& batch_res) noexcept;

    // Processes a Wearable TrustedWearableBatch Result; propagates errors without fake zeros.
    Result<DspBatchOutput> process_wearable_batch(
        const Result<TrustedWearableBatch>& batch_res, uint32_t first_sample_idx) noexcept;

    const DspConfig& config() const noexcept { return detector_.config(); }
    bool in_gap_blanking() const noexcept { return detector_.in_gap_blanking(); }

private:
    EcgIirFilterChain filter_{};
    PanTompkinsDetector detector_{};
};

}  // namespace p05

#endif  // P05_DSP_PAN_TOMPKINS_HPP
