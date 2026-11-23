#ifndef P05_DSP_HRV_ANALYZER_HPP
#define P05_DSP_HRV_ANALYZER_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "../HIL/trusted_types.hpp"
#include "pan_tompkins.hpp"

namespace p05 {

// Online RR-interval outlier filter and time-domain HRV analyzer (SPEC section 3.3, OPEN-10d).
// - Zero-Heap, chip-independent C++17 (no HAL, no dynamic allocation, no <cmath>)
// - Sliding window: 64 valid RR intervals
// - Physiological bounds: [300 ms, 2000 ms] (300,000 .. 2,000,000 us)
// - Statistical outlier filter: rejects RR deviating > 20% from median of last 5 valid beats
// - Computes 5 time-domain metrics: Mean RR, HR, SDNN, RMSSD, pNN50
// - Supports both pure integer (MathMode::Q15) and Soft-Float (MathMode::SoftFloat) for Ablation 1
// - Never bridges RMSSD/pNN50 differences across rejected outliers, Leads-Off, or SeqGap (R-HRV-01)
// - Never returns valid zero metrics when window has insufficient samples or fault occurs (R-HRV-02 / R2)
class HrvAnalyzer {
public:
    static constexpr std::size_t kWindowSize = 64U;        // OPEN-10d: 64 beats
    static constexpr std::size_t kMedianLookback = 5U;     // OPEN-10d: 5-beat median
    static constexpr uint32_t kMinValidRrUs = 300000U;     // 300 ms (200 BPM)
    static constexpr uint32_t kMaxValidRrUs = 2000000U;    // 2000 ms (30 BPM)
    static constexpr uint32_t kNn50ThresholdUs = 50000U;   // 50 ms for pNN50

    enum class RrDecision : uint8_t {
        Accepted = 0U,
        RejectedRange = 1U,
        RejectedOutlier = 2U,
        IgnoredNoRr = 3U,
    };

    HrvAnalyzer() noexcept = default;

    void configure(MathMode mode) noexcept;
    void reset() noexcept;

    // Notifies analyzer of sensor/stream interruption (LeadsOff, Stale, SeqGap, CommsFault).
    // Breaks beat-to-beat continuity so RMSSD/pNN50 never bridge across gaps (R-HRV-01).
    void on_interruption(FailureStatus fault) noexcept;

    // Feeds one RR interval (in microseconds) through physiological + 20% median-5 filter.
    RrDecision push_rr_us(uint32_t rr_us) noexcept;

    // Convenience adapter accepting a DetectedRPeak from Task Card 2 (PanTompkinsDetector).
    RrDecision push_r_peak(const DetectedRPeak& peak) noexcept;

    // Computes all 5 time-domain HRV metrics over the current sliding window.
    // Fails (empty optional, non-valid status) when < 2 valid RR intervals or < 1 contiguous
    // beat pair exist, or when last state is an active interruption (R-HRV-02 / R-BND-02).
    Result<TrustedHrvMetrics> compute_metrics() const noexcept;

    std::size_t valid_count() const noexcept { return count_; }
    std::size_t contiguous_pair_count() const noexcept;
    uint32_t rejected_outlier_count() const noexcept { return rejected_count_; }
    MathMode math_mode() const noexcept { return mode_; }

private:
    struct RrEntry {
        uint32_t rr_us = 0U;
        bool contiguous_with_prev = false;
    };

    uint32_t median_last_5() const noexcept;
    TrustedHrvMetrics compute_integer_metrics() const noexcept;
    TrustedHrvMetrics compute_float_metrics() const noexcept;

    MathMode mode_ = MathMode::Q15;
    std::array<RrEntry, kWindowSize> ring_{};
    std::size_t head_ = 0U;   // next write slot
    std::size_t count_ = 0U;  // valid entries in ring_ (0..64)
    std::size_t beats_since_interruption_ = 0U;  // resets after interruption so 5-beat median re-seeds
    bool prev_beat_contiguous_ = false;
    bool fault_active_ = false;
    FailureStatus last_fault_{CanonicalState::Valid, DetailCode::Ok};
    uint32_t rejected_count_ = 0U;
};

}  // namespace p05

#endif  // P05_DSP_HRV_ANALYZER_HPP
