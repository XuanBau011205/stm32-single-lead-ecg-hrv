#ifndef P05_CORE_STM32F103_PLATFORM_HPP
#define P05_CORE_STM32F103_PLATFORM_HPP

#include <atomic>
#include <cstdint>

#include "platform_controller.hpp"
#include "stm32f103_i2c1.hpp"

namespace p05::stm32 {

// Concrete STM32F103C8T6 Platform Adapter implementing IPlatformHw via direct CMSIS registers
// (zero STM32Cube HAL dependencies except optional isolated stm32f103_i2c1.cpp per OPEN-22).
class Stm32F103PlatformHw final : public IPlatformHw {
public:
    Stm32F103PlatformHw() noexcept = default;

    // Called from SysTick_Handler (1 ms uptime clock, OPEN-5).
    static void isr_on_systick() noexcept;
    static uint32_t static_uptime_ms() noexcept;

    uint32_t uptime_ms() const noexcept override;
    uint32_t dwt_cycles() const noexcept override;

    void set_clock_profile(PowerClockProfile profile) noexcept override;
    void enter_sleep_wfi() noexcept override;
    void configure_unused_gpio_analog_keep_pb12() noexcept override;
    void enter_stop_mode_wfi() noexcept override;

    void set_afe_enable(bool enable) noexcept override;
    bool read_lod_p_high() const noexcept override;
    bool read_lod_n_high() const noexcept override;

    void start_ecg_adc_dma(int16_t* dst_490, uint16_t fs_hz) noexcept override;
    void stop_ecg_adc_dma() noexcept override;

    void set_bat_sense_enable(bool enable) noexcept override;
    bool read_bat_sense_adc(uint16_t& raw_adc_out) noexcept override;

    void set_spi1_pins_analog() noexcept override;
    void set_sd_power_enable(bool enable) noexcept override;
    bool sd_init_card() noexcept override;
    bool sd_write_sector(uint32_t lba,
                         const uint8_t* data_512,
                         uint32_t& duration_ms_out) noexcept override;

    Ina219RawTransaction ina219_read() noexcept override;
    bool ina219_power_down() noexcept override;

    void uart1_send(ConstByteView frame) noexcept override;

    void init_base_peripherals() noexcept;

private:
    uint8_t spi1_xfer(uint8_t tx) noexcept;
    uint8_t sd_send_cmd(uint8_t cmd, uint32_t arg, uint8_t crc) noexcept;
    void sd_cs_low() noexcept;
    void sd_cs_high() noexcept;
    void configure_uart1_baud(uint32_t pclk2_hz) noexcept;
    void enable_dwt_cyccnt() noexcept;
    void configure_exti12_pb12() noexcept;

    static std::atomic<uint32_t> s_uptime_ms_;
    uint32_t sysclk_hz_ = 8000000U;
    uint32_t pclk1_hz_ = 8000000U;
    uint32_t pclk2_hz_ = 8000000U;
    bool sd_is_sdhc_ = true;
    Stm32I2c1Ina219 ina219_{};
};

}  // namespace p05::stm32

#endif  // P05_CORE_STM32F103_PLATFORM_HPP
