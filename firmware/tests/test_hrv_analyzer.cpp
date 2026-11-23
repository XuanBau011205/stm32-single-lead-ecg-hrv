#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "../DSP/hrv_analyzer.hpp"
#include "../HIL/trusted_types.hpp"

namespace {

int g_tests_run = 0;
int g_tests_failed = 0;

#define EXPECT_TRUE(cond)                                                                      \
    do {                                                                                       \
        ++g_tests_run;                                                                         \
        if (!(cond)) {                                                                         \
            ++g_tests_failed;                                                                  \
            std::printf("FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond);                      \
        }                                                                                      \
    } while (false)

#define EXPECT_EQ(a, b)                                                                        \
    do {                                                                                       \
        ++g_tests_run;                                                                         \
        if (!((a) == (b))) {                                                                   \
            ++g_tests_failed;                                                                  \
            std::printf("FAIL [%s:%d]: %s != %s\n", __FILE__, __LINE__, #a, #b);               \
        }                                                                                      \
    } while (false)

uint32_t abs_diff(uint32_t a, uint32_t b) {
    return (a >= b) ? (a - b) : (b - a);
}

void test_01_insufficient_samples_never_return_valid_zero() {
    p05::HrvAnalyzer hrv;
    hrv.configure(p05::MathMode::Q15);

    // 0 beats -> error, empty optional (R-HRV-02 / R2)
    const auto r0 = hrv.compute_metrics();
    EXPECT_TRUE(r0.is_err());
    EXPECT_TRUE(!r0.optional_value().has_value());

    // 1 beat -> still insufficient for SDNN/RMSSD/pNN50
    EXPECT_EQ(hrv.push_rr_us(800000U), p05::HrvAnalyzer::RrDecision::Accepted);
    EXPECT_EQ(hrv.valid_count(), 1U);
    const auto r1 = hrv.compute_metrics();
    EXPECT_TRUE(r1.is_err());
    EXPECT_TRUE(!r1.optional_value().has_value());

    // Break continuity via LeadsOff, then add 2nd beat -> 2 valid beats but 0 contiguous pairs!
    hrv.on_interruption(
        p05::FailureStatus{p05::CanonicalState::SensorUnavailable, p05::DetailCode::LeadsOff});
    const auto r_fault = hrv.compute_metrics();
    EXPECT_TRUE(r_fault.is_err());
    EXPECT_EQ(r_fault.canonical_state(), p05::CanonicalState::SensorUnavailable);

    EXPECT_EQ(hrv.push_rr_us(820000U), p05::HrvAnalyzer::RrDecision::Accepted);
    EXPECT_EQ(hrv.valid_count(), 2U);
    EXPECT_EQ(hrv.contiguous_pair_count(), 0U);
    EXPECT_TRUE(hrv.compute_metrics().is_err());

    // 3rd beat creates 1 genuine contiguous pair (820 ms -> 840 ms) -> valid metrics!
    EXPECT_EQ(hrv.push_rr_us(840000U), p05::HrvAnalyzer::RrDecision::Accepted);
    EXPECT_EQ(hrv.contiguous_pair_count(), 1U);
    const auto r3 = hrv.compute_metrics();
    EXPECT_TRUE(r3.is_ok());
    EXPECT_EQ(r3.value().mean_rr_us, 820000U);
    EXPECT_EQ(r3.value().rmssd_us, 20000U);
    EXPECT_EQ(r3.value().pnn50_permille, 0U);
}

void test_02_exact_analytical_5_hrv_metrics() {
    // Sequence: [800, 860, 800, 860, 800, 860] ms (6 beats, 5 contiguous diffs all = 60 ms)
    // Mean RR = 830,000 us
    // HR = 60e6 * 100 / 830000 = 7228.915 -> 7229 (72.29 BPM)
    // SDNN = sqrt((6 * 30000^2) / 5) = sqrt(1.08e9) = 32863.35 -> 32863 us
    // RMSSD = sqrt((5 * 60000^2) / 5) = 60000 us
    // pNN50 = 5/5 = 1000 permille (100.0%)
    const std::array<uint32_t, 6U> seq = {
        800000U, 860000U, 800000U, 860000U, 800000U, 860000U};

    for (const p05::MathMode mode : {p05::MathMode::Q15, p05::MathMode::SoftFloat}) {
        p05::HrvAnalyzer hrv;
        hrv.configure(mode);
        for (const uint32_t rr : seq) {
            EXPECT_EQ(hrv.push_rr_us(rr), p05::HrvAnalyzer::RrDecision::Accepted);
        }
        const auto res = hrv.compute_metrics();
        EXPECT_TRUE(res.is_ok());
        EXPECT_TRUE(abs_diff(res.value().mean_rr_us, 830000U) <= 1U);
        EXPECT_TRUE(abs_diff(res.value().hr_bpm_x100, 7229U) <= 1U);
        EXPECT_TRUE(abs_diff(res.value().sdnn_us, 32863U) <= 2U);
        EXPECT_TRUE(abs_diff(res.value().rmssd_us, 60000U) <= 2U);
        EXPECT_EQ(res.value().pnn50_permille, 1000U);
    }

    // Strict > 50 ms boundary check for pNN50:
    // Beats: 800, 820 (+20), 880 (+60 >50), 830 (-50 NOT >50), 910 (+80 >50)
    // 4 contiguous pairs -> 2 strictly > 50 ms -> pNN50 = 500 permille (50.0%)
    p05::HrvAnalyzer hrv50;
    hrv50.configure(p05::MathMode::Q15);
    for (const uint32_t rr : {800000U, 820000U, 880000U, 830000U, 910000U}) {
        EXPECT_EQ(hrv50.push_rr_us(rr), p05::HrvAnalyzer::RrDecision::Accepted);
    }
    const auto r50 = hrv50.compute_metrics();
    EXPECT_TRUE(r50.is_ok());
    EXPECT_EQ(r50.value().pnn50_permille, 500U);
}

void test_03_outlier_rejection_and_no_bridging_across_artifacts() {
    p05::HrvAnalyzer hrv;
    hrv.configure(p05::MathMode::Q15);

    // Physiological bounds [300 ms, 2000 ms] (OPEN-10d)
    EXPECT_EQ(hrv.push_rr_us(299999U), p05::HrvAnalyzer::RrDecision::RejectedRange);
    EXPECT_EQ(hrv.push_rr_us(2000001U), p05::HrvAnalyzer::RrDecision::RejectedRange);
    EXPECT_EQ(hrv.valid_count(), 0U);
    EXPECT_EQ(hrv.rejected_outlier_count(), 2U);

    // Seed 5 valid steady beats around 800 ms (median_5 == 800,000 us)
    for (const uint32_t rr : {795000U, 800000U, 805000U, 800000U, 800000U}) {
        EXPECT_EQ(hrv.push_rr_us(rr), p05::HrvAnalyzer::RrDecision::Accepted);
    }
    EXPECT_EQ(hrv.contiguous_pair_count(), 4U);

    // 20% threshold boundary around median 800 ms -> [640 ms, 960 ms] allowed
    EXPECT_EQ(hrv.push_rr_us(961000U), p05::HrvAnalyzer::RrDecision::RejectedOutlier);
    EXPECT_EQ(hrv.push_rr_us(639000U), p05::HrvAnalyzer::RrDecision::RejectedOutlier);
    EXPECT_EQ(hrv.push_rr_us(1500000U), p05::HrvAnalyzer::RrDecision::RejectedOutlier);
    EXPECT_EQ(hrv.valid_count(), 5U);

    // Next valid beat (800 ms) after rejected outlier MUST NOT form a contiguous pair across the outlier
    EXPECT_EQ(hrv.push_rr_us(960000U), p05::HrvAnalyzer::RrDecision::Accepted);  // exact +20% OK
    EXPECT_EQ(hrv.valid_count(), 6U);
    // Still 4 contiguous pairs (the 800 -> [rejected] -> 960 jump is NOT counted in RMSSD/pNN50!)
    EXPECT_EQ(hrv.contiguous_pair_count(), 4U);
    const auto res = hrv.compute_metrics();
    EXPECT_TRUE(res.is_ok());
    // All 4 contiguous pairs had diffs <= 5 ms, so pNN50 MUST remain 0 (not polluted by 160 ms jump!)
    EXPECT_EQ(res.value().pnn50_permille, 0U);
    EXPECT_TRUE(res.value().rmssd_us <= 6000U);
}

void test_04_gap_interruption_and_detected_r_peak_adapter() {
    p05::HrvAnalyzer hrv;
    hrv.configure(p05::MathMode::Q15);

    // DetectedRPeak with has_rr == false (first beat after boot/gap) is ignored without fault
    p05::DetectedRPeak p0{};
    p0.has_rr = false;
    p0.rr_interval_us = 0U;
    EXPECT_EQ(hrv.push_r_peak(p0), p05::HrvAnalyzer::RrDecision::IgnoredNoRr);
    EXPECT_EQ(hrv.valid_count(), 0U);

    p05::DetectedRPeak p1{};
    p1.has_rr = true;
    p1.rr_interval_us = 800000U;
    EXPECT_EQ(hrv.push_r_peak(p1), p05::HrvAnalyzer::RrDecision::Accepted);
    EXPECT_EQ(hrv.push_r_peak(p1), p05::HrvAnalyzer::RrDecision::Accepted);
    EXPECT_TRUE(hrv.compute_metrics().is_ok());

    // SeqGap interruption marks fault active and breaks continuity
    hrv.on_interruption(
        p05::FailureStatus{p05::CanonicalState::Stale, p05::DetailCode::SeqGap});
    const auto r_gap = hrv.compute_metrics();
    EXPECT_TRUE(r_gap.is_err());
    EXPECT_EQ(r_gap.canonical_state(), p05::CanonicalState::Stale);
    EXPECT_EQ(r_gap.detail_code(), p05::DetailCode::SeqGap);

    // Recovery beat at 900 ms does NOT create a 100 ms jump across the gap
    p05::DetectedRPeak p2{};
    p2.has_rr = true;
    p2.rr_interval_us = 900000U;
    EXPECT_EQ(hrv.push_r_peak(p2), p05::HrvAnalyzer::RrDecision::Accepted);
    EXPECT_EQ(hrv.contiguous_pair_count(), 1U);  // only the pre-gap (800, 800) pair
    const auto r_rec = hrv.compute_metrics();
    EXPECT_TRUE(r_rec.is_ok());
    EXPECT_EQ(r_rec.value().rmssd_us, 0U);       // 800 -> 800 diff is 0; 800 -> 900 was not bridged
    EXPECT_EQ(r_rec.value().pnn50_permille, 0U);
}

void test_05_64_beat_window_eviction_and_boundary_overflow_safety() {
    // 1. Sliding window eviction: 36 beats @ 900 ms then 64 beats @ 750 ms (80.00 BPM)
    {
        p05::HrvAnalyzer hrv;
        hrv.configure(p05::MathMode::Q15);
        for (int i = 0; i < 36; ++i) {
            EXPECT_EQ(hrv.push_rr_us(900000U), p05::HrvAnalyzer::RrDecision::Accepted);
        }
        // Ramp smoothly down from 900 ms -> 750 ms within 20% rule (750/900 = -16.7% <= 20%)
        for (int i = 0; i < 64; ++i) {
            EXPECT_EQ(hrv.push_rr_us(750000U), p05::HrvAnalyzer::RrDecision::Accepted);
        }
        EXPECT_EQ(hrv.valid_count(), 64U);
        EXPECT_EQ(hrv.contiguous_pair_count(), 63U);
        const auto res = hrv.compute_metrics();
        EXPECT_TRUE(res.is_ok());
        EXPECT_EQ(res.value().mean_rr_us, 750000U);
        EXPECT_EQ(res.value().hr_bpm_x100, 8000U);
        EXPECT_EQ(res.value().sdnn_us, 0U);
        EXPECT_EQ(res.value().rmssd_us, 0U);
        EXPECT_EQ(res.value().pnn50_permille, 0U);
    }

    // 2. Max physiological boundary (64 beats @ 2,000,000 us = 30.00 BPM) -> no 64-bit overflow
    {
        p05::HrvAnalyzer hrv;
        hrv.configure(p05::MathMode::Q15);
        for (int i = 0; i < 64; ++i) {
            EXPECT_EQ(hrv.push_rr_us(2000000U), p05::HrvAnalyzer::RrDecision::Accepted);
        }
        const auto res = hrv.compute_metrics();
        EXPECT_TRUE(res.is_ok());
        EXPECT_EQ(res.value().mean_rr_us, 2000000U);
        EXPECT_EQ(res.value().hr_bpm_x100, 3000U);
    }

    // 3. Min physiological boundary (64 beats @ 300,000 us = 200.00 BPM) -> no overflow
    {
        p05::HrvAnalyzer hrv;
        hrv.configure(p05::MathMode::Q15);
        for (int i = 0; i < 64; ++i) {
            EXPECT_EQ(hrv.push_rr_us(300000U), p05::HrvAnalyzer::RrDecision::Accepted);
        }
        const auto res = hrv.compute_metrics();
        EXPECT_TRUE(res.is_ok());
        EXPECT_EQ(res.value().mean_rr_us, 300000U);
        EXPECT_EQ(res.value().hr_bpm_x100, 20000U);
    }
}

void test_06_ablation1_integer_vs_soft_float_equivalence() {
    p05::HrvAnalyzer hrv_int;
    p05::HrvAnalyzer hrv_flt;
    hrv_int.configure(p05::MathMode::Q15);
    hrv_flt.configure(p05::MathMode::SoftFloat);

    // Realistic 64-beat respiratory sinus arrhythmia pattern around 820 ms +/- 60 ms
    for (int i = 0; i < 64; ++i) {
        const double wave = 55000.0 * std::sin(2.0 * 3.141592653589793 * static_cast<double>(i) / 7.5);
        const double jitter = ((i % 3 == 0) ? 18000.0 : -9000.0);
        const uint32_t rr = static_cast<uint32_t>(std::lround(820000.0 + wave + jitter));
        EXPECT_EQ(hrv_int.push_rr_us(rr), p05::HrvAnalyzer::RrDecision::Accepted);
        EXPECT_EQ(hrv_flt.push_rr_us(rr), p05::HrvAnalyzer::RrDecision::Accepted);
    }

    const auto ri = hrv_int.compute_metrics();
    const auto rf = hrv_flt.compute_metrics();
    EXPECT_TRUE(ri.is_ok());
    EXPECT_TRUE(rf.is_ok());

    // Ablation 1 precision verification: integer matches Soft-Float within <= 2 us / 0.01 BPM
    EXPECT_TRUE(abs_diff(ri.value().mean_rr_us, rf.value().mean_rr_us) <= 2U);
    EXPECT_TRUE(abs_diff(ri.value().hr_bpm_x100, rf.value().hr_bpm_x100) <= 1U);
    EXPECT_TRUE(abs_diff(ri.value().sdnn_us, rf.value().sdnn_us) <= 2U);
    EXPECT_TRUE(abs_diff(ri.value().rmssd_us, rf.value().rmssd_us) <= 2U);
    EXPECT_EQ(ri.value().pnn50_permille, rf.value().pnn50_permille);
}

void test_07_post_interruption_hr_shift_recovery() {
    p05::HrvAnalyzer hrv;
    hrv.configure(p05::MathMode::Q15);

    // 6 baseline beats at 1000 ms (60 BPM)
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(hrv.push_rr_us(1000000U), p05::HrvAnalyzer::RrDecision::Accepted);
    }

    // Interruption (e.g., Leads-Off while resting, re-attached at 100 BPM = 600 ms, -40% shift)
    hrv.on_interruption(
        p05::FailureStatus{p05::CanonicalState::SensorUnavailable, p05::DetailCode::LeadsOff});
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(hrv.push_rr_us(600000U), p05::HrvAnalyzer::RrDecision::Accepted);
    }
    // 6th beat at 600 ms accepted against newly re-seeded 5-beat median, while 1100 ms outlier rejected
    EXPECT_EQ(hrv.push_rr_us(600000U), p05::HrvAnalyzer::RrDecision::Accepted);
    EXPECT_EQ(hrv.push_rr_us(1100000U), p05::HrvAnalyzer::RrDecision::RejectedOutlier);
    EXPECT_TRUE(hrv.compute_metrics().is_ok());
}

}  // namespace

int main() {
    test_01_insufficient_samples_never_return_valid_zero();
    test_02_exact_analytical_5_hrv_metrics();
    test_03_outlier_rejection_and_no_bridging_across_artifacts();
    test_04_gap_interruption_and_detected_r_peak_adapter();
    test_05_64_beat_window_eviction_and_boundary_overflow_safety();
    test_06_ablation1_integer_vs_soft_float_equivalence();
    test_07_post_interruption_hr_shift_recovery();

    std::printf("P05 Task Card 3 HRV Analyzer Host Unit Tests: %d assertions, %d failed\n",
                g_tests_run, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
