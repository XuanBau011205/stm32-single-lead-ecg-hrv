#include "pan_tompkins.hpp"

namespace p05 {

namespace {

inline constexpr uint32_t kMinMwiNoiseFloorQ = 16U;
inline constexpr float kMinMwiNoiseFloorF = 1.5e-8f;

}  // namespace

PanTompkinsDetector::PanTompkinsDetector() noexcept {
    (void)configure(DspConfig{});
}

bool PanTompkinsDetector::configure(const DspConfig& cfg) noexcept {
    if (cfg.fs_hz != kFs250Hz && cfg.fs_hz != kFs500Hz) {
        return false;
    }
    cfg_ = cfg;
    mwi_len_ = (cfg.fs_hz == kFs500Hz) ? 75U : 38U;             // 150 ms (OPEN-10b)
    refractory_samples_ = (cfg.fs_hz == kFs500Hz) ? 100U : 50U;  // 200 ms (SPEC section 3.2)
    settle_samples_ = static_cast<uint32_t>(cfg.fs_hz) / 4U;     // 250 ms IIR startup settle
    init_floor_q_ = (cfg.fs_hz == kFs500Hz) ? 2000U : 8000U;
    init_floor_f_ = (cfg.fs_hz == kFs500Hz) ? 1.9e-6f : 7.5e-6f;
    reset();
    return true;
}

void PanTompkinsDetector::reset() noexcept {
    blanking_remaining_ = 0U;
    samples_seen_ = 0U;
    locked_polarity_ = 0;

    deriv_q_.fill(0);
    deriv_f_.fill(0.0f);
    mwi_buf_q_.fill(0U);
    mwi_buf_f_.fill(0.0f);
    mwi_pos_ = 0U;
    mwi_sum_q_ = 0U;
    mwi_sum_f_ = 0.0f;

    mwi_prev2_q_ = 0U;
    mwi_prev1_q_ = 0U;
    mwi_prev2_f_ = 0.0f;
    mwi_prev1_f_ = 0.0f;

    bp_hist_q_.fill(0);
    bp_hist_f_.fill(0.0f);
    bp_idx_hist_.fill(0U);
    bp_head_ = 0U;
    bp_count_ = 0U;

    thresh_init_ = false;
    spki_q_ = 0U;
    npki_q_ = 0U;
    thr1_q_ = 0U;
    thr2_q_ = 0U;

    spki_f_ = 0.0f;
    npki_f_ = 0.0f;
    thr1_f_ = 0.0f;
    thr2_f_ = 0.0f;

    has_last_peak_ = false;
    last_mwi_peak_idx_ = 0U;
    last_refined_peak_idx_ = 0U;
    last_offset_q15_ = 0;

    rr_samples_hist_.fill(0U);
    rr_hist_pos_ = 0U;
    rr_hist_count_ = 0U;

    has_sb_cand_ = false;
    sb_cand_ = CandidatePeak{};
}

void PanTompkinsDetector::reset_with_gap_blanking() noexcept {
    reset();
    // OPEN-15: block peak emission for 2 seconds after gap to re-learn thresholds
    blanking_remaining_ = static_cast<uint32_t>(cfg_.fs_hz) * 2U;
}

void PanTompkinsDetector::break_rr_continuity() noexcept {
    has_last_peak_ = false;
    has_sb_cand_ = false;
}

void PanTompkinsDetector::update_thresholds_q(uint32_t peaki,
                                              bool is_signal,
                                              bool is_searchback) noexcept {
    if (!thresh_init_) {
        spki_q_ = peaki;
        npki_q_ = peaki >> 3U;
        thresh_init_ = true;
    } else if (is_signal) {
        if (is_searchback) {
            // SPKI = 0.25 * PEAKI + 0.75 * SPKI
            spki_q_ = static_cast<uint32_t>(
                (static_cast<uint64_t>(peaki) + (3ULL * static_cast<uint64_t>(spki_q_))) >> 2U);
        } else {
            // SPKI = 0.125 * PEAKI + 0.875 * SPKI (OPEN-10b)
            spki_q_ = static_cast<uint32_t>(
                (static_cast<uint64_t>(peaki) + (7ULL * static_cast<uint64_t>(spki_q_))) >> 3U);
        }
    } else {
        // NPKI = 0.125 * PEAKI + 0.875 * NPKI (OPEN-10b)
        npki_q_ = static_cast<uint32_t>(
            (static_cast<uint64_t>(peaki) + (7ULL * static_cast<uint64_t>(npki_q_))) >> 3U);
    }

    if (spki_q_ > npki_q_) {
        thr1_q_ = npki_q_ + ((spki_q_ - npki_q_) >> 2U);  // NPKI + 0.25 * (SPKI - NPKI)
    } else {
        thr1_q_ = npki_q_;
    }
    if (thr1_q_ < kMinMwiNoiseFloorQ) {
        thr1_q_ = kMinMwiNoiseFloorQ;
    }
    thr2_q_ = thr1_q_ >> 1U;  // 0.5 * THR1
}

void PanTompkinsDetector::update_thresholds_f(float peaki,
                                              bool is_signal,
                                              bool is_searchback) noexcept {
    if (!thresh_init_) {
        spki_f_ = peaki;
        npki_f_ = 0.125f * peaki;
        thresh_init_ = true;
    } else if (is_signal) {
        if (is_searchback) {
            spki_f_ = (0.25f * peaki) + (0.75f * spki_f_);
        } else {
            spki_f_ = (0.125f * peaki) + (0.875f * spki_f_);
        }
    } else {
        npki_f_ = (0.125f * peaki) + (0.875f * npki_f_);
    }

    if (spki_f_ > npki_f_) {
        thr1_f_ = npki_f_ + (0.25f * (spki_f_ - npki_f_));
    } else {
        thr1_f_ = npki_f_;
    }
    if (thr1_f_ < kMinMwiNoiseFloorF) {
        thr1_f_ = kMinMwiNoiseFloorF;
    }
    thr2_f_ = 0.5f * thr1_f_;
}

PanTompkinsDetector::CandidatePeak PanTompkinsDetector::refine_peak(
    uint32_t cand_mwi_idx, uint32_t val_q, float val_f) const noexcept {
    CandidatePeak out{};
    out.mwi_idx = cand_mwi_idx;
    out.refined_idx = cand_mwi_idx;
    out.val_q = val_q;
    out.val_f = val_f;

    if (bp_count_ < 3U) {
        return out;
    }

    // Search backwards across the MWI integration span [cand_mwi_idx - mwi_len_ - 4 .. cand_mwi_idx]
    // for the true bandpass QRS extremum |y_bp[k]|.
    const uint32_t search_span = static_cast<uint32_t>(mwi_len_) + 4U;
    const uint32_t min_idx = (cand_mwi_idx > search_span) ? (cand_mwi_idx - search_span) : 0U;

    std::size_t best_pos_idx = 0U;
    std::size_t best_neg_idx = 0U;
    bool found_pos = false;
    bool found_neg = false;
    int32_t max_pos_q = 0;
    int32_t max_neg_q = 0;
    float max_pos_f = 0.0f;
    float max_neg_f = 0.0f;

    const std::size_t inspect_n = (bp_count_ < kBpHistSize) ? bp_count_ : kBpHistSize;
    // Leave 1 sample margin on both ends so k-1 and k+1 exist in history for parabolic interp
    for (std::size_t age = 1U; age + 1U < inspect_n; ++age) {
        const std::size_t pos = (bp_head_ + kBpHistSize - 1U - age) & kBpHistMask;
        const uint32_t idx = bp_idx_hist_[pos];
        if (idx > cand_mwi_idx || idx < min_idx) {
            continue;
        }
        if (cfg_.math_mode == MathMode::Q15) {
            const int32_t v = static_cast<int32_t>(bp_hist_q_[pos]);
            if (v > 0 && (!found_pos || v > max_pos_q)) {
                max_pos_q = v;
                best_pos_idx = pos;
                found_pos = true;
            } else if (v < 0 && (!found_neg || (-v) > max_neg_q)) {
                max_neg_q = -v;
                best_neg_idx = pos;
                found_neg = true;
            }
        } else {
            const float v = bp_hist_f_[pos];
            if (v > 0.0f && (!found_pos || v > max_pos_f)) {
                max_pos_f = v;
                best_pos_idx = pos;
                found_pos = true;
            } else if (v < 0.0f && (!found_neg || (-v) > max_neg_f)) {
                max_neg_f = -v;
                best_neg_idx = pos;
                found_neg = true;
            }
        }
    }

    if (!found_pos && !found_neg) {
        return out;
    }

    // Lock consistently onto the same QRS lobe polarity to prevent +/-35 ms phase-dependent
    // jumps between positive R-peak and negative S-undershoot after bandpass filtering.
    bool pick_pos = found_pos;
    if (found_pos && found_neg) {
        if (locked_polarity_ > 0) {
            // Stay on positive lobe unless negative lobe dominates > 1.6x (e.g., reversed leads)
            const bool neg_dominant = (cfg_.math_mode == MathMode::Q15)
                                          ? ((max_neg_q * 10) > (max_pos_q * 16))
                                          : (max_neg_f > (1.6f * max_pos_f));
            pick_pos = !neg_dominant;
        } else if (locked_polarity_ < 0) {
            const bool pos_dominant = (cfg_.math_mode == MathMode::Q15)
                                          ? ((max_pos_q * 10) > (max_neg_q * 16))
                                          : (max_pos_f > (1.6f * max_neg_f));
            pick_pos = pos_dominant;
        } else {
            pick_pos = (cfg_.math_mode == MathMode::Q15) ? (max_pos_q >= max_neg_q)
                                                         : (max_pos_f >= max_neg_f);
        }
    }

    const std::size_t best_ring_pos = pick_pos ? best_pos_idx : best_neg_idx;
    out.polarity = pick_pos ? 1 : -1;
    out.refined_idx = bp_idx_hist_[best_ring_pos];

    if (cfg_.interp_enable) {
        const std::size_t prev_pos = (best_ring_pos + kBpHistSize - 1U) & kBpHistMask;
        const std::size_t next_pos = (best_ring_pos + 1U) & kBpHistMask;
        if (bp_idx_hist_[prev_pos] + 1U == out.refined_idx &&
            bp_idx_hist_[next_pos] == out.refined_idx + 1U) {
            const ParabolicInterpResult interp =
                (cfg_.math_mode == MathMode::Q15)
                    ? parabolic_interp_q15(bp_hist_q_[prev_pos], bp_hist_q_[best_ring_pos],
                                           bp_hist_q_[next_pos])
                    : parabolic_interp_float(bp_hist_f_[prev_pos], bp_hist_f_[best_ring_pos],
                                             bp_hist_f_[next_pos]);
            out.offset_q15 = interp.sub_sample_offset_q15;
            out.degenerate = interp.degenerate;
        }
    }

    return out;
}

uint32_t PanTompkinsDetector::average_rr_samples() const noexcept {
    if (rr_hist_count_ == 0U) {
        return 0U;
    }
    uint64_t sum = 0U;
    for (std::size_t i = 0U; i < rr_hist_count_; ++i) {
        sum += static_cast<uint64_t>(rr_samples_hist_[i]);
    }
    return static_cast<uint32_t>(sum / static_cast<uint64_t>(rr_hist_count_));
}

bool PanTompkinsDetector::commit_peak(const CandidatePeak& cand,
                                      bool used_search_back,
                                      DetectedRPeak& out_peak) noexcept {
    // Enforce 200 ms refractory period on refined peak index too (R-DSP-03)
    if (has_last_peak_) {
        if (cand.refined_idx <= last_refined_peak_idx_) {
            return false;
        }
        const uint32_t dt_refined = cand.refined_idx - last_refined_peak_idx_;
        if (dt_refined < refractory_samples_) {
            return false;
        }
    }

    out_peak.peak_sample_idx = cand.refined_idx;
    out_peak.sub_sample_offset_q15 = cand.offset_q15;
    out_peak.used_search_back = used_search_back;
    out_peak.degenerate_peak = cand.degenerate;

    if (has_last_peak_) {
        const uint32_t dt_samples = cand.refined_idx - last_refined_peak_idx_;
        const int64_t ts_us = (cfg_.fs_hz == kFs500Hz) ? 2000 : 4000;
        const int64_t dt_us =
            (static_cast<int64_t>(dt_samples) * ts_us) +
            static_cast<int64_t>(offset_q15_to_us(cand.offset_q15, cfg_.fs_hz)) -
            static_cast<int64_t>(offset_q15_to_us(last_offset_q15_, cfg_.fs_hz));

        if (dt_us > 0) {
            out_peak.rr_interval_us = static_cast<uint32_t>(dt_us);
            out_peak.has_rr = true;

            rr_samples_hist_[rr_hist_pos_] = dt_samples;
            rr_hist_pos_ = (rr_hist_pos_ + 1U) % kRrHistLen;
            if (rr_hist_count_ < kRrHistLen) {
                ++rr_hist_count_;
            }
        } else {
            out_peak.rr_interval_us = 0U;
            out_peak.has_rr = false;
        }
    } else {
        out_peak.rr_interval_us = 0U;
        out_peak.has_rr = false;
    }

    has_last_peak_ = true;
    last_mwi_peak_idx_ = cand.mwi_idx;
    last_refined_peak_idx_ = cand.refined_idx;
    last_offset_q15_ = cand.offset_q15;
    locked_polarity_ = cand.polarity;
    return true;
}

DspSampleOutput PanTompkinsDetector::process_filtered(int16_t filtered_q15,
                                                      float filtered_float,
                                                      uint32_t sample_idx) noexcept {
    DspSampleOutput out{};
    out.filtered_q15 = filtered_q15;

    // Store filtered sample in history ring for fine peak refinement & parabolic interpolation
    bp_hist_q_[bp_head_] = filtered_q15;
    bp_hist_f_[bp_head_] = filtered_float;
    bp_idx_hist_[bp_head_] = sample_idx;
    bp_head_ = (bp_head_ + 1U) & kBpHistMask;
    if (bp_count_ < kBpHistSize) {
        ++bp_count_;
    }

    ++samples_seen_;
    const bool blanked = (blanking_remaining_ > 0U);
    if (blanked) {
        --blanking_remaining_;
    }

    // 1. 5-point derivative -> 2. Squaring -> 3. Moving-Window Integration (150 ms)
    uint32_t mwi_curr_q = 0U;
    float mwi_curr_f = 0.0f;

    if (cfg_.math_mode == MathMode::Q15) {
        deriv_q_[4] = deriv_q_[3];
        deriv_q_[3] = deriv_q_[2];
        deriv_q_[2] = deriv_q_[1];
        deriv_q_[1] = deriv_q_[0];
        deriv_q_[0] = filtered_q15;

        int32_t d = 0;
        if (samples_seen_ >= 5U) {
            d = ((2 * static_cast<int32_t>(deriv_q_[0])) + static_cast<int32_t>(deriv_q_[1]) -
                 static_cast<int32_t>(deriv_q_[3]) - (2 * static_cast<int32_t>(deriv_q_[4]))) >>
                3;
        }
        const uint32_t sq = static_cast<uint32_t>(static_cast<int64_t>(d) * static_cast<int64_t>(d));

        mwi_sum_q_ -= static_cast<uint64_t>(mwi_buf_q_[mwi_pos_]);
        mwi_buf_q_[mwi_pos_] = sq;
        mwi_sum_q_ += static_cast<uint64_t>(sq);
        mwi_pos_ = (mwi_pos_ + 1U) % mwi_len_;

        mwi_curr_q = static_cast<uint32_t>(mwi_sum_q_ / static_cast<uint64_t>(mwi_len_));
    } else {
        deriv_f_[4] = deriv_f_[3];
        deriv_f_[3] = deriv_f_[2];
        deriv_f_[2] = deriv_f_[1];
        deriv_f_[1] = deriv_f_[0];
        deriv_f_[0] = filtered_float;

        float d = 0.0f;
        if (samples_seen_ >= 5U) {
            d = 0.125f * ((2.0f * deriv_f_[0]) + deriv_f_[1] - deriv_f_[3] - (2.0f * deriv_f_[4]));
        }
        const float sq = d * d;

        mwi_sum_f_ -= mwi_buf_f_[mwi_pos_];
        mwi_buf_f_[mwi_pos_] = sq;
        mwi_sum_f_ += sq;
        mwi_pos_ = (mwi_pos_ + 1U) % mwi_len_;
        if (mwi_pos_ == 0U) {
            // Periodically re-sum on wraparound to eliminate long-term floating-point drift
            float exact_sum = 0.0f;
            for (std::size_t i = 0U; i < mwi_len_; ++i) {
                exact_sum += mwi_buf_f_[i];
            }
            mwi_sum_f_ = exact_sum;
        }
        if (mwi_sum_f_ < 0.0f) {
            mwi_sum_f_ = 0.0f;
        }
        mwi_curr_f = mwi_sum_f_ / static_cast<float>(mwi_len_);
    }

    // 4. Local MWI peak detection on (mwi_prev2, mwi_prev1, mwi_curr) after 250 ms IIR settle
    bool is_local_peak = false;
    if (samples_seen_ > settle_samples_) {
        const uint32_t min_q = thresh_init_ ? kMinMwiNoiseFloorQ : init_floor_q_;
        const float min_f = thresh_init_ ? kMinMwiNoiseFloorF : init_floor_f_;
        if (cfg_.math_mode == MathMode::Q15) {
            is_local_peak = (mwi_prev1_q_ > mwi_prev2_q_) && (mwi_prev1_q_ >= mwi_curr_q) &&
                            (mwi_prev1_q_ >= min_q);
        } else {
            is_local_peak = (mwi_prev1_f_ > mwi_prev2_f_) && (mwi_prev1_f_ >= mwi_curr_f) &&
                            (mwi_prev1_f_ >= min_f);
        }
    }

    const uint32_t cand_mwi_idx = (sample_idx > 0U) ? (sample_idx - 1U) : 0U;

    if (is_local_peak) {
        const bool in_refractory =
            has_last_peak_ && (cand_mwi_idx > last_mwi_peak_idx_) &&
            ((cand_mwi_idx - last_mwi_peak_idx_) < refractory_samples_);

        if (!in_refractory) {
            const bool above_thr1 = (cfg_.math_mode == MathMode::Q15)
                                        ? (!thresh_init_ || mwi_prev1_q_ >= thr1_q_)
                                        : (!thresh_init_ || mwi_prev1_f_ >= thr1_f_);
            const bool above_thr2 = (cfg_.math_mode == MathMode::Q15)
                                        ? (thresh_init_ && mwi_prev1_q_ >= thr2_q_)
                                        : (thresh_init_ && mwi_prev1_f_ >= thr2_f_);

            if (above_thr1) {
                if (cfg_.math_mode == MathMode::Q15) {
                    update_thresholds_q(mwi_prev1_q_, true, false);
                } else {
                    update_thresholds_f(mwi_prev1_f_, true, false);
                }

                if (!blanked) {
                    const CandidatePeak cand = refine_peak(cand_mwi_idx, mwi_prev1_q_, mwi_prev1_f_);
                    if (commit_peak(cand, false, out.r_peak)) {
                        out.r_peak_detected = true;
                        has_sb_cand_ = false;
                    }
                }
            } else {
                // Noise peak or search-back candidate
                if (cfg_.math_mode == MathMode::Q15) {
                    update_thresholds_q(mwi_prev1_q_, false, false);
                } else {
                    update_thresholds_f(mwi_prev1_f_, false, false);
                }

                if (!blanked && above_thr2) {
                    const bool stronger =
                        !has_sb_cand_ ||
                        ((cfg_.math_mode == MathMode::Q15) ? (mwi_prev1_q_ > sb_cand_.val_q)
                                                           : (mwi_prev1_f_ > sb_cand_.val_f));
                    if (stronger) {
                        sb_cand_ = refine_peak(cand_mwi_idx, mwi_prev1_q_, mwi_prev1_f_);
                        has_sb_cand_ = true;
                    }
                }
            }
        }
    }

    // 5. Search-back trigger: if elapsed > 166% of 8-beat average RR (OPEN-10b)
    if (!blanked && !out.r_peak_detected && has_last_peak_ && has_sb_cand_) {
        const uint32_t avg_rr = average_rr_samples();
        if (avg_rr > 0U && sample_idx > last_mwi_peak_idx_) {
            const uint32_t elapsed = sample_idx - last_mwi_peak_idx_;
            const uint32_t sb_limit = static_cast<uint32_t>(
                (static_cast<uint64_t>(avg_rr) * 166ULL) / 100ULL);
            const bool sb_above_thr2 = (cfg_.math_mode == MathMode::Q15)
                                           ? (sb_cand_.val_q >= thr2_q_)
                                           : (sb_cand_.val_f >= thr2_f_);
            if (elapsed > sb_limit && sb_above_thr2) {
                if (commit_peak(sb_cand_, true, out.r_peak)) {
                    if (cfg_.math_mode == MathMode::Q15) {
                        update_thresholds_q(sb_cand_.val_q, true, true);
                    } else {
                        update_thresholds_f(sb_cand_.val_f, true, true);
                    }
                    out.r_peak_detected = true;
                    has_sb_cand_ = false;
                }
            }
        }
    }

    mwi_prev2_q_ = mwi_prev1_q_;
    mwi_prev1_q_ = mwi_curr_q;
    mwi_prev2_f_ = mwi_prev1_f_;
    mwi_prev1_f_ = mwi_curr_f;
    return out;
}

bool EcgDspCore::configure(const DspConfig& cfg) noexcept {
    if (!filter_.configure(cfg.fs_hz, cfg.math_mode)) {
        return false;
    }
    return detector_.configure(cfg);
}

void EcgDspCore::reset() noexcept {
    filter_.reset();
    detector_.reset();
}

void EcgDspCore::reset_with_gap_blanking() noexcept {
    filter_.reset();
    detector_.reset_with_gap_blanking();
}

DspSampleOutput EcgDspCore::process_trusted_adc_sample(int16_t raw_adc,
                                                       uint32_t sample_idx) noexcept {
    if (filter_.math_mode() == MathMode::Q15) {
        const int16_t x_q15 = EcgIirFilterChain::adc_raw_to_q15(raw_adc);
        const int16_t y_q15 = filter_.process_q15(x_q15);
        return detector_.process_filtered(y_q15, 0.0f, sample_idx);
    }

    const float x_f = EcgIirFilterChain::adc_raw_to_float(raw_adc);
    const float y_f = filter_.process_float(x_f);
    const float scaled = y_f * 32768.0f;
    int16_t y_q15 = 0;
    if (scaled >= 32767.0f) {
        y_q15 = 32767;
    } else if (scaled <= -32768.0f) {
        y_q15 = -32768;
    } else {
        y_q15 = static_cast<int16_t>(scaled >= 0.0f ? (scaled + 0.5f) : (scaled - 0.5f));
    }
    return detector_.process_filtered(y_q15, y_f, sample_idx);
}

Result<DspBatchOutput> EcgDspCore::process_hil_batch(
    const Result<TrustedSampleBatch>& batch_res) noexcept {
    if (batch_res.is_err()) {
        // OPEN-15: SeqGap resets both IIR & Pan-Tompkins and blocks peak detection for 2 s
        if (batch_res.detail_code() == DetailCode::SeqGap) {
            reset_with_gap_blanking();
        } else if (batch_res.canonical_state() == CanonicalState::SensorUnavailable ||
                   batch_res.canonical_state() == CanonicalState::Stale) {
            detector_.break_rr_continuity();
        }
        return Result<DspBatchOutput>::fail(batch_res.status());
    }

    // Duplicate frames are ignored without feeding DSP again (PROPOSED-PROTO-04)
    if (batch_res.detail_code() == DetailCode::DuplicateIgnored) {
        DspBatchOutput empty{};
        return Result<DspBatchOutput>::ok(empty, DetailCode::DuplicateIgnored);
    }

    const TrustedSampleBatch& batch = batch_res.value();
    if (batch.fs_hz != filter_.fs_hz()) {
        return Result<DspBatchOutput>::fail(CanonicalState::Malformed, DetailCode::SemanticRange);
    }

    DspBatchOutput out{};
    out.sample_count = batch.sample_count;
    for (std::size_t i = 0U; i < static_cast<std::size_t>(batch.sample_count); ++i) {
        const uint32_t idx = batch.first_sample_idx + static_cast<uint32_t>(i);
        const DspSampleOutput s_out = process_trusted_adc_sample(batch.samples[i], idx);
        if (s_out.r_peak_detected && out.r_peak_count < kMaxPeaksPerBatch) {
            out.peaks[out.r_peak_count] = s_out.r_peak;
            ++out.r_peak_count;
        }
    }
    return Result<DspBatchOutput>::ok(out);
}

FailureStatus EcgDspCore::process_wearable_batch_into(const FailureStatus& batch_status,
                                                      const TrustedWearableBatch& batch,
                                                      uint32_t first_sample_idx,
                                                      DspBatchOutput& out) noexcept {
    if (batch_status.canonical != CanonicalState::Valid) {
        if (batch_status.detail == DetailCode::SeqGap ||
            batch_status.detail == DetailCode::PingPongOverflow ||
            batch_status.detail == DetailCode::StaleTimeout) {
            reset_with_gap_blanking();
        } else if (batch_status.canonical == CanonicalState::SensorUnavailable ||
                   batch_status.canonical == CanonicalState::Stale) {
            detector_.break_rr_continuity();
        }
        return batch_status;
    }

    if (batch.fs_hz != filter_.fs_hz()) {
        return FailureStatus{CanonicalState::Malformed, DetailCode::SemanticRange};
    }

    out.sample_count = batch.sample_count;
    out.r_peak_count = 0U;
    for (std::size_t i = 0U; i < static_cast<std::size_t>(batch.sample_count); ++i) {
        const uint32_t idx = first_sample_idx + static_cast<uint32_t>(i);
        const DspSampleOutput s_out = process_trusted_adc_sample(batch.samples[i], idx);
        if (s_out.r_peak_detected && out.r_peak_count < kMaxPeaksPerBatch) {
            out.peaks[out.r_peak_count] = s_out.r_peak;
            ++out.r_peak_count;
        }
    }
    return FailureStatus{CanonicalState::Valid, DetailCode::Ok};
}

Result<DspBatchOutput> EcgDspCore::process_wearable_batch(
    const Result<TrustedWearableBatch>& batch_res, uint32_t first_sample_idx) noexcept {
    if (batch_res.is_err()) {
        TrustedWearableBatch dummy{};
        DspBatchOutput unused{};
        return Result<DspBatchOutput>::fail(
            process_wearable_batch_into(batch_res.status(), dummy, first_sample_idx, unused));
    }

    DspBatchOutput out{};
    const FailureStatus st =
        process_wearable_batch_into(batch_res.status(), batch_res.value(), first_sample_idx, out);
    if (st.canonical != CanonicalState::Valid) {
        return Result<DspBatchOutput>::fail(st);
    }
    return Result<DspBatchOutput>::ok(out);
}

}  // namespace p05
