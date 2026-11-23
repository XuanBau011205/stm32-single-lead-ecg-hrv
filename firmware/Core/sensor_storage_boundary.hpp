#ifndef P05_CORE_SENSOR_STORAGE_BOUNDARY_HPP
#define P05_CORE_SENSOR_STORAGE_BOUNDARY_HPP

#include <cstddef>
#include <cstdint>

#include "../HIL/trusted_types.hpp"

namespace p05 {

// Hardware-independent inputs captured by Platform Adapter for a wearable ADC half-buffer.
struct WearableBatchInputs {
    const int16_t* raw_samples = nullptr;  // Owner: caller; length == sample_count
    std::size_t sample_count = 0U;
    uint16_t fs_hz = kDefaultWearableFsHz;
    uint32_t uptime_ms = 0U;
    bool afe_enabled = false;         // PA3 (ECG_SDN == HIGH)
    bool lod_p_high = false;          // PA1 (ECG_LOD_P, HIGH = detached, OPEN-6)
    bool lod_n_high = false;          // PA2 (ECG_LOD_N, HIGH = detached, OPEN-6)
    bool pingpong_overflow = false;   // DMA overwrote unread half-buffer (OPEN-7)
};

// Raw INA219 transaction result supplied by I2C1 adapter.
struct Ina219RawTransaction {
    bool i2c_ok = false;
    uint32_t transaction_ms = 0U;
    uint16_t bus_mv = 0U;
    int32_t current_ua = 0;
};

// Portable boundary adapter for all non-UART untrusted hardware inputs:
// ADC1 (PA0), LOD (PA1/PA2), BAT_SENSE (PB1/PB5), INA219 (PB6/PB7), microSD (PA4..PA7).
// Contains zero chip/HAL/RTOS calls (RULES section 3).
class SensorBoundaryMonitor {
public:
    SensorBoundaryMonitor() noexcept = default;

    // Updates debounced Leads-Off state (50 ms debounce, 60 s continuous -> Stop Mode, OPEN-6).
    // Returns true when debounced Leads-Off is active.
    bool update_leads_off(bool lod_p_high, bool lod_n_high, uint32_t uptime_ms) noexcept;

    // Validates an incoming wearable ECG ADC half-buffer at the boundary.
    Result<TrustedWearableBatch> validate_wearable_batch(
        const WearableBatchInputs& inputs) noexcept;

    // Validates a 12-bit BAT_SENSE reading on PB1 (1:2 divider via PB5 2N7002, OPEN-8).
    // Latches LowBattery below 3400 mV and only recovers at >= 3600 mV.
    Result<TrustedBatteryReading> validate_battery_adc(bool adc_ok, uint16_t raw_adc) noexcept;

    // Validates an INA219 I2C1 reading (10 ms timeout, OPEN-3, R-BND-02 / R-PWR-02).
    Result<TrustedIna219Reading> validate_ina219_reading(
        const Ina219RawTransaction& tx) const noexcept;

    // Tracks microSD SPI1 block write faults (OPEN-3 / OPEN-7b: 3 consecutive -> Stop Mode).
    FailureStatus record_sd_write_result(bool spi_ok, uint32_t write_duration_ms) noexcept;

    // Latches low battery or hardware faults independently of comms errors (R-BND-03).
    bool leads_off_debounced() const noexcept { return lod_debounced_; }
    bool dsp_reset_pending() const noexcept { return dsp_reset_pending_; }
    bool consume_dsp_reset_pending() noexcept;

    bool low_battery_latched() const noexcept { return low_battery_latched_; }
    bool stop_mode_requested() const noexcept {
        return lod_stop_requested_ || low_battery_latched_ || sd_stop_requested_;
    }
    uint8_t consecutive_overflow_faults() const noexcept { return consecutive_overflow_faults_; }
    uint8_t consecutive_spi_faults() const noexcept { return consecutive_spi_faults_; }

    // Clears transient stop flags after a clean wake/re-init (never clears low_battery_latched_).
    void reset_after_wake() noexcept;

private:
    bool lod_debounced_ = false;
    bool lod_candidate_ = false;
    bool has_lod_candidate_time_ = false;
    uint32_t lod_candidate_since_ms_ = 0U;
    bool has_lod_active_time_ = false;
    uint32_t lod_active_since_ms_ = 0U;
    bool lod_stop_requested_ = false;
    bool dsp_reset_pending_ = false;

    bool has_last_adc_ms_ = false;
    uint32_t last_adc_ms_ = 0U;

    bool low_battery_latched_ = false;

    uint8_t consecutive_overflow_faults_ = 0U;
    uint8_t consecutive_spi_faults_ = 0U;
    bool sd_stop_requested_ = false;
};

// Stateless boundary parsers for untrusted 512-byte blocks read back from microSD (PROPOSED-SD-01/02).
// Validates strictly in order: length (512) -> sync ("ECG5") -> version (0x01) -> CRC-16 -> type -> ranges.
Result<TrustedSdRawBlock> parse_sd_raw_ecg_block(ConstByteView raw_block) noexcept;
Result<TrustedSdRrHrvBlock> parse_sd_rr_hrv_block(ConstByteView raw_block) noexcept;

}  // namespace p05

#endif  // P05_CORE_SENSOR_STORAGE_BOUNDARY_HPP
