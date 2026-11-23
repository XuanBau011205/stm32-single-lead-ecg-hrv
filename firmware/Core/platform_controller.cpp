#include "platform_controller.hpp"

namespace p05 {

uint32_t P05AppController::dwt_callback(void* ctx) noexcept {
    auto* self = static_cast<P05AppController*>(ctx);
    return self->hw_.dwt_cycles();
}

P05AppController::P05AppController(IPlatformHw& hw) noexcept
    : hw_(hw), hil_(&P05AppController::dwt_callback, this) {}

void P05AppController::boot(PowerClockProfile profile) noexcept {
    profile_ = profile;
    mode_ = AppMode::BootInit;
    user_stop_requested_ = false;
    ina219_pd_fault_ = false;
    hw_.set_clock_profile(profile_);

    // Measure battery voltage first via PB5 (2N7002 ON -> ADC PB1 -> OFF) (OPEN-1 / OPEN-8)
    hw_.set_bat_sense_enable(true);
    uint16_t raw_bat = 0U;
    const bool adc_ok = hw_.read_bat_sense_adc(raw_bat);
    hw_.set_bat_sense_enable(false);

    last_bat_res_ = sensor_mon_.validate_battery_adc(adc_ok, raw_bat);
    last_bat_poll_ms_ = hw_.uptime_ms();
    has_bat_poll_ = true;

    if (sensor_mon_.low_battery_latched() ||
        (last_bat_res_.is_ok() && last_bat_res_.value().vbat_mv < kVbatRecoverMv)) {
        enter_stop_mode();
        return;
    }

    start_wearable_mode();
}

void P05AppController::start_wearable_mode() noexcept {
    sensor_mon_.reset_after_wake();
    ping_pong_.reset();

    const MathMode mm =
        (profile_ == PowerClockProfile::Run72MHzSoftFloat) ? MathMode::SoftFloat : MathMode::Q15;
    (void)wearable_dsp_.configure(DspConfig{kDefaultWearableFsHz, mm, true});  // 250 Hz (OPEN-25)
    wearable_hrv_.configure(mm);
    wearable_sample_idx_ = 0U;
    pending_peak_count_ = 0U;

    // Power on microSD via AO3401 (PB0 = LOW) and initialize SDHC raw block mode
    hw_.set_sd_power_enable(true);
    sd_powered_ = true;
    sd_ready_ = hw_.sd_init_card();

    // Enable AD8232 AFE (PA3 = HIGH) and start Timer-triggered ADC1 + Circular Ping-Pong DMA
    hw_.set_afe_enable(true);
    afe_enabled_ = true;
    hw_.start_ecg_adc_dma(ping_pong_.dma_dest_ptr(), kDefaultWearableFsHz);

    mode_ = AppMode::WearableRun;
}

void P05AppController::isr_on_dma_half(uint8_t half_index) noexcept {
    ping_pong_.isr_notify_half_complete(half_index, hw_.uptime_ms());
}

uint16_t P05AppController::build_sd_flags(bool overflow_gap) const noexcept {
    uint16_t flags = 0U;
    if (hw_.read_lod_p_high()) {
        flags |= sd_flags::kLodP;
    }
    if (hw_.read_lod_n_high()) {
        flags |= sd_flags::kLodN;
    }
    if (wearable_dsp_.config().math_mode == MathMode::SoftFloat) {
        flags |= sd_flags::kSoftFloat;
    }
    if (wearable_dsp_.config().fs_hz == kFs500Hz) {
        flags |= sd_flags::kFs500Hz;
    }
    if (overflow_gap) {
        flags |= sd_flags::kPingPongOverflowGap;
    }
    if (sensor_mon_.low_battery_latched()) {
        flags |= sd_flags::kLowBattery;
    }
    return flags;
}

bool P05AppController::write_sd_sector_with_retry(const SdSectorBuffer& sector) noexcept {
    if (!sd_powered_ || !sd_ready_) {
        (void)sensor_mon_.record_sd_write_result(false, 0U);
        return false;
    }

    // OPEN-3: SPI1 write timeout 250 ms; retry 1 time before counting as SD fault
    for (int attempt = 0; attempt < 2; ++attempt) {
        uint32_t dur_ms = 0U;
        const bool ok = hw_.sd_write_sector(sd_next_lba_, sector.data(), dur_ms);
        if (ok && dur_ms <= kSpiWriteTimeoutMs) {
            (void)sensor_mon_.record_sd_write_result(true, dur_ms);
            ++sd_next_lba_;
            return true;
        }
    }

    (void)sensor_mon_.record_sd_write_result(false, kSpiWriteTimeoutMs + 1U);
    return false;
}

void P05AppController::flush_rr_hrv_block(uint32_t now_ms) noexcept {
    if (!sd_ready_ || pending_peak_count_ == 0U) {
        return;
    }
    SdRrHrvBlockInput in{};
    in.block_seq = sd_block_seq_++;
    in.timestamp_ms = now_ms;
    in.flags = build_sd_flags(false);
    in.valid_rr_count = pending_peak_count_;
    in.peaks = pending_peaks_;
    in.hrv_res = wearable_hrv_.compute_metrics();
    in.battery_res = last_bat_res_;
    in.ina219_res = last_ina_res_;

    const Result<SdSectorBuffer> formatted = format_sd_rr_hrv_block(in);
    if (formatted.is_ok()) {
        (void)write_sd_sector_with_retry(formatted.value());
    }
    pending_peak_count_ = 0U;
}

void P05AppController::poll_battery_and_ina219(uint32_t now_ms) noexcept {
    if (has_bat_poll_ && (now_ms - last_bat_poll_ms_) < kBatteryPollPeriodMs) {
        return;
    }
    last_bat_poll_ms_ = now_ms;
    has_bat_poll_ = true;

    // 1. Read VBAT with 2N7002 enabled only during ADC sample (0 uA steady leak)
    hw_.set_bat_sense_enable(true);
    uint16_t raw_bat = 0U;
    const bool bat_ok = hw_.read_bat_sense_adc(raw_bat);
    hw_.set_bat_sense_enable(false);
    last_bat_res_ = sensor_mon_.validate_battery_adc(bat_ok, raw_bat);

    // 2. Read INA219 over I2C1 (10 ms timeout; fault returns CommsFault, never 0 per R-PWR-02)
    last_ina_res_ = sensor_mon_.validate_ina219_reading(hw_.ina219_read());
}

void P05AppController::step() noexcept {
    const uint32_t now_ms = hw_.uptime_ms();

    if (mode_ == AppMode::HilMode) {
        const FailureStatus to_st = hil_.poll_timeout(now_ms);
        if (to_st.canonical == CanonicalState::CommsFault || !hil_.active()) {
            // Return to safe default wearable mode after HIL session closes (OPEN-2)
            start_wearable_mode();
        }
        return;
    }

    if (mode_ != AppMode::WearableRun) {
        return;
    }

    // 1. Keep Leads-Off debounce & 60 s Stop timer updated every loop tick
    (void)sensor_mon_.update_leads_off(hw_.read_lod_p_high(), hw_.read_lod_n_high(), now_ms);

    // 2. Poll battery (every 10 s) and INA219
    poll_battery_and_ina219(now_ms);

    // 3. Consume ready Ping-Pong DMA half-buffer (245 samples) if available
    const PingPongDmaBuffer::ReadyHalf half = ping_pong_.main_poll_ready();
    if (half.ready) {
        WearableBatchInputs in{};
        in.raw_samples = half.samples;
        in.sample_count = PingPongDmaBuffer::kHalfSamples;
        in.fs_hz = wearable_dsp_.config().fs_hz;
        in.uptime_ms = half.timestamp_ms;
        in.afe_enabled = afe_enabled_;
        in.lod_p_high = hw_.read_lod_p_high();
        in.lod_n_high = hw_.read_lod_n_high();
        in.pingpong_overflow = half.overflowed;

        const Result<TrustedWearableBatch> batch_res = sensor_mon_.validate_wearable_batch(in);

        if (sensor_mon_.consume_dsp_reset_pending()) {
            // OPEN-6c: reset IIR and Pan-Tompkins when electrodes re-attach
            wearable_dsp_.reset();
            wearable_hrv_.on_interruption(
                FailureStatus{CanonicalState::SensorUnavailable, DetailCode::LeadsOff});
        }

        const Result<DspBatchOutput> dsp_res =
            wearable_dsp_.process_wearable_batch(batch_res, wearable_sample_idx_);
        wearable_sample_idx_ += static_cast<uint32_t>(PingPongDmaBuffer::kHalfSamples);

        if (batch_res.is_ok()) {
            // Write 512 B RAW_ECG_BLOCK to microSD via SPI1
            const Result<SdSectorBuffer> raw_blk = format_sd_raw_ecg_block(
                sd_block_seq_++, half.timestamp_ms, build_sd_flags(false), batch_res.value());
            if (raw_blk.is_ok()) {
                (void)write_sd_sector_with_retry(raw_blk.value());
            }

            if (dsp_res.is_ok()) {
                for (uint8_t p = 0U; p < dsp_res.value().r_peak_count; ++p) {
                    const DetectedRPeak& pk = dsp_res.value().peaks[p];
                    (void)wearable_hrv_.push_r_peak(pk);

                    if (pending_peak_count_ < kSdMaxRrRecords) {
                        TrustedRPeakRecord rec{};
                        rec.peak_sample_idx = pk.peak_sample_idx;
                        rec.sub_sample_offset_q15 = pk.sub_sample_offset_q15;
                        rec.rr_interval_us = pk.has_rr ? pk.rr_interval_us : 0U;
                        pending_peaks_[pending_peak_count_++] = rec;
                    }
                    if (pending_peak_count_ >= kSdMaxRrRecords) {
                        flush_rr_hrv_block(half.timestamp_ms);
                    }
                }
            }
        } else {
            wearable_hrv_.on_interruption(batch_res.status());
        }
    }

    // 4. Check all Stop Mode triggers (LOD 60 s, VBAT < 3.40 V, 3x SD/overflow faults, user button)
    if (sensor_mon_.stop_mode_requested() || user_stop_requested_) {
        enter_stop_mode();
        return;
    }

    // 5. Optimized mode: sleep between samples via WFI (SPEC section 4.B, R-PWR-02)
    if (profile_ == PowerClockProfile::Run8MHzQ15Sleep && !half.ready) {
        hw_.enter_sleep_wfi();
    }
}

void P05AppController::on_uart1_frame(ConstByteView raw_frame) noexcept {
    const uint32_t now_ms = hw_.uptime_ms();
    // OPEN-2: electrodes detached ONLY when BOTH PA1 and PA2 report HIGH
    const bool detached = hw_.read_lod_p_high() && hw_.read_lod_n_high();
    const HilSafetyInputs safety{protection_known_, detached};

    const EncodedHilFrame resp = hil_.handle_frame(raw_frame, now_ms, safety);
    hw_.uart1_send(resp.view());

    if (hil_.active() && mode_ != AppMode::HilMode) {
        // Safely isolate wearable peripherals during HIL session
        hw_.stop_ecg_adc_dma();
        hw_.set_afe_enable(false);
        afe_enabled_ = false;
        hw_.set_spi1_pins_analog();
        hw_.set_sd_power_enable(false);
        sd_powered_ = false;
        sd_ready_ = false;
        mode_ = AppMode::HilMode;
    }
}

void P05AppController::on_uart1_comms_fault() noexcept {
    (void)hil_.on_uart_fault();
}

void P05AppController::enter_stop_mode() noexcept {
    const uint32_t now_ms = hw_.uptime_ms();

    // Exact OPEN-9b / R-PWR-01 power-down sequence for 32.1 uA Stop Mode:
    // 1. Stop TIM3 / ADC1 / DMA1_Ch1
    hw_.stop_ecg_adc_dma();

    // 2. Flush final pending RR_HRV_BLOCK to microSD if SD is healthy and powered (OPEN-9b)
    const bool sd_healthy =
        sd_powered_ && sd_ready_ &&
        (sensor_mon_.consecutive_spi_faults() < kMaxConsecutiveSdFaults) &&
        (sensor_mon_.consecutive_overflow_faults() < kMaxConsecutiveSdFaults);
    if (sd_healthy) {
        flush_rr_hrv_block(now_ms);
    }

    // 3. CRITICAL hardware rule: set PA4..PA7 (SPI1) to Analog Input BEFORE/WITH PB0 = HIGH
    //    to prevent ESD-diode back-powering leakage into the unpowered microSD card!
    hw_.set_spi1_pins_analog();
    hw_.set_sd_power_enable(false);
    sd_powered_ = false;
    sd_ready_ = false;

    // 4. Send Power-Down command to INA219 over I2C1 (retry once on failure, log flag, still sleep)
    bool pd_ok = hw_.ina219_power_down();
    if (!pd_ok) {
        pd_ok = hw_.ina219_power_down();
    }
    ina219_pd_fault_ = !pd_ok;

    // 5. Shutdown AD8232 (PA3 = LOW -> 0.5 uA)
    hw_.set_afe_enable(false);
    afe_enabled_ = false;

    // 6. Disconnect 2N7002 battery divider (PB5 = LOW -> 0 uA)
    hw_.set_bat_sense_enable(false);

    // 7. Put all unused GPIOs to Analog Input while preserving PB12 (EXTI12 SW_WAKE)
    hw_.configure_unused_gpio_analog_keep_pb12();

    // 8. Enter STM32 Stop Mode (PWR->CR, SLEEPDEEP, WFI -> 32.1 uA total)
    mode_ = AppMode::StopMode;
    user_stop_requested_ = false;
    hw_.enter_stop_mode_wfi();
}

}  // namespace p05
