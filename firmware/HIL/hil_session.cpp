#include "hil_session.hpp"

namespace p05 {

namespace {

// Safely extracts SEQ from raw wire header for NACK diagnostics only (never trusted for logic).
uint16_t extract_raw_seq_best_effort(ConstByteView raw_frame) noexcept {
    if (raw_frame.valid_pointer() && raw_frame.length >= 6U) {
        return read_u16_le(&raw_frame.data[4]);
    }
    return 0U;
}

}  // namespace

HilSession::HilSession(CycleCounterFn cycle_fn, void* cycle_ctx) noexcept
    : cycle_fn_(cycle_fn), cycle_ctx_(cycle_ctx) {}

void HilSession::set_cycle_counter(CycleCounterFn cycle_fn, void* cycle_ctx) noexcept {
    cycle_fn_ = cycle_fn;
    cycle_ctx_ = cycle_ctx;
}

uint32_t HilSession::read_cycles() const noexcept {
    if (cycle_fn_ != nullptr) {
        return cycle_fn_(cycle_ctx_);
    }
    return 0U;
}

void HilSession::reset_processing_state() noexcept {
    dsp_.reset();
    hrv_.reset();
    cycles_.reset();
    has_cached_ack_ = false;
    cached_ack_seq_ = 0U;
    cached_peak_count_ = 0U;
    cached_hrv_ =
        Result<TrustedHrvMetrics>::fail(CanonicalState::SensorUnavailable, DetailCode::LeadsOff);
}

EncodedHilFrame HilSession::encode_error_nack(uint16_t rejected_seq,
                                              FailureStatus status) noexcept {
    EncodedHilFrame f{};
    // Header (8 bytes)
    f.bytes[0] = kHilSync0;
    f.bytes[1] = kHilSync1;
    f.bytes[2] = kHilVersion;
    f.bytes[3] = static_cast<uint8_t>(HilMsgType::ErrorNack);
    write_u16_le(&f.bytes[4], rejected_seq);
    write_u16_le(&f.bytes[6], 4U);  // PAYLOAD_LEN = 4 bytes

    // Payload (4 bytes, PROTOCOL.md section 2.3)
    write_u16_le(&f.bytes[8], rejected_seq);
    f.bytes[10] = static_cast<uint8_t>(status.canonical);
    f.bytes[11] = static_cast<uint8_t>(status.detail);

    // CRC-16/CCITT-FALSE over bytes 0..11
    const uint16_t crc = crc16_ccitt_false(f.bytes.data(), 12U);
    write_u16_le(&f.bytes[12], crc);
    f.length = 14U;
    return f;
}

EncodedHilFrame HilSession::encode_result_ack(
    uint16_t ack_seq,
    FailureStatus status,
    const CycleStats& stats,
    const std::array<DetectedRPeak, kMaxPeaksPerBatch>& peaks,
    uint8_t peak_count,
    const Result<TrustedHrvMetrics>& hrv_res) noexcept {
    EncodedHilFrame f{};
    const uint8_t safe_peaks =
        (peak_count <= kMaxPeaksPerBatch) ? peak_count : static_cast<uint8_t>(kMaxPeaksPerBatch);
    const uint16_t payload_len =
        static_cast<uint16_t>(34U + (10U * static_cast<uint16_t>(safe_peaks)));

    // Header (8 bytes)
    f.bytes[0] = kHilSync0;
    f.bytes[1] = kHilSync1;
    f.bytes[2] = kHilVersion;
    f.bytes[3] = static_cast<uint8_t>(HilMsgType::ResultAck);
    write_u16_le(&f.bytes[4], ack_seq);
    write_u16_le(&f.bytes[6], payload_len);

    // Payload (PROTOCOL.md section 2.3)
    std::size_t off = kHilHeaderBytes;
    write_u16_le(&f.bytes[off], ack_seq);
    off += 2U;
    f.bytes[off++] = static_cast<uint8_t>(status.canonical);
    f.bytes[off++] = static_cast<uint8_t>(status.detail);
    write_u32_le(&f.bytes[off], stats.min_cycles);
    off += 4U;
    write_u32_le(&f.bytes[off], stats.mean_cycles);
    off += 4U;
    write_u32_le(&f.bytes[off], stats.max_cycles);
    off += 4U;
    f.bytes[off++] = safe_peaks;

    for (std::size_t i = 0U; i < static_cast<std::size_t>(safe_peaks); ++i) {
        write_u32_le(&f.bytes[off], peaks[i].peak_sample_idx);
        off += 4U;
        write_i16_le(&f.bytes[off], peaks[i].sub_sample_offset_q15);
        off += 2U;
        write_u32_le(&f.bytes[off], peaks[i].has_rr ? peaks[i].rr_interval_us : 0U);
        off += 4U;
    }

    if (hrv_res.is_ok()) {
        const TrustedHrvMetrics& m = hrv_res.value();
        f.bytes[off++] = 1U;  // hrv_valid = 1
        write_u32_le(&f.bytes[off], m.mean_rr_us);
        off += 4U;
        write_u16_le(&f.bytes[off], m.hr_bpm_x100);
        off += 2U;
        write_u32_le(&f.bytes[off], m.sdnn_us);
        off += 4U;
        write_u32_le(&f.bytes[off], m.rmssd_us);
        off += 4U;
        write_u16_le(&f.bytes[off], m.pnn50_permille);
        off += 2U;
    } else {
        // Explicit hrv_valid = 0 guarantees zero placeholders are never mistaken for valid HRV (R2)
        f.bytes[off++] = 0U;
        for (std::size_t z = 0U; z < 16U; ++z) {
            f.bytes[off++] = 0U;
        }
    }

    const uint16_t crc = crc16_ccitt_false(f.bytes.data(), off);
    write_u16_le(&f.bytes[off], crc);
    off += kHilCrcBytes;
    f.length = off;
    return f;
}

EncodedHilFrame HilSession::handle_frame(ConstByteView raw_frame,
                                         uint32_t uptime_ms,
                                         const HilSafetyInputs& safety) noexcept {
    const Result<TrustedHilCommand> parsed =
        validator_.validate_incoming_frame(raw_frame, uptime_ms, safety);

    if (parsed.is_err()) {
        const uint16_t rej_seq = extract_raw_seq_best_effort(raw_frame);
        if (parsed.detail_code() == DetailCode::SeqGap) {
            // OPEN-15: gap resets IIR & Pan-Tompkins with 2 s blanking and breaks HRV continuity
            dsp_.reset_with_gap_blanking();
            hrv_.on_interruption(parsed.status());
        } else if (!validator_.hil_session_active()) {
            // Session aborted (3 consecutive malformed frames, timeout, or protection lockout)
            reset_processing_state();
        }
        return encode_error_nack(rej_seq, parsed.status());
    }

    const TrustedHilCommand& cmd = parsed.value();

    // PROPOSED-PROTO-04: duplicate SEQ replays prior ACK without mutating DSP/HRV state
    if (parsed.detail_code() == DetailCode::DuplicateIgnored && has_cached_ack_) {
        return encode_result_ack(
            cached_ack_seq_,
            FailureStatus{CanonicalState::Valid, DetailCode::DuplicateIgnored},
            cached_cycles_,
            cached_peaks_,
            cached_peak_count_,
            cached_hrv_);
    }

    if (cmd.msg_type == HilMsgType::ConfigReq) {
        const DspConfig new_cfg{
            cmd.config.fs_hz, cmd.config.math_mode, cmd.config.interp_enable};
        const bool mode_changed =
            (new_cfg.fs_hz != dsp_.config().fs_hz) ||
            (new_cfg.math_mode != dsp_.config().math_mode) ||
            (new_cfg.interp_enable != dsp_.config().interp_enable);

        if (cmd.config.reset_state || mode_changed) {
            (void)dsp_.configure(new_cfg);
            hrv_.configure(new_cfg.math_mode);
            cycles_.reset();
        }

        cached_ack_seq_ = cmd.seq;
        cached_cycles_ = cycles_;
        cached_peak_count_ = 0U;
        cached_hrv_ = hrv_.compute_metrics();
        has_cached_ack_ = true;

        return encode_result_ack(
            cmd.seq,
            FailureStatus{CanonicalState::Valid, DetailCode::Ok},
            cached_cycles_,
            cached_peaks_,
            cached_peak_count_,
            cached_hrv_);
    }

    // HIL_SAMPLES_IN: run per-sample DSP + DWT cycle timing + online HRV update
    const TrustedSampleBatch& batch = cmd.batch;
    std::array<DetectedRPeak, kMaxPeaksPerBatch> batch_peaks{};
    uint8_t batch_peak_count = 0U;

    for (std::size_t i = 0U; i < static_cast<std::size_t>(batch.sample_count); ++i) {
        const uint32_t sample_idx = batch.first_sample_idx + static_cast<uint32_t>(i);
        const uint32_t c_start = read_cycles();
        const DspSampleOutput s_out = dsp_.process_trusted_adc_sample(batch.samples[i], sample_idx);
        const uint32_t c_end = read_cycles();
        cycles_.record(c_end - c_start);

        if (s_out.r_peak_detected) {
            (void)hrv_.push_r_peak(s_out.r_peak);
            if (batch_peak_count < kMaxPeaksPerBatch) {
                batch_peaks[batch_peak_count] = s_out.r_peak;
                ++batch_peak_count;
            }
        }
    }

    const Result<TrustedHrvMetrics> hrv_res = hrv_.compute_metrics();

    cached_ack_seq_ = cmd.seq;
    cached_cycles_ = cycles_;
    cached_peaks_ = batch_peaks;
    cached_peak_count_ = batch_peak_count;
    cached_hrv_ = hrv_res;
    has_cached_ack_ = true;

    return encode_result_ack(
        cmd.seq,
        FailureStatus{CanonicalState::Valid, DetailCode::Ok},
        cached_cycles_,
        cached_peaks_,
        cached_peak_count_,
        cached_hrv_);
}

FailureStatus HilSession::poll_timeout(uint32_t uptime_ms) noexcept {
    const FailureStatus st = validator_.check_timeout(uptime_ms);
    if (st.canonical == CanonicalState::CommsFault) {
        // OPEN-3: UART1 timeout closes HIL session and resets DSP state
        reset_processing_state();
    }
    return st;
}

FailureStatus HilSession::on_uart_fault() noexcept {
    // OPEN-3 & R-BND-03: close session & reset DSP, NEVER clear latched safety lockout
    const FailureStatus st = validator_.on_comms_fault();
    reset_processing_state();
    return st;
}

bool HilSession::try_clear_lockout(const HilSafetyInputs& safety) noexcept {
    return validator_.try_clear_lockout(safety);
}

}  // namespace p05
