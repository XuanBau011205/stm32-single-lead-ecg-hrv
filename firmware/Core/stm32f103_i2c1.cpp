#include "stm32f103_i2c1.hpp"
#include "stm32f103_regs.hpp"

// OPEN-22 / R-PLT-01 approved sole exception for STM32F1 I2C1 (ES096 Rev 15 section 2.8.7).
#ifdef P05_USE_HAL_I2C
#include "stm32f1xx_hal.h"
#endif

namespace p05::stm32 {

namespace {

inline constexpr uint32_t kI2cCr1Pe = 1U << 0U;
inline constexpr uint32_t kI2cCr1Start = 1U << 8U;
inline constexpr uint32_t kI2cCr1Stop = 1U << 9U;
inline constexpr uint32_t kI2cCr1Ack = 1U << 10U;
inline constexpr uint32_t kI2cCr1Swrst = 1U << 15U;

inline constexpr uint32_t kI2cSr1Sb = 1U << 0U;
inline constexpr uint32_t kI2cSr1Addr = 1U << 1U;
inline constexpr uint32_t kI2cSr1Btf = 1U << 2U;
inline constexpr uint32_t kI2cSr1Rxne = 1U << 6U;
inline constexpr uint32_t kI2cSr1Txe = 1U << 7U;
inline constexpr uint32_t kI2cSr1Af = 1U << 10U;

void tiny_delay() noexcept {
    for (volatile uint32_t i = 0U; i < 40U; ++i) {
    }
}

}  // namespace

void Stm32I2c1Ina219::bus_recovery_es096_9clocks() noexcept {
    // Workaround for STM32F1 errata ES096 section 2.8.7 (stuck BUSY flag):
    // Temporarily drive PB6 (SCL) & PB7 (SDA) as Open-Drain GPIO, pulse 9 clocks, then SWRST.
    i2c1()->CR1 &= ~kI2cCr1Pe;
    gpio_set_mode(gpiob(), 6U, kGpioOutOd2MHz);
    gpio_set_mode(gpiob(), 7U, kGpioOutOd2MHz);
    gpiob()->BSRR = (1U << 6U) | (1U << 7U);  // release high
    tiny_delay();

    for (uint8_t clk = 0U; clk < 9U; ++clk) {
        gpiob()->BRR = (1U << 6U);
        tiny_delay();
        gpiob()->BSRR = (1U << 6U);
        tiny_delay();
    }
    // Generate STOP condition manually
    gpiob()->BRR = (1U << 7U);
    tiny_delay();
    gpiob()->BSRR = (1U << 7U);
    tiny_delay();

    // Hand PB6/PB7 back to AF Open-Drain and toggle SWRST
    gpio_set_mode(gpiob(), 6U, kGpioAfOd50MHz);
    gpio_set_mode(gpiob(), 7U, kGpioAfOd50MHz);
    i2c1()->CR1 |= kI2cCr1Swrst;
    tiny_delay();
    i2c1()->CR1 &= ~kI2cCr1Swrst;
}

void Stm32I2c1Ina219::init(uint32_t pclk1_hz, UptimeFn uptime_fn) noexcept {
    pclk1_hz_ = pclk1_hz;
    uptime_fn_ = uptime_fn;

    rcc()->APB2ENR |= (1U << 3U);   // IOPBEN
    rcc()->APB1ENR |= (1U << 21U);  // I2C1EN

    bus_recovery_es096_9clocks();

    const uint32_t freq_mhz = pclk1_hz_ / 1000000U;
    i2c1()->CR2 = freq_mhz & 0x3FU;
    // Standard mode 100 kHz: CCR = PCLK1 / (2 * 100kHz)
    const uint32_t ccr = pclk1_hz_ / 200000U;
    i2c1()->CCR = (ccr < 4U) ? 4U : ccr;
    i2c1()->TRISE = (freq_mhz + 1U) & 0x3FU;
    i2c1()->CR1 |= kI2cCr1Pe;

    // Program 100 uA/LSB calibration for 0.1 Ohm shunt (R_SHUNT on P05 PCB)
    (void)write_reg16(kRegCalibration, kCalVal100uA, kI2cTimeoutMs);
    (void)wake_continuous();
}

bool Stm32I2c1Ina219::write_reg16(uint8_t reg, uint16_t val, uint32_t timeout_ms) noexcept {
    const uint32_t t0 = (uptime_fn_ != nullptr) ? uptime_fn_() : 0U;
    auto timed_out = [&]() noexcept {
        return (uptime_fn_ != nullptr) && ((uptime_fn_() - t0) > timeout_ms);
    };

    i2c1()->SR1 &= ~kI2cSr1Af;
    i2c1()->CR1 |= kI2cCr1Start;
    while ((i2c1()->SR1 & kI2cSr1Sb) == 0U) {
        if (timed_out()) {
            i2c1()->CR1 |= kI2cCr1Stop;
            return false;
        }
    }

    i2c1()->DR = static_cast<uint32_t>(kIna219Addr7 << 1U);
    while ((i2c1()->SR1 & (kI2cSr1Addr | kI2cSr1Af)) == 0U) {
        if (timed_out()) {
            i2c1()->CR1 |= kI2cCr1Stop;
            return false;
        }
    }
    if ((i2c1()->SR1 & kI2cSr1Af) != 0U) {
        i2c1()->SR1 &= ~kI2cSr1Af;
        i2c1()->CR1 |= kI2cCr1Stop;
        return false;
    }
    (void)i2c1()->SR2;  // Clear ADDR

    const uint8_t payload[3] = {
        reg,
        static_cast<uint8_t>((val >> 8U) & 0xFFU),
        static_cast<uint8_t>(val & 0xFFU),
    };
    for (uint8_t b : payload) {
        while ((i2c1()->SR1 & kI2cSr1Txe) == 0U) {
            if (timed_out()) {
                i2c1()->CR1 |= kI2cCr1Stop;
                return false;
            }
        }
        i2c1()->DR = b;
    }

    while ((i2c1()->SR1 & kI2cSr1Btf) == 0U) {
        if (timed_out()) {
            i2c1()->CR1 |= kI2cCr1Stop;
            return false;
        }
    }
    i2c1()->CR1 |= kI2cCr1Stop;
    return true;
}

bool Stm32I2c1Ina219::read_reg16(uint8_t reg, uint16_t& val_out, uint32_t timeout_ms) noexcept {
    const uint32_t t0 = (uptime_fn_ != nullptr) ? uptime_fn_() : 0U;
    auto timed_out = [&]() noexcept {
        return (uptime_fn_ != nullptr) && ((uptime_fn_() - t0) > timeout_ms);
    };

    // Phase 1: Write register pointer
    i2c1()->SR1 &= ~kI2cSr1Af;
    i2c1()->CR1 |= kI2cCr1Start;
    while ((i2c1()->SR1 & kI2cSr1Sb) == 0U) {
        if (timed_out()) {
            i2c1()->CR1 |= kI2cCr1Stop;
            return false;
        }
    }
    i2c1()->DR = static_cast<uint32_t>(kIna219Addr7 << 1U);
    while ((i2c1()->SR1 & (kI2cSr1Addr | kI2cSr1Af)) == 0U) {
        if (timed_out()) {
            i2c1()->CR1 |= kI2cCr1Stop;
            return false;
        }
    }
    if ((i2c1()->SR1 & kI2cSr1Af) != 0U) {
        i2c1()->SR1 &= ~kI2cSr1Af;
        i2c1()->CR1 |= kI2cCr1Stop;
        return false;
    }
    (void)i2c1()->SR2;
    i2c1()->DR = reg;
    while ((i2c1()->SR1 & kI2cSr1Btf) == 0U) {
        if (timed_out()) {
            i2c1()->CR1 |= kI2cCr1Stop;
            return false;
        }
    }

    // Phase 2: Repeated START + Read 2 bytes (MSB, LSB)
    i2c1()->CR1 |= kI2cCr1Ack | kI2cCr1Start;
    while ((i2c1()->SR1 & kI2cSr1Sb) == 0U) {
        if (timed_out()) {
            i2c1()->CR1 |= kI2cCr1Stop;
            return false;
        }
    }
    i2c1()->DR = static_cast<uint32_t>((kIna219Addr7 << 1U) | 1U);
    while ((i2c1()->SR1 & (kI2cSr1Addr | kI2cSr1Af)) == 0U) {
        if (timed_out()) {
            i2c1()->CR1 |= kI2cCr1Stop;
            return false;
        }
    }
    if ((i2c1()->SR1 & kI2cSr1Af) != 0U) {
        i2c1()->SR1 &= ~kI2cSr1Af;
        i2c1()->CR1 |= kI2cCr1Stop;
        return false;
    }
    (void)i2c1()->SR2;

    while ((i2c1()->SR1 & kI2cSr1Rxne) == 0U) {
        if (timed_out()) {
            i2c1()->CR1 |= kI2cCr1Stop;
            return false;
        }
    }
    const uint8_t msb = static_cast<uint8_t>(i2c1()->DR & 0xFFU);

    i2c1()->CR1 &= ~kI2cCr1Ack;
    i2c1()->CR1 |= kI2cCr1Stop;
    while ((i2c1()->SR1 & kI2cSr1Rxne) == 0U) {
        if (timed_out()) {
            return false;
        }
    }
    const uint8_t lsb = static_cast<uint8_t>(i2c1()->DR & 0xFFU);

    val_out = static_cast<uint16_t>((static_cast<uint16_t>(msb) << 8U) | lsb);
    return true;
}

Ina219RawTransaction Stm32I2c1Ina219::read_telemetry() noexcept {
    Ina219RawTransaction tx{};
    const uint32_t t0 = (uptime_fn_ != nullptr) ? uptime_fn_() : 0U;

    uint16_t bus_raw = 0U;
    uint16_t cur_raw = 0U;
    const bool ok1 = read_reg16(kRegBusVoltage, bus_raw, kI2cTimeoutMs);
    const bool ok2 = ok1 && read_reg16(kRegCurrent, cur_raw, kI2cTimeoutMs);

    tx.transaction_ms = (uptime_fn_ != nullptr) ? (uptime_fn_() - t0) : 0U;
    tx.i2c_ok = ok2;
    if (ok2) {
        tx.bus_mv = static_cast<uint16_t>((bus_raw >> 3U) * 4U);
        tx.current_ua = static_cast<int32_t>(static_cast<int16_t>(cur_raw)) * 100;
    }
    return tx;
}

bool Stm32I2c1Ina219::power_down() noexcept {
    return write_reg16(kRegConfig, 0x0000U, kI2cTimeoutMs);
}

bool Stm32I2c1Ina219::wake_continuous() noexcept {
    // 16V bus range, +/-40mV PGA, 12-bit ADC, continuous shunt & bus (0x019F)
    return write_reg16(kRegConfig, 0x019FU, kI2cTimeoutMs);
}

}  // namespace p05::stm32
