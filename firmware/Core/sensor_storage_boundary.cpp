#include "sensor_storage_boundary.hpp"

namespace p05 {

namespace {

inline constexpr uint8_t kSdSync0 = 0x45U;  // 'E'
inline constexpr uint8_t kSdSync1 = 0x43U;  // 'C'
inline constexpr uint8_t kSdSync2 = 0x47U;  // 'G'
inline constexpr uint8_t kSdSync3 = 0x35U;  // '5'
inline constexpr uint8_t kSdVersion = 0x01U;

inline constexpr uint16_t kIna219MaxBusMv = 26000U;      // INA219 datasheet max bus voltage 26 V
inline constexpr int32_t kIna219MaxCurrentUa = 3200000;  // 320 mV / 0.1 Ohm = 3.2 A
inline constexpr uint16_t kVbatMaxPhysicalMv = 6600U;    // 3.3 V ADC full-scale x 2 divider

constexpr uint32_t adc_stale_threshold_ms(uint16_t fs_hz) noexcept {
    return (fs_hz == kFs500Hz) ? kAdcStale500HzMs : kAdcStale250HzMs;
}

Result<TrustedSdHeader> parse_and_validate_sd_envelope(ConstByteView raw_block,
                                                       SdBlockType expected_type) noexcept {
    // 1. Length & pointer check FIRST
    if (!raw_block.valid_pointer() || raw_block.length != kSdBlockBytes) {
        return Result<TrustedSdHeader>::fail(CanonicalState::Malformed, DetailCode::BadLength);
    }

    const uint8_t* const buf = raw_block.data;

    // 2. Sync ("ECG5")
    if (buf[0] != kSdSync0 || buf[1] != kSdSync1 || buf[2] != kSdSync2 || buf[3] != kSdSync3) {
        return Result<TrustedSdHeader>::fail(CanonicalState::Malformed, DetailCode::BadSync);
    }

    // 3. Version (0x01)
    if (buf[4] != kSdVersion) {
        return Result<TrustedSdHeader>::fail(CanonicalState::Malformed, DetailCode::BadVersion);
    }

    // 4. CRC-16/CCITT-FALSE over bytes 0..509 vs Little-Endian trailer bytes 510..511 (R-BND-01)
    const uint16_t wire_crc = read_u16_le(&buf[510]);
    const uint16_t calc_crc = crc16_ccitt_false(buf, 510U);
    if (wire_crc != calc_crc) {
        return Result<TrustedSdHeader>::fail(CanonicalState::Malformed, DetailCode::BadCrc);
    }

    // 5. Block type, reserved trailer, flag mask & header field-by-field Little-Endian decode
    const uint8_t raw_type = buf[5];
    if (raw_type != static_cast<uint8_t>(expected_type)) {
        return Result<TrustedSdHeader>::fail(CanonicalState::Malformed, DetailCode::SemanticRange);
    }

    if (read_u16_le(&buf[508]) != 0U) {
        return Result<TrustedSdHeader>::fail(CanonicalState::Malformed, DetailCode::SemanticRange);
    }

    const uint16_t raw_flags = read_u16_le(&buf[6]);
    if ((raw_flags & ~0x00FFU) != 0U) {
        return Result<TrustedSdHeader>::fail(CanonicalState::Malformed, DetailCode::SemanticRange);
    }

    TrustedSdHeader hdr{};
    hdr.version = buf[4];
    hdr.block_type = expected_type;
    hdr.flags = raw_flags;
    hdr.block_seq = read_u32_le(&buf[8]);
    hdr.timestamp_ms = read_u32_le(&buf[12]);
    return Result<TrustedSdHeader>::ok(hdr);
}

}  // namespace

bool SensorBoundaryMonitor::update_leads_off(bool lod_p_high,
                                             bool lod_n_high,
                                             uint32_t uptime_ms) noexcept {
    const bool raw_lod = lod_p_high || lod_n_high;

    if (raw_lod != lod_candidate_ || !has_lod_candidate_time_) {
        lod_candidate_ = raw_lod;
        lod_candidate_since_ms_ = uptime_ms;
        has_lod_candidate_time_ = true;
    }

    const uint32_t stable_ms = uptime_ms - lod_candidate_since_ms_;
    if (stable_ms >= kLodDebounceMs && lod_debounced_ != lod_candidate_) {
        const bool was_detached = lod_debounced_;
        lod_debounced_ = lod_candidate_;
        if (lod_debounced_) {
            lod_active_since_ms_ = uptime_ms;
            has_lod_active_time_ = true;
        } else {
            has_lod_active_time_ = false;
            lod_stop_requested_ = false;
            if (was_detached) {
                // OPEN-6c: reset IIR and Pan-Tompkins when electrodes re-attach
                dsp_reset_pending_ = true;
            }
        }
    }

    const bool raw_both_lod = lod_p_high && lod_n_high;
    if (!raw_both_lod) {
        // Fail-safe: any electrode attachment drops HIL-safe flag immediately (0 ms delay)
        both_lod_candidate_ = false;
        both_lod_candidate_since_ms_ = uptime_ms;
        has_both_lod_candidate_time_ = true;
        both_lod_debounced_ = false;
    } else {
        if (!both_lod_candidate_ || !has_both_lod_candidate_time_) {
            both_lod_candidate_ = true;
            both_lod_candidate_since_ms_ = uptime_ms;
            has_both_lod_candidate_time_ = true;
        }
        if ((uptime_ms - both_lod_candidate_since_ms_) >= kLodDebounceMs) {
            both_lod_debounced_ = true;
        }
    }

    if (lod_debounced_ && has_lod_active_time_) {
        if ((uptime_ms - lod_active_since_ms_) >= kLodStopTimeoutMs) {
            lod_stop_requested_ = true;
        }
    }

    return lod_debounced_;
}

FailureStatus SensorBoundaryMonitor::validate_wearable_batch_into(
    const WearableBatchInputs& inputs, TrustedWearableBatch& out) noexcept {
    // Update debounced Leads-Off tracker first so 50 ms debounce and 60 s Stop timer stay current.
    const bool lod_now = update_leads_off(inputs.lod_p_high, inputs.lod_n_high, inputs.uptime_ms);

    if (low_battery_latched_) {
        return FailureStatus{CanonicalState::SensorUnavailable, DetailCode::LowBattery};
    }

    if (!inputs.afe_enabled) {
        has_last_adc_ms_ = false;
        return FailureStatus{CanonicalState::SensorUnavailable, DetailCode::AfeShutdown};
    }

    if (lod_now) {
        has_last_adc_ms_ = false;
        return FailureStatus{CanonicalState::SensorUnavailable, DetailCode::LeadsOff};
    }

    if (inputs.fs_hz != kFs250Hz && inputs.fs_hz != kFs500Hz) {
        return FailureStatus{CanonicalState::Malformed, DetailCode::SemanticRange};
    }

    // Ping-Pong DMA overflow check (OPEN-7b: 3 consecutive overflow blocks -> Stop Mode)
    if (inputs.pingpong_overflow) {
        if (consecutive_overflow_faults_ < 255U) {
            ++consecutive_overflow_faults_;
        }
        if (consecutive_overflow_faults_ >= kMaxConsecutiveSdFaults) {
            sd_stop_requested_ = true;
            return FailureStatus{CanonicalState::CommsFault, DetailCode::SpiSdFault};
        }
        return FailureStatus{CanonicalState::CommsFault, DetailCode::PingPongOverflow};
    }

    // Explicit uptime-based staleness check (OPEN-5)
    if (has_last_adc_ms_) {
        const uint32_t elapsed_ms = inputs.uptime_ms - last_adc_ms_;
        if (elapsed_ms > adc_stale_threshold_ms(inputs.fs_hz)) {
            last_adc_ms_ = inputs.uptime_ms;
            return FailureStatus{CanonicalState::Stale, DetailCode::StaleTimeout};
        }
    }

    // Buffer pointer & length checks
    if (inputs.raw_samples == nullptr || inputs.sample_count == 0U ||
        inputs.sample_count > kSdMaxRawSamples) {
        return FailureStatus{CanonicalState::Malformed, DetailCode::BadLength};
    }

    out.timestamp_ms = inputs.uptime_ms;
    out.fs_hz = inputs.fs_hz;
    out.sample_count = static_cast<uint16_t>(inputs.sample_count);

    for (std::size_t i = 0U; i < inputs.sample_count; ++i) {
        const int16_t sample = inputs.raw_samples[i];
        if (sample < static_cast<int16_t>(kAdcRawMin) ||
            sample > static_cast<int16_t>(kAdcRawMax)) {
            return FailureStatus{CanonicalState::Malformed, DetailCode::SemanticRange};
        }
        out.samples[i] = sample;
    }

    consecutive_overflow_faults_ = 0U;
    last_adc_ms_ = inputs.uptime_ms;
    has_last_adc_ms_ = true;
    return FailureStatus{CanonicalState::Valid, DetailCode::Ok};
}

Result<TrustedWearableBatch> SensorBoundaryMonitor::validate_wearable_batch(
    const WearableBatchInputs& inputs) noexcept {
    TrustedWearableBatch out{};
    const FailureStatus st = validate_wearable_batch_into(inputs, out);
    if (st.canonical != CanonicalState::Valid) {
        return Result<TrustedWearableBatch>::fail(st);
    }
    return Result<TrustedWearableBatch>::ok(out);
}

Result<TrustedBatteryReading> SensorBoundaryMonitor::validate_battery_adc(
    bool adc_ok, uint16_t raw_adc) noexcept {
    if (!adc_ok) {
        return Result<TrustedBatteryReading>::fail(CanonicalState::SensorUnavailable,
                                                   DetailCode::LowBattery);
    }

    if (raw_adc > kAdcRawMax) {
        return Result<TrustedBatteryReading>::fail(CanonicalState::Malformed,
                                                   DetailCode::SemanticRange);
    }

    // 12-bit ADC @ 3300 mV VREF with 1:2 divider (100k / 100k on PB1 gated by PB5)
    const uint32_t vbat_mv_u32 =
        (static_cast<uint32_t>(raw_adc) * 6600U + 2047U) / static_cast<uint32_t>(kAdcRawMax);
    const uint16_t vbat_mv = static_cast<uint16_t>(vbat_mv_u32);

    if (vbat_mv < kVbatLowStopMv) {
        low_battery_latched_ = true;
        return Result<TrustedBatteryReading>::fail(CanonicalState::SensorUnavailable,
                                                   DetailCode::LowBattery);
    }

    if (low_battery_latched_) {
        if (vbat_mv < kVbatRecoverMv) {
            return Result<TrustedBatteryReading>::fail(CanonicalState::SensorUnavailable,
                                                       DetailCode::LowBattery);
        }
        low_battery_latched_ = false;
    }

    TrustedBatteryReading reading{};
    reading.raw_adc = raw_adc;
    reading.vbat_mv = vbat_mv;
    return Result<TrustedBatteryReading>::ok(reading);
}

Result<TrustedIna219Reading> SensorBoundaryMonitor::validate_ina219_reading(
    const Ina219RawTransaction& tx) const noexcept {
    // R-BND-02 / R-PWR-02: I2C failure or timeout > 10 ms MUST return CommsFault, never 0.
    if (!tx.i2c_ok || tx.transaction_ms > kI2cTimeoutMs) {
        return Result<TrustedIna219Reading>::fail(CanonicalState::CommsFault,
                                                  DetailCode::I2cIna219Fault);
    }

    if (tx.bus_mv > kIna219MaxBusMv || tx.current_ua < -kIna219MaxCurrentUa ||
        tx.current_ua > kIna219MaxCurrentUa) {
        return Result<TrustedIna219Reading>::fail(CanonicalState::Malformed,
                                                  DetailCode::SemanticRange);
    }

    TrustedIna219Reading out{};
    out.bus_mv = tx.bus_mv;
    out.current_ua = tx.current_ua;
    return Result<TrustedIna219Reading>::ok(out);
}

FailureStatus SensorBoundaryMonitor::record_sd_write_result(bool spi_ok,
                                                            uint32_t write_duration_ms) noexcept {
    if (spi_ok && write_duration_ms <= kSpiWriteTimeoutMs) {
        consecutive_spi_faults_ = 0U;
        return FailureStatus{CanonicalState::Valid, DetailCode::Ok};
    }

    if (consecutive_spi_faults_ < 255U) {
        ++consecutive_spi_faults_;
    }
    if (consecutive_spi_faults_ >= kMaxConsecutiveSdFaults) {
        sd_stop_requested_ = true;
    }
    return FailureStatus{CanonicalState::CommsFault, DetailCode::SpiSdFault};
}

bool SensorBoundaryMonitor::consume_dsp_reset_pending() noexcept {
    const bool pending = dsp_reset_pending_;
    dsp_reset_pending_ = false;
    return pending;
}

void SensorBoundaryMonitor::reset_after_wake() noexcept {
    lod_stop_requested_ = false;
    sd_stop_requested_ = false;
    consecutive_overflow_faults_ = 0U;
    consecutive_spi_faults_ = 0U;
    has_last_adc_ms_ = false;
    has_lod_candidate_time_ = false;
    has_both_lod_candidate_time_ = false;
    has_lod_active_time_ = false;
}

Result<TrustedSdRawBlock> parse_sd_raw_ecg_block(ConstByteView raw_block) noexcept {
    const Result<TrustedSdHeader> hdr =
        parse_and_validate_sd_envelope(raw_block, SdBlockType::RawEcg);
    if (hdr.is_err()) {
        return Result<TrustedSdRawBlock>::fail(hdr.status());
    }

    const uint8_t* const buf = raw_block.data;
    const uint16_t valid_count = read_u16_le(&buf[16]);
    if (valid_count == 0U || valid_count > kSdMaxRawSamples) {
        return Result<TrustedSdRawBlock>::fail(CanonicalState::Malformed,
                                               DetailCode::SemanticRange);
    }

    TrustedSdRawBlock block{};
    block.header = hdr.value();
    block.valid_sample_count = valid_count;

    for (std::size_t i = 0U; i < static_cast<std::size_t>(valid_count); ++i) {
        const int16_t s = read_i16_le(&buf[18U + (2U * i)]);
        if (s < static_cast<int16_t>(kAdcRawMin) || s > static_cast<int16_t>(kAdcRawMax)) {
            return Result<TrustedSdRawBlock>::fail(CanonicalState::Malformed,
                                                   DetailCode::SemanticRange);
        }
        block.samples[i] = s;
    }
    return Result<TrustedSdRawBlock>::ok(block);
}

Result<TrustedSdRrHrvBlock> parse_sd_rr_hrv_block(ConstByteView raw_block) noexcept {
    const Result<TrustedSdHeader> hdr =
        parse_and_validate_sd_envelope(raw_block, SdBlockType::RrHrv);
    if (hdr.is_err()) {
        return Result<TrustedSdRrHrvBlock>::fail(hdr.status());
    }

    const uint8_t* const buf = raw_block.data;
    const uint8_t valid_rr_count = buf[16];
    const uint8_t hrv_valid_raw = buf[17];
    if (valid_rr_count > kSdMaxRrRecords || (hrv_valid_raw != 0U && hrv_valid_raw != 1U)) {
        return Result<TrustedSdRrHrvBlock>::fail(CanonicalState::Malformed,
                                                 DetailCode::SemanticRange);
    }

    const uint16_t vbat_mv = read_u16_le(&buf[18]);
    const int16_t ina219_cur = read_i16_le(&buf[20]);
    const uint16_t ina219_bus = read_u16_le(&buf[22]);
    if (vbat_mv > kVbatMaxPhysicalMv || ina219_bus > kIna219MaxBusMv) {
        return Result<TrustedSdRrHrvBlock>::fail(CanonicalState::Malformed,
                                                 DetailCode::SemanticRange);
    }

    TrustedSdRrHrvBlock block{};
    block.header = hdr.value();
    block.valid_rr_count = valid_rr_count;
    block.hrv_valid = (hrv_valid_raw == 1U);
    block.ina219_valid = (block.header.flags & (1U << 6U)) == 0U;
    block.battery_valid = (block.header.flags & (1U << 7U)) == 0U;
    block.vbat_mv = vbat_mv;
    block.ina219_current_ua_div10 = ina219_cur;
    block.ina219_bus_mv = ina219_bus;

    // HRV metrics at bytes 24..39 (PROPOSED-SD-02, PROTOCOL.md section 3.4)
    block.hrv.mean_rr_us = read_u32_le(&buf[24]);
    block.hrv.sdnn_us = read_u32_le(&buf[28]);
    block.hrv.rmssd_us = read_u32_le(&buf[32]);
    block.hrv.hr_bpm_x100 = read_u16_le(&buf[36]);
    block.hrv.pnn50_permille = read_u16_le(&buf[38]);

    // 46 R-peak records at bytes 40..499 (10 bytes each)
    for (std::size_t i = 0U; i < static_cast<std::size_t>(valid_rr_count); ++i) {
        const std::size_t off = 40U + (10U * i);
        block.peaks[i].peak_sample_idx = read_u32_le(&buf[off]);
        block.peaks[i].sub_sample_offset_q15 = read_i16_le(&buf[off + 4U]);
        block.peaks[i].rr_interval_us = read_u32_le(&buf[off + 6U]);
    }
    return Result<TrustedSdRrHrvBlock>::ok(block);
}

}  // namespace p05
