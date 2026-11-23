#ifndef P05_CORE_STM32F103_I2C1_HPP
#define P05_CORE_STM32F103_I2C1_HPP

#include <cstdint>

#include "sensor_storage_boundary.hpp"

namespace p05::stm32 {

// Isolated I2C1 driver for INA219 on PB6 (SCL) / PB7 (SDA), address 0x40.
// Per OPEN-22 / R-PLT-01, stm32f103_i2c1.cpp is the SOLE file allowed to include
// STM32Cube HAL/LL (when P05_USE_HAL_I2C is defined); otherwise runs bare-metal CMSIS
// with STM32F1 errata ES096 section 2.8.7 9-clock GPIO bus recovery.
class Stm32I2c1Ina219 {
public:
    static constexpr uint8_t kIna219Addr7 = 0x40U;
    static constexpr uint8_t kRegConfig = 0x00U;
    static constexpr uint8_t kRegBusVoltage = 0x02U;
    static constexpr uint8_t kRegCurrent = 0x04U;
    static constexpr uint8_t kRegCalibration = 0x05U;
    static constexpr uint16_t kCalVal100uA = 0x1000U;  // 0.1 Ohm shunt -> 100 uA/LSB

    using UptimeFn = uint32_t (*)() noexcept;

    void init(uint32_t pclk1_hz, UptimeFn uptime_fn) noexcept;

    // Reads bus voltage (mV) and shunt current (uA) within OPEN-3 10 ms timeout.
    Ina219RawTransaction read_telemetry() noexcept;

    // Writes Power-Down mode (0x0000) to INA219 Config register (6.0 uA Stop Mode).
    bool power_down() noexcept;

    // Restores continuous shunt+bus conversion mode after waking from Stop Mode.
    bool wake_continuous() noexcept;

private:
    void bus_recovery_es096_9clocks() noexcept;
    bool write_reg16(uint8_t reg, uint16_t val, uint32_t timeout_ms) noexcept;
    bool read_reg16(uint8_t reg, uint16_t& val_out, uint32_t timeout_ms) noexcept;

    uint32_t pclk1_hz_ = 8000000U;
    UptimeFn uptime_fn_ = nullptr;
};

}  // namespace p05::stm32

#endif  // P05_CORE_STM32F103_I2C1_HPP
