#ifndef P05_CORE_PLATFORM_CONTROLLER_HPP
#define P05_CORE_PLATFORM_CONTROLLER_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "../DSP/hrv_analyzer.hpp"
#include "../DSP/pan_tompkins.hpp"
#include "../HIL/hil_session.hpp"
#include "../HIL/trusted_types.hpp"
#include "ping_pong_buffer.hpp"
#include "sd_block_formatter.hpp"
#include "sensor_storage_boundary.hpp"

namespace p05 {

enum class PowerClockProfile : uint8_t {
    Run8MHzQ15Sleep = 0U,   // Optimized wearable profile (~7.6 mA, ~52.6 h battery)
    Run72MHzSoftFloat = 1U, // Baseline Ablation 1 profile (~30.4 mA, ~13.1 h battery)
};

enum class AppMode : uint8_t {
    BootInit = 0U,
    WearableRun = 1U,
    HilMode = 2U,
    StopMode = 3U,
};

// Abstract hardware register/bus interface implemented by:
// - Stm32F103PlatformHw on real MCU (CMSIS bare-metal + I2C1 exception OPEN-22)
// - MockPlatformHw in host unit/integration tests (TASKS.md Card 5)
// Protected non-virtual destructor avoids operator delete linkage in freestanding MCU builds.
class IPlatformHw {
public:
    // Time & DWT cycle counter
    virtual uint32_t uptime_ms() const noexcept = 0;
    virtual uint32_t dwt_cycles() const noexcept = 0;

    // Clocks & low-power execution
    virtual void set_clock_profile(PowerClockProfile profile) noexcept = 0;
    virtual void enter_sleep_wfi() noexcept = 0;
    virtual void configure_unused_gpio_analog_keep_pb12() noexcept = 0;
    virtual void enter_stop_mode_wfi() noexcept = 0;

    // Analog Front-End (AD8232 on PA0/PA1/PA2/PA3)
    virtual void set_afe_enable(bool enable) noexcept = 0;  // PA3 HIGH=on, LOW=shutdown (0.5 uA)
    virtual bool read_lod_p_high() const noexcept = 0;      // PA1 HIGH = RA detached
    virtual bool read_lod_n_high() const noexcept = 0;      // PA2 HIGH = LA detached

    // Timer-triggered ADC1 + DMA1_Ch1 Ping-Pong (PA0)
    virtual void start_ecg_adc_dma(int16_t* dst_490, uint16_t fs_hz) noexcept = 0;
    virtual void stop_ecg_adc_dma() noexcept = 0;

    // Battery Sense (PB1 ADC1_IN9 gated by 2N7002 on PB5)
    virtual void set_bat_sense_enable(bool enable) noexcept = 0;  // PB5 HIGH=measure, LOW=0 uA
    virtual bool read_bat_sense_adc(uint16_t& raw_adc_out) noexcept = 0;

    // microSD SPI1 (PA4..PA7) & AO3401 P-MOSFET Power Gate (PB0)
    virtual void set_spi1_pins_analog() noexcept = 0;     // Prevents ESD-diode back-powering!
    virtual void set_sd_power_enable(bool enable) noexcept = 0;  // PB0 LOW=ON, HIGH=OFF (0 uA)
    virtual bool sd_init_card() noexcept = 0;
    virtual bool sd_write_sector(uint32_t lba,
                                 const uint8_t* data_512,
                                 uint32_t& duration_ms_out) noexcept = 0;

    // INA219 over I2C1 (PB6/PB7)
    virtual Ina219RawTransaction ina219_read() noexcept = 0;
    virtual bool ina219_power_down() noexcept = 0;

    // UART1 (PA9/PA10, 115200 bps)
    virtual void uart1_send(ConstByteView frame) noexcept = 0;

protected:
    ~IPlatformHw() = default;
};

// Top-level P05 system controller orchestrating Card 1..4 portable modules over IPlatformHw.
// Enforces all Gate G3/G4 hardware Fail-Safe rules (OPEN-1..25) with zero heap allocation.
class P05AppController {
public:
    static constexpr uint32_t kBatteryPollPeriodMs = 10000U;  // OPEN-8: 10 s

    explicit P05AppController(IPlatformHw& hw) noexcept;

    // Initial boot check: measures VBAT (>= 3.60 V) and enters WearableRun (250 Hz default)
    // or immediately enters StopMode if battery is below recovery threshold.
    void boot(PowerClockProfile profile = PowerClockProfile::Run8MHzQ15Sleep) noexcept;

    // Called by DMA1_Channel1_IRQHandler (HTIF -> half=0, TCIF -> half=1). O(1) non-blocking!
    void isr_on_dma_half(uint8_t half_index) noexcept;

    // Called when SW_WAKE (PB12 EXTI12) triggers a user sleep request while running.
    void request_user_stop() noexcept { user_stop_requested_ = true; }

    // One non-blocking iteration of the main superloop.
    void step() noexcept;

    // Feeds one complete binary frame received from UART1 RX ring into HIL safety/session stack.
    void on_uart1_frame(ConstByteView raw_frame) noexcept;

    // Records a UART1 transport error without clearing latched protection lockout (R-BND-03).
    void on_uart1_comms_fault() noexcept;

    // Executes the exact OPEN-9b / R-PWR-01 Stop Mode sequence (32.1 uA target).
    void enter_stop_mode() noexcept;

    AppMode mode() const noexcept { return mode_; }
    bool ina219_powerdown_fault() const noexcept { return ina219_pd_fault_; }
    uint32_t sd_sectors_written() const noexcept { return sd_next_lba_; }
    bool hil_lockout_latched() const noexcept { return hil_.lockout_latched(); }

private:
    static uint32_t dwt_callback(void* ctx) noexcept;

    bool write_sd_sector_with_retry(const SdSectorBuffer& sector) noexcept;
    void poll_battery_and_ina219(uint32_t now_ms) noexcept;
    void flush_rr_hrv_block(uint32_t now_ms) noexcept;
    uint16_t build_sd_flags(bool overflow_gap) const noexcept;
    void start_wearable_mode() noexcept;

    IPlatformHw& hw_;
    PingPongDmaBuffer ping_pong_{};
    SensorBoundaryMonitor sensor_mon_{};
    EcgDspCore wearable_dsp_{};
    HrvAnalyzer wearable_hrv_{};
    HilSession hil_;

    AppMode mode_ = AppMode::BootInit;
    PowerClockProfile profile_ = PowerClockProfile::Run8MHzQ15Sleep;
    bool afe_enabled_ = false;
    bool sd_powered_ = false;
    bool sd_ready_ = false;
    bool protection_known_ = true;
    bool user_stop_requested_ = false;
    bool ina219_pd_fault_ = false;

    uint32_t wearable_sample_idx_ = 0U;
    uint32_t sd_block_seq_ = 0U;
    uint32_t sd_next_lba_ = 0U;
    uint32_t last_bat_poll_ms_ = 0U;
    bool has_bat_poll_ = false;

    Result<TrustedBatteryReading> last_bat_res_ =
        Result<TrustedBatteryReading>::fail(CanonicalState::SensorUnavailable, DetailCode::LowBattery);
    Result<TrustedIna219Reading> last_ina_res_ =
        Result<TrustedIna219Reading>::fail(CanonicalState::CommsFault, DetailCode::I2cIna219Fault);

    std::array<TrustedRPeakRecord, kSdMaxRrRecords> pending_peaks_{};
    uint8_t pending_peak_count_ = 0U;
};

}  // namespace p05

#endif  // P05_CORE_PLATFORM_CONTROLLER_HPP
