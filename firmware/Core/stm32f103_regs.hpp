#ifndef P05_CORE_STM32F103_REGS_HPP
#define P05_CORE_STM32F103_REGS_HPP

#include <cstdint>

// Minimal, self-contained RM0008 / ARMv7-M CMSIS-style register map for STM32F103C8T6.
// Zero STM32Cube HAL/LL includes (R-PLT-01 / OPEN-22).

namespace p05::stm32 {

struct RccRegs {
    volatile uint32_t CR;
    volatile uint32_t CFGR;
    volatile uint32_t CIR;
    volatile uint32_t APB2RSTR;
    volatile uint32_t APB1RSTR;
    volatile uint32_t AHBENR;
    volatile uint32_t APB2ENR;
    volatile uint32_t APB1ENR;
    volatile uint32_t BDCR;
    volatile uint32_t CSR;
};

struct FlashRegs {
    volatile uint32_t ACR;
};

struct PwrRegs {
    volatile uint32_t CR;
    volatile uint32_t CSR;
};

struct GpioRegs {
    volatile uint32_t CRL;
    volatile uint32_t CRH;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t BRR;
    volatile uint32_t LCKR;
};

struct AfioRegs {
    volatile uint32_t EVCR;
    volatile uint32_t MAPR;
    volatile uint32_t EXTICR[4];
};

struct ExtiRegs {
    volatile uint32_t IMR;
    volatile uint32_t EMR;
    volatile uint32_t RTSR;
    volatile uint32_t FTSR;
    volatile uint32_t SWIER;
    volatile uint32_t PR;
};

struct TimRegs {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SMCR;
    volatile uint32_t DIER;
    volatile uint32_t SR;
    volatile uint32_t EGR;
    volatile uint32_t CCMR1;
    volatile uint32_t CCMR2;
    volatile uint32_t CCER;
    volatile uint32_t CNT;
    volatile uint32_t PSC;
    volatile uint32_t ARR;
};

struct AdcRegs {
    volatile uint32_t SR;
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SMPR1;
    volatile uint32_t SMPR2;
    volatile uint32_t JOFR[4];
    volatile uint32_t HTR;
    volatile uint32_t LTR;
    volatile uint32_t SQR1;
    volatile uint32_t SQR2;
    volatile uint32_t SQR3;
    volatile uint32_t JSQR;
    volatile uint32_t JDR[4];
    volatile uint32_t DR;
};

struct DmaChannelRegs {
    volatile uint32_t CCR;
    volatile uint32_t CNDTR;
    volatile uint32_t CPAR;
    volatile uint32_t CMAR;
    volatile uint32_t RESERVED;
};

struct DmaRegs {
    volatile uint32_t ISR;
    volatile uint32_t IFCR;
    DmaChannelRegs CH[7];
};

struct SpiRegs {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SR;
    volatile uint32_t DR;
};

struct I2cRegs {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t OAR1;
    volatile uint32_t OAR2;
    volatile uint32_t DR;
    volatile uint32_t SR1;
    volatile uint32_t SR2;
    volatile uint32_t CCR;
    volatile uint32_t TRISE;
};

struct UsartRegs {
    volatile uint32_t SR;
    volatile uint32_t DR;
    volatile uint32_t BRR;
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t CR3;
};

struct SysTickRegs {
    volatile uint32_t CTRL;
    volatile uint32_t LOAD;
    volatile uint32_t VAL;
    volatile uint32_t CALIB;
};

struct ScbRegs {
    volatile uint32_t CPUID;
    volatile uint32_t ICSR;
    volatile uint32_t VTOR;
    volatile uint32_t AIRCR;
    volatile uint32_t SCR;
};

struct DwtRegs {
    volatile uint32_t CTRL;
    volatile uint32_t CYCCNT;
};

// Peripheral base addresses (STM32F103C8T6 RM0008 + ARMv7-M PPB)
inline constexpr uintptr_t kRccBase = 0x40021000UL;
inline constexpr uintptr_t kFlashBase = 0x40022000UL;
inline constexpr uintptr_t kPwrBase = 0x40007000UL;
inline constexpr uintptr_t kGpioABase = 0x40010800UL;
inline constexpr uintptr_t kGpioBBase = 0x40010C00UL;
inline constexpr uintptr_t kGpioCBase = 0x40011000UL;
inline constexpr uintptr_t kAfioBase = 0x40010000UL;
inline constexpr uintptr_t kExtiBase = 0x40010400UL;
inline constexpr uintptr_t kTim3Base = 0x40000400UL;
inline constexpr uintptr_t kAdc1Base = 0x40012400UL;
inline constexpr uintptr_t kDma1Base = 0x40020000UL;
inline constexpr uintptr_t kSpi1Base = 0x40013000UL;
inline constexpr uintptr_t kI2c1Base = 0x40005400UL;
inline constexpr uintptr_t kUsart1Base = 0x40013800UL;

inline constexpr uintptr_t kSysTickBase = 0xE000E010UL;
inline constexpr uintptr_t kNvicIser0Addr = 0xE000E100UL;
inline constexpr uintptr_t kNvicIser1Addr = 0xE000E104UL;
inline constexpr uintptr_t kScbBase = 0xE000ED00UL;
inline constexpr uintptr_t kCoreDebugDemcrAddr = 0xE000EDFCUL;
inline constexpr uintptr_t kDwtBase = 0xE0001000UL;

inline RccRegs* rcc() noexcept { return reinterpret_cast<RccRegs*>(kRccBase); }
inline FlashRegs* flash() noexcept { return reinterpret_cast<FlashRegs*>(kFlashBase); }
inline PwrRegs* pwr() noexcept { return reinterpret_cast<PwrRegs*>(kPwrBase); }
inline GpioRegs* gpioa() noexcept { return reinterpret_cast<GpioRegs*>(kGpioABase); }
inline GpioRegs* gpiob() noexcept { return reinterpret_cast<GpioRegs*>(kGpioBBase); }
inline GpioRegs* gpioc() noexcept { return reinterpret_cast<GpioRegs*>(kGpioCBase); }
inline AfioRegs* afio() noexcept { return reinterpret_cast<AfioRegs*>(kAfioBase); }
inline ExtiRegs* exti() noexcept { return reinterpret_cast<ExtiRegs*>(kExtiBase); }
inline TimRegs* tim3() noexcept { return reinterpret_cast<TimRegs*>(kTim3Base); }
inline AdcRegs* adc1() noexcept { return reinterpret_cast<AdcRegs*>(kAdc1Base); }
inline DmaRegs* dma1() noexcept { return reinterpret_cast<DmaRegs*>(kDma1Base); }
inline SpiRegs* spi1() noexcept { return reinterpret_cast<SpiRegs*>(kSpi1Base); }
inline I2cRegs* i2c1() noexcept { return reinterpret_cast<I2cRegs*>(kI2c1Base); }
inline UsartRegs* usart1() noexcept { return reinterpret_cast<UsartRegs*>(kUsart1Base); }
inline SysTickRegs* systick() noexcept { return reinterpret_cast<SysTickRegs*>(kSysTickBase); }
inline ScbRegs* scb() noexcept { return reinterpret_cast<ScbRegs*>(kScbBase); }
inline DwtRegs* dwt() noexcept { return reinterpret_cast<DwtRegs*>(kDwtBase); }

inline volatile uint32_t* nvic_iser0() noexcept {
    return reinterpret_cast<volatile uint32_t*>(kNvicIser0Addr);
}
inline volatile uint32_t* nvic_iser1() noexcept {
    return reinterpret_cast<volatile uint32_t*>(kNvicIser1Addr);
}
inline volatile uint32_t* coredebug_demcr() noexcept {
    return reinterpret_cast<volatile uint32_t*>(kCoreDebugDemcrAddr);
}

// Helper: set 4-bit CRL/CRH mode+cnf nibble for pin [0..15]
inline void gpio_set_mode(GpioRegs* port, uint8_t pin, uint32_t cnf_mode_4bit) noexcept {
    if (pin < 8U) {
        const uint32_t shift = static_cast<uint32_t>(pin) * 4U;
        port->CRL = (port->CRL & ~(0xFUL << shift)) | ((cnf_mode_4bit & 0xFUL) << shift);
    } else {
        const uint32_t shift = static_cast<uint32_t>(pin - 8U) * 4U;
        port->CRH = (port->CRH & ~(0xFUL << shift)) | ((cnf_mode_4bit & 0xFUL) << shift);
    }
}

// Common 4-bit CNF|MODE encodings (RM0008 Table 20)
inline constexpr uint32_t kGpioAnalog = 0x0U;       // Analog input (0 uA Stop Mode)
inline constexpr uint32_t kGpioInputFloat = 0x4U;   // Input floating (PB12 external 100k pull-up)
inline constexpr uint32_t kGpioOutPp2MHz = 0x2U;    // Output push-pull 2 MHz
inline constexpr uint32_t kGpioOutOd2MHz = 0x6U;    // Output open-drain 2 MHz
inline constexpr uint32_t kGpioAfPp50MHz = 0xBU;    // Alternate function push-pull 50 MHz
inline constexpr uint32_t kGpioAfOd50MHz = 0xFU;    // Alternate function open-drain 50 MHz (I2C1)

}  // namespace p05::stm32

#endif  // P05_CORE_STM32F103_REGS_HPP
