#include "hrv_analyzer.hpp"

namespace p05 {

namespace {

inline constexpr uint64_t kMicrosPerMinuteX100 = 6000000000ULL;  // 60e6 us/min * 100 (0.01 BPM)

// Exact 64-bit integer square root with round-to-nearest integer (zero float, zero <cmath>).
uint32_t isqrt_u64_rounded(uint64_t x) noexcept {
    if (x == 0ULL) {
        return 0U;
    }
    uint64_t op = x;
    uint64_t res = 0ULL;
    uint64_t one = 1ULL << 62U;
    while (one > op) {
        one >>= 2U;
    }
    while (one != 0ULL) {
        if (op >= res + one) {
            op -= (res + one);
            res = (res >> 1U) + one;
        } else {
            res >>= 1U;
        }
        one >>= 2U;
    }
    // Round to nearest: if remainder > res, round up
    if (op > res) {
        ++res;
    }
    return static_cast<uint32_t>(res);
}

// Portable single-precision Newton-Raphson square root (zero <cmath> dependency on MCU).
float sqrt_soft_float(float x) noexcept {
    if (x <= 0.0f) {
        return 0.0f;
    }
    // Initial guess via exponent halving (portable without bit-punning UB)
    float g = x;
    int scale_steps = 0;
    while (g >= 4.0f && scale_steps < 64) {
        g *= 0.25f;
        ++scale_steps;
    }
    while (g < 0.25f && scale_steps > -64) {
        g *= 4.0f;
        --scale_steps;
    }
    // Start close to sqrt(g) in [0.5, 2.0]
    float y = 0.5f * (g + 1.0f);
    for (int i = 0; i < 6; ++i) {
        y = 0.5f * (y + (g / y));
    }
    while (scale_steps > 0) {
        y *= 2.0f;
        --scale_steps;
    }
    while (scale_steps < 0) {
        y *= 0.5f;
        ++scale_steps;
    }
    return y;
}

inline uint32_t abs_diff_u32(uint32_t a, uint32_t b) noexcept {
    return (a >= b) ? (a - b) : (b - a);
}

inline uint64_t div_round_u64(uint64_t num, uint64_t den) noexcept {
    return (num + (den / 2ULL)) / den;
}

inline uint32_t round_float_to_u32(float v) noexcept {
    if (v <= 0.0f) {
        return 0U;
    }
    return static_cast<uint32_t>(v + 0.5f);
}

}  // namespace

void HrvAnalyzer::configure(MathMode mode) noexcept {
    mode_ = mode;
    reset();
}

void HrvAnalyzer::reset() noexcept {
    ring_.fill(RrEntry{});
    head_ = 0U;
    count_ = 0U;
    beats_since_interruption_ = 0U;
    prev_beat_contiguous_ = false;
    fault_active_ = false;
    last_fault_ = FailureStatus{CanonicalState::Valid, DetailCode::Ok};
    rejected_count_ = 0U;
}

void HrvAnalyzer::on_interruption(FailureStatus fault) noexcept {
    // R-HRV-01: break beat continuity across Leads-Off, Stale, SeqGap, or CommsFault
    prev_beat_contiguous_ = false;
    // Allow first 5 post-interruption beats in [300 ms, 2000 ms] to re-seed the 5-beat median
    // baseline so a large true HR shift during the gap never deadlocks the 20% filter.
    beats_since_interruption_ = 0U;
    if (fault.canonical != CanonicalState::Valid) {
        fault_active_ = true;
        last_fault_ = fault;
    }
}

uint32_t HrvAnalyzer::median_last_5() const noexcept {
    std::array<uint32_t, kMedianLookback> buf{};
    for (std::size_t i = 0U; i < kMedianLookback; ++i) {
        const std::size_t idx = (head_ + kWindowSize - 1U - i) % kWindowSize;
        buf[i] = ring_[idx].rr_us;
    }
    // Insertion sort on 5 elements (deterministic, <= 10 comparisons, zero heap)
    for (std::size_t i = 1U; i < kMedianLookback; ++i) {
        const uint32_t key = buf[i];
        std::size_t j = i;
        while (j > 0U && buf[j - 1U] > key) {
            buf[j] = buf[j - 1U];
            --j;
        }
        buf[j] = key;
    }
    return buf[2];
}

HrvAnalyzer::RrDecision HrvAnalyzer::push_rr_us(uint32_t rr_us) noexcept {
    // 1. Physiological range check: [300 ms, 2000 ms] (OPEN-10d)
    if (rr_us < kMinValidRrUs || rr_us > kMaxValidRrUs) {
        prev_beat_contiguous_ = false;
        ++rejected_count_;
        return RrDecision::RejectedRange;
    }

    // 2. Statistical 20% deviation check against median of last 5 valid beats (OPEN-10d)
    if (count_ >= kMedianLookback && beats_since_interruption_ >= kMedianLookback) {
        const uint32_t med5 = median_last_5();
        const uint64_t diff = static_cast<uint64_t>(abs_diff_u32(rr_us, med5));
        // |rr - med5| > 0.20 * med5  <=>  5 * |rr - med5| > med5 (exact integer test)
        if ((5ULL * diff) > static_cast<uint64_t>(med5)) {
            prev_beat_contiguous_ = false;
            ++rejected_count_;
            return RrDecision::RejectedOutlier;
        }
    }

    // 3. Accept valid beat into 64-beat ring buffer
    ring_[head_] = RrEntry{rr_us, prev_beat_contiguous_};
    head_ = (head_ + 1U) % kWindowSize;
    if (count_ < kWindowSize) {
        ++count_;
    }
    if (beats_since_interruption_ < kWindowSize) {
        ++beats_since_interruption_;
    }
    prev_beat_contiguous_ = true;
    fault_active_ = false;
    last_fault_ = FailureStatus{CanonicalState::Valid, DetailCode::Ok};
    return RrDecision::Accepted;
}

HrvAnalyzer::RrDecision HrvAnalyzer::push_r_peak(const DetectedRPeak& peak) noexcept {
    if (!peak.has_rr) {
        prev_beat_contiguous_ = false;
        return RrDecision::IgnoredNoRr;
    }
    return push_rr_us(peak.rr_interval_us);
}

std::size_t HrvAnalyzer::contiguous_pair_count() const noexcept {
    if (count_ < 2U) {
        return 0U;
    }
    const std::size_t start = (count_ == kWindowSize) ? head_ : 0U;
    std::size_t pairs = 0U;
    for (std::size_t i = 1U; i < count_; ++i) {
        const std::size_t idx = (start + i) % kWindowSize;
        if (ring_[idx].contiguous_with_prev) {
            ++pairs;
        }
    }
    return pairs;
}

TrustedHrvMetrics HrvAnalyzer::compute_integer_metrics() const noexcept {
    const std::size_t start = (count_ == kWindowSize) ? head_ : 0U;

    uint64_t sum_rr = 0ULL;
    for (std::size_t i = 0U; i < count_; ++i) {
        const std::size_t idx = (start + i) % kWindowSize;
        sum_rr += static_cast<uint64_t>(ring_[idx].rr_us);
    }

    const uint64_t mean_rr_u64 = div_round_u64(sum_rr, static_cast<uint64_t>(count_));
    const uint32_t mean_rr_us = static_cast<uint32_t>(mean_rr_u64);

    // HR (x100 BPM) = 60,000,000 * 100 / mean_rr_us
    const uint64_t hr_x100_u64 = div_round_u64(kMicrosPerMinuteX100, mean_rr_u64);
    const uint16_t hr_bpm_x100 =
        (hr_x100_u64 > 65535ULL) ? 65535U : static_cast<uint16_t>(hr_x100_u64);

    // SDNN = sqrt(sum((RR_i - mean)^2) / (N - 1))
    uint64_t sum_var = 0ULL;
    for (std::size_t i = 0U; i < count_; ++i) {
        const std::size_t idx = (start + i) % kWindowSize;
        const uint64_t diff = static_cast<uint64_t>(abs_diff_u32(ring_[idx].rr_us, mean_rr_us));
        sum_var += diff * diff;
    }
    const uint64_t var_sdnn = div_round_u64(sum_var, static_cast<uint64_t>(count_ - 1U));
    const uint32_t sdnn_us = isqrt_u64_rounded(var_sdnn);

    // RMSSD & pNN50 over strictly contiguous beat pairs only (R-HRV-01)
    uint64_t sum_sq_diff = 0ULL;
    uint32_t nn50_count = 0U;
    std::size_t pair_count = 0U;

    for (std::size_t i = 1U; i < count_; ++i) {
        const std::size_t curr_idx = (start + i) % kWindowSize;
        if (!ring_[curr_idx].contiguous_with_prev) {
            continue;
        }
        const std::size_t prev_idx = (start + i - 1U) % kWindowSize;
        const uint32_t diff = abs_diff_u32(ring_[curr_idx].rr_us, ring_[prev_idx].rr_us);
        const uint64_t diff64 = static_cast<uint64_t>(diff);
        sum_sq_diff += diff64 * diff64;
        if (diff > kNn50ThresholdUs) {
            ++nn50_count;
        }
        ++pair_count;
    }

    const uint64_t mean_sq_diff = div_round_u64(sum_sq_diff, static_cast<uint64_t>(pair_count));
    const uint32_t rmssd_us = isqrt_u64_rounded(mean_sq_diff);
    const uint16_t pnn50_permille = static_cast<uint16_t>(
        div_round_u64(static_cast<uint64_t>(nn50_count) * 1000ULL,
                      static_cast<uint64_t>(pair_count)));

    TrustedHrvMetrics m{};
    m.mean_rr_us = mean_rr_us;
    m.hr_bpm_x100 = hr_bpm_x100;
    m.sdnn_us = sdnn_us;
    m.rmssd_us = rmssd_us;
    m.pnn50_permille = pnn50_permille;
    return m;
}

TrustedHrvMetrics HrvAnalyzer::compute_float_metrics() const noexcept {
    const std::size_t start = (count_ == kWindowSize) ? head_ : 0U;
    const float n_f = static_cast<float>(count_);

    float sum_rr_ms = 0.0f;
    for (std::size_t i = 0U; i < count_; ++i) {
        const std::size_t idx = (start + i) % kWindowSize;
        sum_rr_ms += static_cast<float>(ring_[idx].rr_us) * 0.001f;
    }
    const float mean_rr_ms = sum_rr_ms / n_f;
    const float hr_bpm_x100_f = (60000.0f / mean_rr_ms) * 100.0f;

    float sum_var_ms2 = 0.0f;
    for (std::size_t i = 0U; i < count_; ++i) {
        const std::size_t idx = (start + i) % kWindowSize;
        const float d_ms = (static_cast<float>(ring_[idx].rr_us) * 0.001f) - mean_rr_ms;
        sum_var_ms2 += d_ms * d_ms;
    }
    const float sdnn_ms = sqrt_soft_float(sum_var_ms2 / static_cast<float>(count_ - 1U));

    float sum_sq_diff_ms2 = 0.0f;
    uint32_t nn50_count = 0U;
    std::size_t pair_count = 0U;

    for (std::size_t i = 1U; i < count_; ++i) {
        const std::size_t curr_idx = (start + i) % kWindowSize;
        if (!ring_[curr_idx].contiguous_with_prev) {
            continue;
        }
        const std::size_t prev_idx = (start + i - 1U) % kWindowSize;
        const uint32_t diff_u32 = abs_diff_u32(ring_[curr_idx].rr_us, ring_[prev_idx].rr_us);
        const float diff_ms = static_cast<float>(diff_u32) * 0.001f;
        sum_sq_diff_ms2 += diff_ms * diff_ms;
        if (diff_u32 > kNn50ThresholdUs) {
            ++nn50_count;
        }
        ++pair_count;
    }

    const float rmssd_ms = sqrt_soft_float(sum_sq_diff_ms2 / static_cast<float>(pair_count));
    const float pnn50_permille_f =
        (static_cast<float>(nn50_count) * 1000.0f) / static_cast<float>(pair_count);

    TrustedHrvMetrics m{};
    m.mean_rr_us = round_float_to_u32(mean_rr_ms * 1000.0f);
    const uint32_t hr_u32 = round_float_to_u32(hr_bpm_x100_f);
    m.hr_bpm_x100 = (hr_u32 > 65535U) ? 65535U : static_cast<uint16_t>(hr_u32);
    m.sdnn_us = round_float_to_u32(sdnn_ms * 1000.0f);
    m.rmssd_us = round_float_to_u32(rmssd_ms * 1000.0f);
    m.pnn50_permille = static_cast<uint16_t>(round_float_to_u32(pnn50_permille_f));
    return m;
}

Result<TrustedHrvMetrics> HrvAnalyzer::compute_metrics() const noexcept {
    // R-HRV-02 / R-BND-02: if active fault or insufficient valid samples/pairs, NEVER return valid 0s
    if (fault_active_) {
        return Result<TrustedHrvMetrics>::fail(last_fault_);
    }
    if (count_ < 2U || contiguous_pair_count() < 1U) {
        return Result<TrustedHrvMetrics>::fail(CanonicalState::SensorUnavailable,
                                               DetailCode::LeadsOff);
    }

    const TrustedHrvMetrics metrics =
        (mode_ == MathMode::SoftFloat) ? compute_float_metrics() : compute_integer_metrics();
    return Result<TrustedHrvMetrics>::ok(metrics);
}

}  // namespace p05
