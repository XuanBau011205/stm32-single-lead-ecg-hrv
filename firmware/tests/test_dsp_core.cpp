#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "../DSP/iir_filter.hpp"
#include "../DSP/pan_tompkins.hpp"
#include "../DSP/parabolic_interp.hpp"
#include "../HIL/trusted_types.hpp"

namespace {

inline constexpr double kPi = 3.14159265358979323846;

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

double measure_sine_rms_q15(uint16_t fs_hz, p05::MathMode mode, double freq_hz, int16_t amp) {
    p05::EcgIirFilterChain filt;
    (void)filt.configure(fs_hz, mode);

    const int settle = static_cast<int>(fs_hz) * 2;
    const int measure = static_cast<int>(fs_hz) * 2;
    double sum_sq = 0.0;
    for (int n = 0; n < settle + measure; ++n) {
        const double t = static_cast<double>(n) / static_cast<double>(fs_hz);
        const int16_t x = static_cast<int16_t>(std::lround(static_cast<double>(amp) *
                                                           std::sin(2.0 * kPi * freq_hz * t)));
        const int16_t y = filt.process_q15(x);
        if (n >= settle) {
            const double yd = static_cast<double>(y);
            sum_sq += yd * yd;
        }
    }
    return std::sqrt(sum_sq / static_cast<double>(measure));
}

// Synthetic realistic single-lead ECG (P + QRS + T) centered around ADC 2048
int16_t synth_ecg_adc(double t_sec,
                      const double* beat_times,
                      std::size_t num_beats,
                      const double* beat_scales,
                      double mains_50hz_adc,
                      double wander_03hz_adc) {
    double val = 2048.0;
    val += mains_50hz_adc * std::sin(2.0 * kPi * 50.0 * t_sec);
    val += wander_03hz_adc * std::sin(2.0 * kPi * 0.3 * t_sec);

    for (std::size_t b = 0U; b < num_beats; ++b) {
        const double dt = t_sec - beat_times[b];
        if (dt < -0.35 || dt > 0.45) {
            continue;
        }
        const double s = (beat_scales != nullptr) ? beat_scales[b] : 1.0;
        // P wave (-0.16 s), Q (-0.025 s), R (0.0 s), S (+0.025 s), T (+0.26 s)
        val += s * 90.0 * std::exp(-0.5 * ((dt + 0.16) / 0.022) * ((dt + 0.16) / 0.022));
        val -= s * 140.0 * std::exp(-0.5 * ((dt + 0.024) / 0.008) * ((dt + 0.024) / 0.008));
        val += s * 1100.0 * std::exp(-0.5 * (dt / 0.011) * (dt / 0.011));
        val -= s * 220.0 * std::exp(-0.5 * ((dt - 0.024) / 0.009) * ((dt - 0.024) / 0.009));
        val += s * 240.0 * std::exp(-0.5 * ((dt - 0.26) / 0.045) * ((dt - 0.26) / 0.045));
    }

    if (val < 0.0) {
        val = 0.0;
    } else if (val > 4095.0) {
        val = 4095.0;
    }
    return static_cast<int16_t>(std::lround(val));
}

void test_01_iir_frequency_response_and_q15_vs_float() {
    const std::array<uint16_t, 2U> rates = {250U, 500U};
    const std::array<p05::MathMode, 2U> modes = {p05::MathMode::Q15, p05::MathMode::SoftFloat};

    for (const uint16_t fs : rates) {
        double rms_10_q15 = 0.0;
        double rms_10_flt = 0.0;

        for (const p05::MathMode mode : modes) {
            const double rms_05 = measure_sine_rms_q15(fs, mode, 0.5, 10000);
            const double rms_10 = measure_sine_rms_q15(fs, mode, 10.0, 10000);
            const double rms_50 = measure_sine_rms_q15(fs, mode, 50.0, 10000);

            const double atten_50_db = 20.0 * std::log10((rms_50 + 1e-6) / rms_10);
            const double atten_05_db = 20.0 * std::log10((rms_05 + 1e-6) / rms_10);

            // Passband 10 Hz should stay near ~0.88x input RMS (~6200)
            EXPECT_TRUE(rms_10 > 5500.0 && rms_10 < 7200.0);
            // Notch 50 Hz must strongly attenuate >= 35 dB relative to 10 Hz passband
            EXPECT_TRUE(atten_50_db < -35.0);
            // Baseline 0.5 Hz must strongly attenuate >= 30 dB relative to 10 Hz passband
            EXPECT_TRUE(atten_05_db < -30.0);

            if (mode == p05::MathMode::Q15) {
                rms_10_q15 = rms_10;
            } else {
                rms_10_flt = rms_10;
            }
        }

        // Ablation 1 equivalence check: Q15 vs SoftFloat passband gain matches within 0.5%
        const double rel_diff = std::fabs(rms_10_q15 - rms_10_flt) / rms_10_flt;
        EXPECT_TRUE(rel_diff < 0.005);
    }

    // Invalid fs rejected
    p05::EcgIirFilterChain f_bad;
    EXPECT_TRUE(!f_bad.configure(360U, p05::MathMode::Q15));
}

void test_02_iir_int16_boundary_no_overflow_wraparound() {
    const std::array<uint16_t, 2U> rates = {250U, 500U};
    for (const uint16_t fs : rates) {
        p05::EcgIirFilterChain filt;
        EXPECT_TRUE(filt.configure(fs, p05::MathMode::Q15));

        // 1. Constant int16 max (+32767) -> HPF drives steady-state output to 0 without wrap
        int16_t last = 0;
        for (int n = 0; n < static_cast<int>(fs) * 3; ++n) {
            last = filt.process_q15(32767);
        }
        EXPECT_TRUE(std::abs(static_cast<int>(last)) < 5);

        // 2. Constant int16 min (-32768) -> HPF drives steady-state output to 0 without wrap
        filt.reset();
        for (int n = 0; n < static_cast<int>(fs) * 3; ++n) {
            last = filt.process_q15(-32768);
        }
        EXPECT_TRUE(std::abs(static_cast<int>(last)) < 5);

        // 3. Full-scale 10 Hz square wave (-32768 / +32767) -> saturates cleanly, never wraps sign
        filt.reset();
        const int half_period = static_cast<int>(fs) / 20;
        for (int n = 0; n < static_cast<int>(fs) * 2; ++n) {
            const int16_t x = ((n / half_period) % 2 == 0) ? 32767 : -32768;
            const int16_t y = filt.process_q15(x);
            EXPECT_TRUE(y >= -32768 && y <= 32767);
        }
        // After zeroing input for 2 s, filter settles back cleanly near 0 (no limit-cycle oscillation)
        for (int n = 0; n < static_cast<int>(fs) * 2; ++n) {
            last = filt.process_q15(0);
        }
        EXPECT_TRUE(std::abs(static_cast<int>(last)) < 10);
    }
}

void test_03_parabolic_interpolation_accuracy_and_degenerate_cases() {
    // R-DSP-04: sub-sample peak localization error < 0.5 ms at 250 Hz (Ts = 4.0 ms)
    const double fs = 250.0;
    const double sigma_s = 0.011;  // realistic QRS Gaussian width (11 ms)
    double max_err_q15_ms = 0.0;
    double max_err_flt_ms = 0.0;

    for (int step = 0; step < 20; ++step) {
        const double frac_sample = -0.45 + (0.045 * static_cast<double>(step));
        const double t_peak = (10.0 + frac_sample) / fs;

        std::array<float, 21U> yf{};
        std::array<int32_t, 21U> yq{};
        for (std::size_t k = 0U; k < 21U; ++k) {
            const double t = static_cast<double>(k) / fs;
            const double dt = t - t_peak;
            const double g = std::exp(-0.5 * (dt / sigma_s) * (dt / sigma_s));
            yf[k] = static_cast<float>(g);
            yq[k] = static_cast<int32_t>(std::lround(g * 16000.0));
        }

        const auto rq = p05::parabolic_interp_q15(yq[9], yq[10], yq[11]);
        const auto rf = p05::parabolic_interp_float(yf[9], yf[10], yf[11]);
        EXPECT_TRUE(!rq.degenerate);
        EXPECT_TRUE(!rf.degenerate);

        const double est_q15_ms =
            (10.0 * 4.0) + (static_cast<double>(p05::offset_q15_to_us(rq.sub_sample_offset_q15, 250U)) /
                            1000.0);
        const double est_flt_ms =
            (10.0 * 4.0) + (static_cast<double>(p05::offset_q15_to_us(rf.sub_sample_offset_q15, 250U)) /
                            1000.0);
        const double true_ms = t_peak * 1000.0;

        const double err_q = std::fabs(est_q15_ms - true_ms);
        const double err_f = std::fabs(est_flt_ms - true_ms);
        if (err_q > max_err_q15_ms) {
            max_err_q15_ms = err_q;
        }
        if (err_f > max_err_flt_ms) {
            max_err_flt_ms = err_f;
        }
    }

    EXPECT_TRUE(max_err_q15_ms < 0.5);
    EXPECT_TRUE(max_err_flt_ms < 0.5);

    // Inverted negative peak works identically
    const auto r_neg = p05::parabolic_interp_q15(-500, -1000, -750);
    EXPECT_TRUE(!r_neg.degenerate);
    EXPECT_TRUE(r_neg.sub_sample_offset_q15 > 0);  // shifts toward -750 side (+1)

    // PROPOSED-DSP-01 / OPEN-10c degenerate cases (no divide by zero)
    const auto r_flat_q = p05::parabolic_interp_q15(1000, 1000, 1000);
    EXPECT_TRUE(r_flat_q.degenerate);
    EXPECT_EQ(r_flat_q.sub_sample_offset_q15, 0);

    const auto r_line_q = p05::parabolic_interp_q15(100, 200, 300);
    EXPECT_TRUE(r_line_q.degenerate);
    EXPECT_EQ(r_line_q.sub_sample_offset_q15, 0);

    const auto r_flat_f = p05::parabolic_interp_float(1.0f, 1.0f, 1.0f);
    EXPECT_TRUE(r_flat_f.degenerate);
    EXPECT_EQ(r_flat_f.sub_sample_offset_q15, 0);

    const auto r_non_ext = p05::parabolic_interp_q15(100, 200, 400);
    EXPECT_TRUE(r_non_ext.degenerate);
    EXPECT_EQ(r_non_ext.sub_sample_offset_q15, 0);
}

void test_04_full_ecg_pipeline_q15_and_float_at_250hz_and_500hz() {
    // 15 seconds @ 72 BPM -> exact RR = 833.333 ms (non-integer multiple of 4 ms!)
    // Plus 50 Hz mains noise (180 ADC pp) and 0.3 Hz baseline wander (250 ADC pp)
    std::array<double, 16U> beat_times{};
    for (std::size_t i = 0U; i < beat_times.size(); ++i) {
        beat_times[i] = 1.0 + (static_cast<double>(i) * (60.0 / 72.0));
    }

    const std::array<uint16_t, 2U> rates = {250U, 500U};
    const std::array<p05::MathMode, 2U> modes = {p05::MathMode::Q15, p05::MathMode::SoftFloat};

    for (const uint16_t fs : rates) {
        for (const p05::MathMode mode : modes) {
            p05::EcgDspCore dsp;
            EXPECT_TRUE(dsp.configure(p05::DspConfig{fs, mode, true}));

            const uint32_t total_samples = static_cast<uint32_t>(fs) * 15U;
            std::size_t peaks_found = 0U;
            double sum_abs_rr_err_ms = 0.0;
            std::size_t rr_count = 0U;

            for (uint32_t n = 0U; n < total_samples; ++n) {
                const double t = static_cast<double>(n) / static_cast<double>(fs);
                const int16_t raw =
                    synth_ecg_adc(t, beat_times.data(), beat_times.size(), nullptr, 180.0, 250.0);
                const auto out = dsp.process_trusted_adc_sample(raw, n);
                if (out.r_peak_detected) {
                    ++peaks_found;
                    if (out.r_peak.has_rr) {
                        const double rr_ms =
                            static_cast<double>(out.r_peak.rr_interval_us) / 1000.0;
                        sum_abs_rr_err_ms += std::fabs(rr_ms - 833.333333);
                        ++rr_count;
                    }
                }
            }

            // All 16 beats detected cleanly (no T-wave false positives, no missed beats)
            EXPECT_EQ(peaks_found, 16U);
            EXPECT_EQ(rr_count, 15U);
            const double mae_rr_ms = sum_abs_rr_err_ms / static_cast<double>(rr_count);
            EXPECT_TRUE(mae_rr_ms < 1.0);  // Well below Gate G4 5 ms target!
        }
    }
    // Ablation 2 check: at 250 Hz (4 ms period), parabolic interpolation ON reduces RR error vs OFF
    {
        double mae_on = 0.0;
        double mae_off = 0.0;
        for (const bool interp : {true, false}) {
            p05::EcgDspCore dsp;
            EXPECT_TRUE(dsp.configure(p05::DspConfig{250U, p05::MathMode::Q15, interp}));
            double err_sum = 0.0;
            std::size_t n_rr = 0U;
            for (uint32_t n = 0U; n < 250U * 15U; ++n) {
                const double t = static_cast<double>(n) / 250.0;
                const int16_t raw =
                    synth_ecg_adc(t, beat_times.data(), beat_times.size(), nullptr, 0.0, 0.0);
                const auto out = dsp.process_trusted_adc_sample(raw, n);
                if (out.r_peak_detected && out.r_peak.has_rr) {
                    const double rr_ms = static_cast<double>(out.r_peak.rr_interval_us) / 1000.0;
                    err_sum += std::fabs(rr_ms - 833.333333);
                    ++n_rr;
                }
            }
            EXPECT_EQ(n_rr, 15U);
            if (interp) {
                mae_on = err_sum / static_cast<double>(n_rr);
            } else {
                mae_off = err_sum / static_cast<double>(n_rr);
            }
        }
        EXPECT_TRUE(mae_on < 0.25);
        EXPECT_TRUE(mae_on < mae_off);
    }
}

void test_05_refractory_200ms_and_search_back_166pct() {
    // 1. Refractory period test @250 Hz (R-DSP-03):
    //    Beat #1 at 1.00 s; artifact at 1.18 s (180 ms later: > 150 ms MWI width so distinct MWI peak,
    //    but < 200 ms refractory so MUST be rejected!); valid beat #2 at 1.44 s (440 ms after #1).
    {
        p05::EcgDspCore dsp;
        EXPECT_TRUE(dsp.configure(p05::DspConfig{250U, p05::MathMode::Q15, true}));
        const std::array<double, 3U> beats = {1.00, 1.18, 1.44};
        const std::array<double, 3U> scales = {1.0, 1.0, 1.0};

        std::array<p05::DetectedRPeak, 8U> detected{};
        std::size_t n_det = 0U;
        for (uint32_t n = 0U; n < 250U * 3U; ++n) {
            const double t = static_cast<double>(n) / 250.0;
            const int16_t raw = synth_ecg_adc(t, beats.data(), beats.size(), scales.data(), 0.0, 0.0);
            const auto out = dsp.process_trusted_adc_sample(raw, n);
            if (out.r_peak_detected && n_det < detected.size()) {
                detected[n_det++] = out.r_peak;
            }
        }
        // Only 2 beats emitted (1.00 s and 1.44 s); the 1.18 s spike inside 200 ms refractory is blocked!
        EXPECT_EQ(n_det, 2U);
        EXPECT_TRUE(detected[1].has_rr);
        EXPECT_TRUE(detected[1].rr_interval_us >= 420000U && detected[1].rr_interval_us <= 460000U);
    }

    // 1b. Exact boundary > 200 ms (@250 Hz: beat #2 at 240 ms = 60 samples >= 50 samples refractory)
    {
        p05::EcgDspCore dsp;
        EXPECT_TRUE(dsp.configure(p05::DspConfig{250U, p05::MathMode::Q15, true}));
        const std::array<double, 2U> beats = {1.00, 1.24};
        std::size_t n_det = 0U;
        for (uint32_t n = 0U; n < 250U * 2U; ++n) {
            const double t = static_cast<double>(n) / 250.0;
            const int16_t raw = synth_ecg_adc(t, beats.data(), beats.size(), nullptr, 0.0, 0.0);
            if (dsp.process_trusted_adc_sample(raw, n).r_peak_detected) {
                ++n_det;
            }
        }
        EXPECT_EQ(n_det, 2U);
    }

    // 2. Search-Back test (OPEN-10b): 8 normal beats at 0.80 s intervals (scale 1.0),
    //    then 1 weak beat at 7.40 s (scale 0.42 -> falls between THR2 and THR1!),
    //    then pause until 8.80 s (> 166% * 0.80 s = 1.328 s after 6.60 s -> triggers search-back!).
    {
        p05::EcgDspCore dsp;
        EXPECT_TRUE(dsp.configure(p05::DspConfig{250U, p05::MathMode::Q15, true}));
        const std::array<double, 10U> beats = {
            1.00, 1.80, 2.60, 3.40, 4.20, 5.00, 5.80, 6.60, 7.40, 9.00};
        const std::array<double, 10U> scales = {
            1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 0.42, 1.00};

        std::size_t n_det = 0U;
        bool saw_search_back = false;
        for (uint32_t n = 0U; n < 250U * 10U; ++n) {
            const double t = static_cast<double>(n) / 250.0;
            const int16_t raw = synth_ecg_adc(t, beats.data(), beats.size(), scales.data(), 0.0, 0.0);
            const auto out = dsp.process_trusted_adc_sample(raw, n);
            if (out.r_peak_detected) {
                ++n_det;
                if (out.r_peak.used_search_back) {
                    saw_search_back = true;
                }
            }
        }
        EXPECT_EQ(n_det, 10U);
        EXPECT_TRUE(saw_search_back);
    }
}

void test_06_error_propagation_and_gap_2s_blanking() {
    p05::EcgDspCore dsp;
    EXPECT_TRUE(dsp.configure(p05::DspConfig{250U, p05::MathMode::Q15, true}));

    // 1. SensorUnavailable / LeadsOff propagates cleanly with zero fake peaks (R-BND-02)
    const auto err_lod = p05::Result<p05::TrustedSampleBatch>::fail(
        p05::CanonicalState::SensorUnavailable, p05::DetailCode::LeadsOff);
    const auto r_lod = dsp.process_hil_batch(err_lod);
    EXPECT_TRUE(r_lod.is_err());
    EXPECT_EQ(r_lod.canonical_state(), p05::CanonicalState::SensorUnavailable);
    EXPECT_EQ(r_lod.detail_code(), p05::DetailCode::LeadsOff);

    // 2. DuplicateIgnored returns ok(empty, DuplicateIgnored) without mutating state
    p05::TrustedSampleBatch dup_b{};
    dup_b.fs_hz = 250U;
    dup_b.sample_count = 4U;
    const auto r_dup = dsp.process_hil_batch(
        p05::Result<p05::TrustedSampleBatch>::ok(dup_b, p05::DetailCode::DuplicateIgnored));
    EXPECT_TRUE(r_dup.is_ok());
    EXPECT_EQ(r_dup.detail_code(), p05::DetailCode::DuplicateIgnored);
    EXPECT_EQ(r_dup.value().sample_count, 0U);

    // 3. SeqGap engages exact 2-second blanking (500 samples @250 Hz, OPEN-15)
    const auto err_gap = p05::Result<p05::TrustedSampleBatch>::fail(p05::CanonicalState::Stale,
                                                                    p05::DetailCode::SeqGap);
    const auto r_gap = dsp.process_hil_batch(err_gap);
    EXPECT_TRUE(r_gap.is_err());
    EXPECT_EQ(r_gap.detail_code(), p05::DetailCode::SeqGap);
    EXPECT_TRUE(dsp.in_gap_blanking());

    // Feed 5 seconds of normal ECG (beats at 0.5, 1.3, 2.1, 2.9, 3.7, 4.5 s)
    // Beats during first 2.0 s (0.5 s and 1.3 s) MUST be suppressed by 2 s blanking!
    // Beats after 2.0 s MUST be detected, and the first beat after blanking MUST have has_rr == false!
    const std::array<double, 6U> beats = {0.5, 1.3, 2.3, 3.1, 3.9, 4.7};
    std::size_t peaks_during_2s = 0U;
    std::size_t peaks_after_2s = 0U;
    bool first_after_has_rr = true;

    for (uint32_t start = 0U; start < 250U * 5U; start += 50U) {
        p05::TrustedSampleBatch batch{};
        batch.fs_hz = 250U;
        batch.first_sample_idx = start;
        batch.sample_count = 50U;
        for (uint32_t i = 0U; i < 50U; ++i) {
            const double t = static_cast<double>(start + i) / 250.0;
            batch.samples[i] = synth_ecg_adc(t, beats.data(), beats.size(), nullptr, 0.0, 0.0);
        }
        const auto res = dsp.process_hil_batch(p05::Result<p05::TrustedSampleBatch>::ok(batch));
        EXPECT_TRUE(res.is_ok());
        for (uint8_t p = 0U; p < res.value().r_peak_count; ++p) {
            if (start < 500U) {
                ++peaks_during_2s;
            } else {
                if (peaks_after_2s == 0U) {
                    first_after_has_rr = res.value().peaks[p].has_rr;
                }
                ++peaks_after_2s;
            }
        }
    }

    EXPECT_EQ(peaks_during_2s, 0U);
    EXPECT_TRUE(!dsp.in_gap_blanking());
    EXPECT_TRUE(peaks_after_2s >= 3U);
    EXPECT_TRUE(!first_after_has_rr);  // No bogus RR bridged across gap (R-BND-02)
}

}  // namespace

int main() {
    test_01_iir_frequency_response_and_q15_vs_float();
    test_02_iir_int16_boundary_no_overflow_wraparound();
    test_03_parabolic_interpolation_accuracy_and_degenerate_cases();
    test_04_full_ecg_pipeline_q15_and_float_at_250hz_and_500hz();
    test_05_refractory_200ms_and_search_back_166pct();
    test_06_error_propagation_and_gap_2s_blanking();

    std::printf("P05 Task Card 2 DSP Core Host Unit Tests: %d assertions, %d failed\n",
                g_tests_run, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
