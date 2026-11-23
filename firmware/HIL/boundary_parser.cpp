#include "boundary_parser.hpp"

namespace p05 {

namespace {

constexpr bool is_valid_fs(uint16_t fs_hz) noexcept {
    return (fs_hz == kFs250Hz) || (fs_hz == kFs500Hz);
}

constexpr bool is_binary_flag(uint8_t val) noexcept {
    return (val == 0U) || (val == 1U);
}

}  // namespace

Result<TrustedHilCommand> parse_hil_frame_stateless(ConstByteView raw_frame,
                                                    uint16_t active_fs_hz) noexcept {
    // 1. Length & pointer check FIRST (RULES section 4)
    if (!raw_frame.valid_pointer() || raw_frame.length < kHilMinFrameBytes ||
        raw_frame.length > kHilMaxFrameBytes) {
        return Result<TrustedHilCommand>::fail(CanonicalState::Malformed, DetailCode::BadLength);
    }

    const uint8_t* const buf = raw_frame.data;
    const uint16_t payload_len = read_u16_le(&buf[6]);
    const std::size_t expected_total =
        kHilHeaderBytes + static_cast<std::size_t>(payload_len) + kHilCrcBytes;
    if (payload_len > kHilMaxPayloadBytes || expected_total != raw_frame.length) {
        return Result<TrustedHilCommand>::fail(CanonicalState::Malformed, DetailCode::BadLength);
    }

    // 2. Sync check
    if (buf[0] != kHilSync0 || buf[1] != kHilSync1) {
        return Result<TrustedHilCommand>::fail(CanonicalState::Malformed, DetailCode::BadSync);
    }

    // 3. Version check
    if (buf[2] != kHilVersion) {
        return Result<TrustedHilCommand>::fail(CanonicalState::Malformed, DetailCode::BadVersion);
    }

    // 4. CRC-16/CCITT-FALSE check BEFORE any payload values are decoded (R-BND-01 / R1)
    const std::size_t crc_offset = kHilHeaderBytes + static_cast<std::size_t>(payload_len);
    const uint16_t wire_crc = read_u16_le(&buf[crc_offset]);
    const uint16_t calc_crc = crc16_ccitt_false(buf, crc_offset);
    if (wire_crc != calc_crc) {
        return Result<TrustedHilCommand>::fail(CanonicalState::Malformed, DetailCode::BadCrc);
    }

    // 5. Explicit Little-Endian field-by-field decode + semantic range checks
    const uint8_t raw_type = buf[3];
    const uint16_t seq = read_u16_le(&buf[4]);
    const uint8_t* const payload = &buf[kHilHeaderBytes];

    if (raw_type == static_cast<uint8_t>(HilMsgType::ConfigReq)) {
        if (payload_len != 5U) {
            return Result<TrustedHilCommand>::fail(CanonicalState::Malformed, DetailCode::BadLength);
        }

        const uint16_t fs_hz = read_u16_le(&payload[0]);
        const uint8_t raw_math = payload[2];
        const uint8_t raw_interp = payload[3];
        const uint8_t raw_reset = payload[4];

        if (!is_valid_fs(fs_hz) || !is_binary_flag(raw_math) || !is_binary_flag(raw_interp) ||
            !is_binary_flag(raw_reset)) {
            return Result<TrustedHilCommand>::fail(CanonicalState::Malformed,
                                                   DetailCode::SemanticRange);
        }

        TrustedHilCommand cmd{};
        cmd.msg_type = HilMsgType::ConfigReq;
        cmd.seq = seq;
        cmd.config.seq = seq;
        cmd.config.fs_hz = fs_hz;
        cmd.config.math_mode = (raw_math == 0U) ? MathMode::Q15 : MathMode::SoftFloat;
        cmd.config.interp_enable = (raw_interp == 1U);
        cmd.config.reset_state = (raw_reset == 1U);
        return Result<TrustedHilCommand>::ok(cmd);
    }

    if (raw_type == static_cast<uint8_t>(HilMsgType::SamplesIn)) {
        if (payload_len < 5U) {
            return Result<TrustedHilCommand>::fail(CanonicalState::Malformed, DetailCode::BadLength);
        }

        const uint32_t first_sample_idx = read_u32_le(&payload[0]);
        const uint8_t sample_count = payload[4];
        if (sample_count == 0U || sample_count > kMaxBatchSamples) {
            return Result<TrustedHilCommand>::fail(CanonicalState::Malformed,
                                                   DetailCode::SemanticRange);
        }

        const std::size_t expected_payload = 5U + (2U * static_cast<std::size_t>(sample_count));
        if (static_cast<std::size_t>(payload_len) != expected_payload) {
            return Result<TrustedHilCommand>::fail(CanonicalState::Malformed, DetailCode::BadLength);
        }

        if (!is_valid_fs(active_fs_hz)) {
            return Result<TrustedHilCommand>::fail(CanonicalState::Malformed,
                                                   DetailCode::SemanticRange);
        }

        TrustedHilCommand cmd{};
        cmd.msg_type = HilMsgType::SamplesIn;
        cmd.seq = seq;
        cmd.batch.seq = seq;
        cmd.batch.first_sample_idx = first_sample_idx;
        cmd.batch.fs_hz = active_fs_hz;
        cmd.batch.sample_count = sample_count;

        for (std::size_t i = 0U; i < static_cast<std::size_t>(sample_count); ++i) {
            const int16_t raw_sample = read_i16_le(&payload[5U + (2U * i)]);
            if (raw_sample < static_cast<int16_t>(kAdcRawMin) ||
                raw_sample > static_cast<int16_t>(kAdcRawMax)) {
                return Result<TrustedHilCommand>::fail(CanonicalState::Malformed,
                                                       DetailCode::SemanticRange);
            }
            cmd.batch.samples[i] = raw_sample;
        }
        return Result<TrustedHilCommand>::ok(cmd);
    }

    return Result<TrustedHilCommand>::fail(CanonicalState::Malformed, DetailCode::SemanticRange);
}

Result<TrustedHilCommand> HilBoundaryValidator::validate_incoming_frame(
    ConstByteView raw_frame, uint32_t uptime_ms, const HilSafetyInputs& safety) noexcept {
    // 1. Hardware safety & protection latch check FIRST (R-BND-03 / OPEN-2 / OPEN-9a).
    if (!safety.protection_status_known) {
        if (!lockout_latched_) {
            lockout_detail_ = DetailCode::ProtectionUnknown;
        }
        lockout_latched_ = true;
        close_session_keep_lockout();
        return Result<TrustedHilCommand>::fail(CanonicalState::ProtectionStatusUnknown,
                                               lockout_detail_);
    }

    if (!safety.electrodes_detached) {
        // Electrodes still attached while UART1 HIL traffic arrived -> latch mode conflict lockout.
        if (!lockout_latched_) {
            lockout_detail_ = DetailCode::ModeConflictLockout;
        }
        lockout_latched_ = true;
        close_session_keep_lockout();
        return Result<TrustedHilCommand>::fail(CanonicalState::ProtectionStatusUnknown,
                                               lockout_detail_);
    }

    if (lockout_latched_) {
        // Once latched, lockout preserves original reason until explicit try_clear_lockout().
        close_session_keep_lockout();
        return Result<TrustedHilCommand>::fail(CanonicalState::ProtectionStatusUnknown,
                                               lockout_detail_);
    }

    // 2. Check staleness on an already-active session against explicit uptime clock (OPEN-5).
    if (session_active_) {
        const uint32_t elapsed_ms = uptime_ms - last_valid_rx_ms_;
        if (elapsed_ms > kUartTimeoutMs) {
            close_session_keep_lockout();
            return Result<TrustedHilCommand>::fail(CanonicalState::Stale, DetailCode::StaleTimeout);
        }
    }

    // 3. Stateless wire validation (length -> sync -> version -> CRC -> endianness -> semantic range)
    const Result<TrustedHilCommand> parsed = parse_hil_frame_stateless(raw_frame, active_fs_hz_);
    if (parsed.is_err()) {
        if (parsed.canonical_state() == CanonicalState::Malformed) {
            if (consecutive_malformed_ < 255U) {
                ++consecutive_malformed_;
            }
            if (consecutive_malformed_ >= kMaxConsecutiveMalformed) {
                close_session_keep_lockout();
            }
        }
        return parsed;
    }

    const TrustedHilCommand& cmd = parsed.value();

    // 4. HIL session entry rule (OPEN-2): must enter via HIL_CONFIG_REQ before HIL_SAMPLES_IN.
    if (!session_active_ && cmd.msg_type != HilMsgType::ConfigReq) {
        if (consecutive_malformed_ < 255U) {
            ++consecutive_malformed_;
        }
        return Result<TrustedHilCommand>::fail(CanonicalState::Malformed, DetailCode::SemanticRange);
    }

    // 5. Handle HIL_CONFIG_REQ (starts or reconfigures session).
    if (cmd.msg_type == HilMsgType::ConfigReq) {
        if (session_active_ && has_last_seq_) {
            const int16_t seq_diff = static_cast<int16_t>(cmd.seq - last_valid_seq_);
            if (seq_diff == 0) {
                consecutive_malformed_ = 0U;
                last_valid_rx_ms_ = uptime_ms;
                return Result<TrustedHilCommand>::ok(cmd, DetailCode::DuplicateIgnored);
            }
            if (seq_diff < 0 && !cmd.config.reset_state) {
                if (consecutive_malformed_ < 255U) {
                    ++consecutive_malformed_;
                }
                if (consecutive_malformed_ >= kMaxConsecutiveMalformed) {
                    close_session_keep_lockout();
                }
                return Result<TrustedHilCommand>::fail(CanonicalState::Malformed,
                                                       DetailCode::SeqReplay);
            }
        }

        session_active_ = true;
        active_fs_hz_ = cmd.config.fs_hz;
        last_valid_seq_ = cmd.seq;
        has_last_seq_ = true;
        last_seq_was_valid_ = true;
        if (cmd.config.reset_state || !has_expected_sample_idx_) {
            has_expected_sample_idx_ = false;
            expected_next_sample_idx_ = 0U;
        }
        last_valid_rx_ms_ = uptime_ms;
        consecutive_malformed_ = 0U;
        return Result<TrustedHilCommand>::ok(cmd, DetailCode::Ok);
    }

    // 6. Sequence & sample-index continuity checks for HIL_SAMPLES_IN (OPEN-15 / R-BND-05).
    const int16_t seq_diff = static_cast<int16_t>(cmd.seq - last_valid_seq_);
    if (seq_diff == 0) {
        consecutive_malformed_ = 0U;
        last_valid_rx_ms_ = uptime_ms;
        if (!last_seq_was_valid_) {
            // Prior frame with this seq was rejected for a gap and never produced a valid ACK.
            return Result<TrustedHilCommand>::fail(CanonicalState::Stale, DetailCode::SeqGap);
        }
        return Result<TrustedHilCommand>::ok(cmd, DetailCode::DuplicateIgnored);
    }

    if (seq_diff < 0) {
        if (consecutive_malformed_ < 255U) {
            ++consecutive_malformed_;
        }
        if (consecutive_malformed_ >= kMaxConsecutiveMalformed) {
            close_session_keep_lockout();
        }
        return Result<TrustedHilCommand>::fail(CanonicalState::Malformed, DetailCode::SeqReplay);
    }

    // Valid wire frame resets malformed counter even if sequence has a gap.
    consecutive_malformed_ = 0U;
    last_valid_rx_ms_ = uptime_ms;

    const bool seq_gap = (seq_diff > 1);
    const bool sample_idx_gap =
        has_expected_sample_idx_ && (cmd.batch.first_sample_idx != expected_next_sample_idx_);

    last_valid_seq_ = cmd.seq;
    expected_next_sample_idx_ =
        cmd.batch.first_sample_idx + static_cast<uint32_t>(cmd.batch.sample_count);
    has_expected_sample_idx_ = true;

    if (seq_gap || sample_idx_gap) {
        last_seq_was_valid_ = false;
        return Result<TrustedHilCommand>::fail(CanonicalState::Stale, DetailCode::SeqGap);
    }

    last_seq_was_valid_ = true;
    return Result<TrustedHilCommand>::ok(cmd, DetailCode::Ok);
}

FailureStatus HilBoundaryValidator::check_timeout(uint32_t uptime_ms) noexcept {
    if (!session_active_) {
        if (lockout_latched_) {
            return FailureStatus{CanonicalState::ProtectionStatusUnknown, lockout_detail_};
        }
        return FailureStatus{CanonicalState::Valid, DetailCode::Ok};
    }

    const uint32_t elapsed_ms = uptime_ms - last_valid_rx_ms_;
    if (elapsed_ms > kUartTimeoutMs) {
        close_session_keep_lockout();
        return FailureStatus{CanonicalState::CommsFault, DetailCode::UartFault};
    }

    return FailureStatus{CanonicalState::Valid, DetailCode::Ok};
}

FailureStatus HilBoundaryValidator::on_comms_fault() noexcept {
    // R-BND-03: comms failure NEVER clears lockout_latched_.
    close_session_keep_lockout();
    return FailureStatus{CanonicalState::CommsFault, DetailCode::UartFault};
}

bool HilBoundaryValidator::try_clear_lockout(const HilSafetyInputs& safety) noexcept {
    if (safety.protection_status_known && safety.electrodes_detached) {
        lockout_latched_ = false;
        lockout_detail_ = DetailCode::Ok;
        return true;
    }
    return false;
}

void HilBoundaryValidator::close_session_keep_lockout() noexcept {
    session_active_ = false;
    has_last_seq_ = false;
    last_seq_was_valid_ = false;
    has_expected_sample_idx_ = false;
    expected_next_sample_idx_ = 0U;
}

}  // namespace p05
