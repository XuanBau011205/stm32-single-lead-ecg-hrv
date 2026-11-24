#include "stm32f103_platform.hpp"

#include "stm32f103_regs.hpp"

namespace p05::stm32 {

namespace {

void spin_cycles(uint32_t iters) noexcept {
    for (volatile uint32_t i = 0U; i < iters; ++i) {
    }
}

bool wait_reg_bits(volatile uint32_t& reg,
                   uint32_t mask,
                   uint32_t expected,
                   uint32_t max_iters) noexcept {
    for (uint32_t w = 0U; w < max_iters; ++w) {
        if ((reg & mask) == expected) {
            return true;
        }
    }
    return false;
}

void calibrate_adc1() noexcept {
    // RM0008 section 11.4: wake ADC1, wait t_STAB, reset calibration, then run calibration
    adc1()->CR2 |= (1U << 0U);  // ADON = 1
    spin_cycles(250U);
    adc1()->CR2 |= (1U << 3U);  // RSTCAL
    for (uint32_t w = 0U; w < 100000U && (adc1()->CR2 & (1U << 3U)) != 0U; ++w) {
    }
    adc1()->CR2 |= (1U << 2U);  // CAL
    for (uint32_t w = 0U; w < 100000U && (adc1()->CR2 & (1U << 2U)) != 0U; ++w) {
    }
}

void wait_spi1_not_busy() noexcept {
    for (uint32_t w = 0U; w < 100000U && (spi1()->SR & (1U << 7U)) != 0U; ++w) {
    }
}

}  // namespace

std::atomic<uint32_t> Stm32F103PlatformHw::s_uptime_ms_{0U};

void Stm32F103PlatformHw::isr_on_systick() noexcept {
    s_uptime_ms_.fetch_add(1U, std::memory_order_relaxed);
}

uint32_t Stm32F103PlatformHw::static_uptime_ms() noexcept {
    return s_uptime_ms_.load(std::memory_order_relaxed);
}

uint32_t Stm32F103PlatformHw::uptime_ms() const noexcept {
    return static_uptime_ms();
}

uint32_t Stm32F103PlatformHw::dwt_cycles() const noexcept {
    return dwt()->CYCCNT;
}

void Stm32F103PlatformHw::enable_dwt_cyccnt() noexcept {
    *coredebug_demcr() |= (1U << 24U);  // TRCENA
    dwt()->CYCCNT = 0U;
    dwt()->CTRL |= (1U << 0U);          // CYCCNTENA
}

void Stm32F103PlatformHw::configure_uart1_baud(uint32_t pclk2_hz) noexcept {
    // OPEN-11: 115200 bps, 8-N-1 (0% error @ 72 MHz, +0.64% @ 8 MHz)
    rcc()->APB2ENR |= (1U << 2U) | (1U << 14U);   // IOPAEN | USART1EN
    gpio_set_mode(gpioa(), 9U, kGpioAfPp50MHz);    // PA9 TX
    gpio_set_mode(gpioa(), 10U, kGpioInputPull);   // PA10 RX with internal pull-up (prevents floating EMI)
    gpioa()->BSRR = (1U << 10U);                   // Pull-up HIGH when cable disconnected
    usart1()->BRR = (pclk2_hz + 57600U) / 115200U;
    usart1()->CR1 = (1U << 13U) | (1U << 5U) | (1U << 3U) | (1U << 2U);  // UE | RXNEIE | TE | RE
    *nvic_iser1() = (1U << 5U);  // IRQ37 = USART1
}

void Stm32F103PlatformHw::configure_exti12_pb12() noexcept {
    // OPEN-23: SW_WAKE on PB12 (EXTI12 falling edge, external 100k pull-up -> 0 uA)
    rcc()->APB2ENR |= (1U << 0U) | (1U << 3U);  // AFIOEN | IOPBEN
    gpio_set_mode(gpiob(), 12U, kGpioInputFloat);
    afio()->EXTICR[3] = (afio()->EXTICR[3] & ~0x000FU) | 0x0001U;  // EXTI12 -> Port B
    exti()->FTSR |= (1U << 12U);
    exti()->RTSR &= ~(1U << 12U);
    exti()->IMR |= (1U << 12U);
    *nvic_iser1() = (1U << 8U);  // IRQ40 = EXTI15_10
}

void Stm32F103PlatformHw::init_base_peripherals() noexcept {
    rcc()->APB2ENR |= (1U << 0U) | (1U << 2U) | (1U << 3U) | (1U << 4U);  // AFIO, A, B, C
    rcc()->APB1ENR |= (1U << 28U);                                         // PWREN

    // PA0 = ECG_OUT (Analog), PA1/PA2 = LOD+/LOD- (Input), PA3 = ECG_SDN (Out PP, default LOW)
    gpio_set_mode(gpioa(), 0U, kGpioAnalog);
    gpio_set_mode(gpioa(), 1U, kGpioInputFloat);
    gpio_set_mode(gpioa(), 2U, kGpioInputFloat);
    gpio_set_mode(gpioa(), 3U, kGpioOutPp2MHz);
    gpioa()->BRR = (1U << 3U);

    // PA8 = TP_MCO (HSE 8 MHz diagnostic output for Gate G3 bring-up)
    gpio_set_mode(gpioa(), 8U, kGpioAfPp50MHz);

    // PB0 = SD_PWR_EN (AO3401 active-low, default HIGH = OFF), PB1 = BAT_SENSE (Analog),
    // PB5 = BAT_SENSE_EN (2N7002 active-high, default LOW = OFF)
    gpio_set_mode(gpiob(), 0U, kGpioOutPp2MHz);
    gpiob()->BSRR = (1U << 0U);
    gpio_set_mode(gpiob(), 1U, kGpioAnalog);
    gpio_set_mode(gpiob(), 5U, kGpioOutPp2MHz);
    gpiob()->BRR = (1U << 5U);

    configure_exti12_pb12();
    enable_dwt_cyccnt();
}

void Stm32F103PlatformHw::set_clock_profile(PowerClockProfile profile) noexcept {
    init_base_peripherals();

    // Enable 8 MHz HSE and wait for HSERDY (bounded)
    rcc()->CR |= (1U << 16U);
    if (!wait_reg_bits(rcc()->CR, (1U << 17U), (1U << 17U), 500000U)) {
        return;
    }

    if (profile == PowerClockProfile::Run72MHzSoftFloat) {
        // Switch away from PLL before reconfiguring PLL, then set 2 Flash WS before raising SYSCLK
        if (((rcc()->CFGR >> 2U) & 0x3U) == 0x2U) {
            rcc()->CFGR = (rcc()->CFGR & ~0x3U) | 0x1U;  // SW = HSE (8 MHz)
            (void)wait_reg_bits(rcc()->CFGR, (0x3U << 2U), (0x1U << 2U), 500000U);
        }
        rcc()->CR &= ~(1U << 24U);  // PLLOFF before changing PLLMUL/PLLSRC
        (void)wait_reg_bits(rcc()->CR, (1U << 25U), 0U, 500000U);

        // 2 Flash wait states + prefetch, PPRE1 = /2 (36 MHz), ADCPRE = /6 (12 MHz),
        // PLL = HSE x 9 = 72 MHz, MCO = HSE (8 MHz)
        flash()->ACR = 0x12U;
        rcc()->CFGR =
            (0x6U << 24U) | (0x7U << 18U) | (1U << 16U) | (0x2U << 14U) | (0x4U << 8U) | 0x1U;
        rcc()->CR |= (1U << 24U);  // PLLON
        if (wait_reg_bits(rcc()->CR, (1U << 25U), (1U << 25U), 500000U)) {
            rcc()->CFGR = (rcc()->CFGR & ~0x3U) | 0x2U;  // SW = PLL
            (void)wait_reg_bits(rcc()->CFGR, (0x3U << 2U), (0x2U << 2U), 500000U);
        }
        sysclk_hz_ = 72000000U;
        pclk1_hz_ = 36000000U;
        pclk2_hz_ = 72000000U;
    } else {
        // Switch SYSCLK to HSE 8 MHz FIRST, then disable PLL, THEN reduce Flash ACR to 0 WS (CRITICAL-03)
        rcc()->CFGR = (0x6U << 24U) | 0x1U;  // MCO = HSE, SW = HSE
        (void)wait_reg_bits(rcc()->CFGR, (0x3U << 2U), (0x1U << 2U), 500000U);
        rcc()->CR &= ~(1U << 24U);  // PLLOFF
        (void)wait_reg_bits(rcc()->CR, (1U << 25U), 0U, 500000U);
        flash()->ACR = 0x10U;  // 0 Flash wait states + prefetch enabled (safe at 8 MHz)
        sysclk_hz_ = 8000000U;
        pclk1_hz_ = 8000000U;
        pclk2_hz_ = 8000000U;
    }

    // SysTick 1 ms uptime clock (OPEN-5)
    systick()->LOAD = (sysclk_hz_ / 1000U) - 1U;
    systick()->VAL = 0U;
    systick()->CTRL = 0x7U;  // CLKSOURCE=HCLK | TICKINT | ENABLE

    configure_uart1_baud(pclk2_hz_);
    ina219_.init(pclk1_hz_, &Stm32F103PlatformHw::static_uptime_ms);
}

void Stm32F103PlatformHw::enter_sleep_wfi() noexcept {
    scb()->SCR &= ~(1U << 2U);  // SLEEPDEEP = 0 (Sleep Mode)
#if defined(__arm__) || defined(__thumb__)
    asm volatile("wfi" ::: "memory");
#endif
}

void Stm32F103PlatformHw::configure_unused_gpio_analog_keep_pb12() noexcept {
    // Disable USART1 and I2C1 before switching PA9/PA10 and PB6/PB7 to Analog Input
    // so floating/pulled lines cannot fire spurious UART RX break IRQs or I2C glitches.
    usart1()->CR1 = 0U;
    i2c1()->CR1 = 0U;

    // Put all unused pins on Port A, B, C to Analog Input (0 uA Stop Mode leak).
    // Preserve PA3 (ECG_SDN=LOW), PB0 (SD_PWR_EN=HIGH), PB5 (BAT_SENSE_EN=LOW),
    // PB12 (SW_WAKE EXTI12 floating with external 100k pull-up), PA13/PA14 (SWD).
    gpio_set_mode(gpioa(), 0U, kGpioAnalog);
    gpio_set_mode(gpioa(), 1U, kGpioAnalog);
    gpio_set_mode(gpioa(), 2U, kGpioAnalog);
    gpio_set_mode(gpioa(), 4U, kGpioAnalog);
    gpio_set_mode(gpioa(), 5U, kGpioAnalog);
    gpio_set_mode(gpioa(), 6U, kGpioAnalog);
    gpio_set_mode(gpioa(), 7U, kGpioAnalog);
    gpio_set_mode(gpioa(), 8U, kGpioAnalog);
    gpio_set_mode(gpioa(), 9U, kGpioAnalog);
    gpio_set_mode(gpioa(), 10U, kGpioAnalog);
    gpio_set_mode(gpioa(), 11U, kGpioAnalog);
    gpio_set_mode(gpioa(), 12U, kGpioAnalog);
    gpio_set_mode(gpioa(), 15U, kGpioAnalog);

    gpio_set_mode(gpiob(), 1U, kGpioAnalog);
    gpio_set_mode(gpiob(), 2U, kGpioAnalog);
    gpio_set_mode(gpiob(), 3U, kGpioAnalog);
    gpio_set_mode(gpiob(), 4U, kGpioAnalog);
    gpio_set_mode(gpiob(), 6U, kGpioAnalog);
    gpio_set_mode(gpiob(), 7U, kGpioAnalog);
    gpio_set_mode(gpiob(), 8U, kGpioAnalog);
    gpio_set_mode(gpiob(), 9U, kGpioAnalog);
    gpio_set_mode(gpiob(), 10U, kGpioAnalog);
    gpio_set_mode(gpiob(), 11U, kGpioAnalog);
    gpio_set_mode(gpiob(), 13U, kGpioAnalog);
    gpio_set_mode(gpiob(), 14U, kGpioAnalog);
    gpio_set_mode(gpiob(), 15U, kGpioAnalog);

    gpioc()->CRL = 0U;
    gpioc()->CRH = 0U;
}

void Stm32F103PlatformHw::enter_stop_mode_wfi() noexcept {
    // Enable PWREN | BKPEN, allow Backup Domain write (DBP), and enable LSE 32.768 kHz RTC (SPEC 4.A).
    // PWR->CR: PDDS = 0 (Stop Mode), LPDS = 1 (Low-Power Regulator) -> 24 uA MCU Stop.
    rcc()->APB1ENR |= (1U << 28U) | (1U << 27U);
    pwr()->CR = (pwr()->CR & ~(1U << 1U)) | (1U << 0U) | (1U << 8U);
    rcc()->BDCR |= (1U << 0U);  // LSEON
    if (wait_reg_bits(rcc()->BDCR, (1U << 1U), (1U << 1U), 500000U)) {
        if ((rcc()->BDCR & (0x3U << 8U)) == 0U) {
            rcc()->BDCR |= (0x1U << 8U);  // RTCSEL = LSE
        }
        rcc()->BDCR |= (1U << 15U);  // RTCEN
    }

    // Disable SysTick TICKINT so SysTick cannot wake WFI; clear pending EXTI12 before sleep.
    const uint32_t prev_systick_ctrl = systick()->CTRL;
    systick()->CTRL = prev_systick_ctrl & ~(1U << 1U);
    exti()->PR = (1U << 12U);

    scb()->SCR |= (1U << 2U);  // SLEEPDEEP = 1
#if defined(__arm__) || defined(__thumb__)
    asm volatile("dsb" ::: "memory");
    asm volatile("wfi" ::: "memory");
#endif
    scb()->SCR &= ~(1U << 2U);
    systick()->CTRL = prev_systick_ctrl;
}

void Stm32F103PlatformHw::set_afe_enable(bool enable) noexcept {
    if (enable) {
        gpioa()->BSRR = (1U << 3U);
    } else {
        gpioa()->BRR = (1U << 3U);
    }
}

bool Stm32F103PlatformHw::read_lod_p_high() const noexcept {
    return (gpioa()->IDR & (1U << 1U)) != 0U;
}

bool Stm32F103PlatformHw::read_lod_n_high() const noexcept {
    return (gpioa()->IDR & (1U << 2U)) != 0U;
}

void Stm32F103PlatformHw::start_ecg_adc_dma(int16_t* dst_490, uint16_t fs_hz) noexcept {
    rcc()->AHBENR |= (1U << 0U);   // DMA1EN
    rcc()->APB2ENR |= (1U << 9U);  // ADC1EN
    rcc()->APB1ENR |= (1U << 1U);  // TIM3EN

    // 1. DMA1 Channel 1: Circular Ping-Pong (490 x 16-bit), HTIE + TCIE
    DmaChannelRegs& ch1 = dma1()->CH[0];
    ch1.CCR = 0U;
    ch1.CPAR = static_cast<uint32_t>(kAdc1Base + 0x4CU);  // &ADC1->DR
    ch1.CMAR = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dst_490));
    ch1.CNDTR = static_cast<uint32_t>(PingPongDmaBuffer::kTotalSamples);
    // MSIZE=16b (01<<10), PSIZE=16b (01<<8), MINC (1<<7), CIRC (1<<5), HTIE (1<<2), TCIE (1<<1), EN
    ch1.CCR = (1U << 10U) | (1U << 8U) | (1U << 7U) | (1U << 5U) | (1U << 2U) | (1U << 1U) | 1U;
    *nvic_iser0() = (1U << 11U);  // IRQ11 = DMA1_Channel1

    // 2. ADC1: power on + self-calibrate, then configure Channel 0 (PA0) + TIM3 TRGO + DMA
    adc1()->CR2 = 0U;
    calibrate_adc1();
    adc1()->SQR1 = 0U;
    adc1()->SQR3 = 0U;            // IN0
    adc1()->SMPR2 = (0x5U << 0U); // 55.5 cycles sample time
    adc1()->SR = 0U;
    adc1()->CR2 = (1U << 20U) | (0x4U << 17U) | (1U << 8U) | (1U << 0U);

    // 3. TIM3 TRGO on Update event at exact fs_hz (250 Hz or 500 Hz)
    const uint32_t tim_clk = (pclk1_hz_ < sysclk_hz_) ? (pclk1_hz_ * 2U) : pclk1_hz_;
    tim3()->CR1 = 0U;
    tim3()->PSC = (tim_clk / 1000000U) - 1U;  // 1 MHz tick
    tim3()->ARR = (1000000U / static_cast<uint32_t>(fs_hz)) - 1U;
    tim3()->CR2 = (0x2U << 4U);  // MMS = 010 (Update -> TRGO)
    tim3()->EGR = 1U;
    tim3()->CR1 = 1U;
}

void Stm32F103PlatformHw::stop_ecg_adc_dma() noexcept {
    tim3()->CR1 &= ~1U;
    adc1()->CR2 &= ~((1U << 20U) | (1U << 8U) | (1U << 0U));
    dma1()->CH[0].CCR &= ~1U;
}

void Stm32F103PlatformHw::set_bat_sense_enable(bool enable) noexcept {
    if (enable) {
        gpiob()->BSRR = (1U << 5U);
        spin_cycles(400U);  // Allow 100k/100k + ADC pin RC settling after 2N7002 turns ON
    } else {
        gpiob()->BRR = (1U << 5U);
    }
}

bool Stm32F103PlatformHw::read_bat_sense_adc(uint16_t& raw_adc_out) noexcept {
    rcc()->APB2ENR |= (1U << 9U);  // ADC1EN
    // Pause TIM3 trigger cleanly so ECG DMA buffer cannot capture BAT_SENSE channel 9
    const uint32_t prev_tim3_cr1 = tim3()->CR1;
    tim3()->CR1 = prev_tim3_cr1 & ~1U;

    const uint32_t prev_cr2 = adc1()->CR2;
    const uint32_t prev_sqr3 = adc1()->SQR3;
    const uint32_t prev_smpr2 = adc1()->SMPR2;

    if ((prev_cr2 & 1U) == 0U) {
        adc1()->CR2 = 0U;
        calibrate_adc1();
    }

    // Configure Channel 9 (PB1) with 239.5 cycles for high-impedance 100k/100k divider, DMA=0
    adc1()->SQR3 = 9U;
    adc1()->SMPR2 = (prev_smpr2 & ~(0x7U << 27U)) | (0x7U << 27U);
    adc1()->SR = 0U;
    adc1()->CR2 = (1U << 20U) | (0x7U << 17U) | (1U << 0U);  // EXTTRIG | SWSTART sel | ADON
    adc1()->CR2 |= (1U << 22U);                              // SWSTART trigger

    const uint32_t t0 = uptime_ms();
    bool ok = true;
    while ((adc1()->SR & (1U << 1U)) == 0U) {
        if ((uptime_ms() - t0) > 10U) {
            ok = false;
            break;
        }
    }
    if (ok) {
        raw_adc_out = static_cast<uint16_t>(adc1()->DR & 0x0FFFU);
    }

    // Restore ADC1 without writing ADON 1->1 (which would inject an unscheduled conversion into DMA)
    adc1()->CR2 = 0U;
    adc1()->SQR3 = prev_sqr3;
    adc1()->SMPR2 = prev_smpr2;
    adc1()->SR = 0U;
    if ((prev_cr2 & 1U) != 0U) {
        adc1()->CR2 = prev_cr2;  // 0->1 wakes ADC from power-down without triggering conversion
        spin_cycles(250U);
    }
    tim3()->CR1 = prev_tim3_cr1;
    return ok;
}

void Stm32F103PlatformHw::set_spi1_pins_analog() noexcept {
    wait_spi1_not_busy();
    spi1()->CR1 &= ~(1U << 6U);  // SPE = 0
    gpio_set_mode(gpioa(), 4U, kGpioAnalog);
    gpio_set_mode(gpioa(), 5U, kGpioAnalog);
    gpio_set_mode(gpioa(), 6U, kGpioAnalog);
    gpio_set_mode(gpioa(), 7U, kGpioAnalog);
}

void Stm32F103PlatformHw::set_sd_power_enable(bool enable) noexcept {
    // PB0 drives P-MOSFET AO3401 Gate: LOW = ON, HIGH = OFF (0 uA leak)
    if (enable) {
        gpiob()->BRR = (1U << 0U);
    } else {
        gpiob()->BSRR = (1U << 0U);
    }
}

void Stm32F103PlatformHw::sd_cs_low() noexcept { gpioa()->BRR = (1U << 4U); }
void Stm32F103PlatformHw::sd_cs_high() noexcept { gpioa()->BSRR = (1U << 4U); }

uint8_t Stm32F103PlatformHw::spi1_xfer(uint8_t tx) noexcept {
    if (!wait_reg_bits(spi1()->SR, (1U << 1U), (1U << 1U), 100000U)) {  // TXE
        return 0xFFU;
    }
    spi1()->DR = tx;
    if (!wait_reg_bits(spi1()->SR, (1U << 0U), (1U << 0U), 100000U)) {  // RXNE
        return 0xFFU;
    }
    return static_cast<uint8_t>(spi1()->DR & 0xFFU);
}

uint8_t Stm32F103PlatformHw::sd_send_cmd(uint8_t cmd, uint32_t arg, uint8_t crc) noexcept {
    (void)spi1_xfer(0xFFU);
    (void)spi1_xfer(static_cast<uint8_t>(0x40U | (cmd & 0x3FU)));
    (void)spi1_xfer(static_cast<uint8_t>((arg >> 24U) & 0xFFU));
    (void)spi1_xfer(static_cast<uint8_t>((arg >> 16U) & 0xFFU));
    (void)spi1_xfer(static_cast<uint8_t>((arg >> 8U) & 0xFFU));
    (void)spi1_xfer(static_cast<uint8_t>(arg & 0xFFU));
    (void)spi1_xfer(crc);

    uint8_t r1 = 0xFFU;
    for (uint8_t retry = 0U; retry < 16U; ++retry) {
        r1 = spi1_xfer(0xFFU);
        if ((r1 & 0x80U) == 0U) {
            break;
        }
    }
    return r1;
}

bool Stm32F103PlatformHw::sd_init_card() noexcept {
    // Configure SPI1 on PA4 (CS), PA5 (SCK), PA6 (MISO pull-up), PA7 (MOSI) (OPEN-21)
    rcc()->APB2ENR |= (1U << 2U) | (1U << 12U);  // IOPAEN | SPI1EN
    gpio_set_mode(gpioa(), 4U, kGpioOutPp2MHz);
    gpio_set_mode(gpioa(), 5U, kGpioAfPp50MHz);
    gpio_set_mode(gpioa(), 6U, kGpioInputPull);
    gpioa()->BSRR = (1U << 6U);  // Pull-up on PA6 MISO per SD SPI spec
    gpio_set_mode(gpioa(), 7U, kGpioAfPp50MHz);
    sd_cs_high();

    // Wait >= 2 ms for SD power-up ramp after AO3401 enables VDD_SD
    const uint32_t pwr_t0 = uptime_ms();
    while ((uptime_ms() - pwr_t0) < 2U) {
    }

    // Init speed <= 400 kHz: BR = /64 (or /256 @ 72 MHz), MSTR | SSM | SSI | SPE
    const uint32_t br = (pclk2_hz_ > 16000000U) ? (0x7U << 3U) : (0x5U << 3U);
    spi1()->CR1 = (1U << 9U) | (1U << 8U) | (1U << 6U) | br | (1U << 2U);

    // 80 dummy clocks with CS HIGH
    for (uint8_t i = 0U; i < 10U; ++i) {
        (void)spi1_xfer(0xFFU);
    }

    // CMD0 -> IDLE (R1 == 0x01)
    sd_cs_low();
    const uint8_t r0 = sd_send_cmd(0U, 0U, 0x95U);
    sd_cs_high();
    if (r0 != 0x01U) {
        return false;
    }

    // CMD8 -> SD v2.0 voltage check (0x000001AA)
    sd_cs_low();
    const uint8_t r8 = sd_send_cmd(8U, 0x000001AAU, 0x87U);
    for (uint8_t i = 0U; i < 4U; ++i) {
        (void)spi1_xfer(0xFFU);
    }
    sd_cs_high();
    if (r8 != 0x01U) {
        return false;
    }

    // CMD55 + ACMD41 (HCS=1 for SDHC, OPEN-16)
    const uint32_t t0 = uptime_ms();
    bool ready = false;
    while ((uptime_ms() - t0) <= 500U) {
        sd_cs_low();
        (void)sd_send_cmd(55U, 0U, 0x65U);
        sd_cs_high();
        sd_cs_low();
        const uint8_t r41 = sd_send_cmd(41U, 0x40000000U, 0x77U);
        sd_cs_high();
        if (r41 == 0x00U) {
            ready = true;
            break;
        }
    }
    if (!ready) {
        return false;
    }

    // CMD58 -> verify OCR CCS bit 30 (bit 6 of OCR[31:24]) confirms SDHC card (OPEN-16)
    sd_cs_low();
    const uint8_t r58 = sd_send_cmd(58U, 0U, 0xFDU);
    const uint8_t ocr0 = spi1_xfer(0xFFU);
    for (uint8_t i = 0U; i < 3U; ++i) {
        (void)spi1_xfer(0xFFU);
    }
    sd_cs_high();
    if (r58 != 0x00U) {
        return false;
    }
    sd_is_sdhc_ = ((ocr0 & 0x40U) != 0U);
    if (!sd_is_sdhc_) {
        return false;
    }

    // Raise SPI1 clock to fast data rate (fPCLK2 / 4) only after SPI1 BSY clears
    wait_spi1_not_busy();
    spi1()->CR1 = (spi1()->CR1 & ~(0x7U << 3U)) | (0x1U << 3U);
    return true;
}

bool Stm32F103PlatformHw::sd_write_sector(uint32_t lba,
                                          const uint8_t* data_512,
                                          uint32_t& duration_ms_out) noexcept {
    const uint32_t t0 = uptime_ms();
    const uint32_t addr = sd_is_sdhc_ ? lba : (lba * 512U);

    sd_cs_low();
    const uint8_t r24 = sd_send_cmd(24U, addr, 0xFFU);
    if (r24 != 0x00U) {
        sd_cs_high();
        duration_ms_out = uptime_ms() - t0;
        return false;
    }

    (void)spi1_xfer(0xFFU);
    (void)spi1_xfer(0xFEU);  // Start Block Token
    for (std::size_t i = 0U; i < kSdBlockBytes; ++i) {
        (void)spi1_xfer(data_512[i]);
    }
    (void)spi1_xfer(0xFFU);  // Dummy SPI CRC
    (void)spi1_xfer(0xFFU);

    const uint8_t resp = spi1_xfer(0xFFU);
    if ((resp & 0x1FU) != 0x05U) {
        sd_cs_high();
        duration_ms_out = uptime_ms() - t0;
        return false;
    }

    // Wait for busy release within OPEN-3 250 ms timeout
    while (spi1_xfer(0xFFU) == 0x00U) {
        if ((uptime_ms() - t0) > kSpiWriteTimeoutMs) {
            sd_cs_high();
            duration_ms_out = uptime_ms() - t0;
            return false;
        }
    }

    sd_cs_high();
    (void)spi1_xfer(0xFFU);
    wait_spi1_not_busy();
    duration_ms_out = uptime_ms() - t0;
    return true;
}

Ina219RawTransaction Stm32F103PlatformHw::ina219_read() noexcept {
    return ina219_.read_telemetry();
}

bool Stm32F103PlatformHw::ina219_power_down() noexcept {
    return ina219_.power_down();
}

void Stm32F103PlatformHw::uart1_send(ConstByteView frame) noexcept {
    if (!frame.valid_pointer()) {
        return;
    }
    for (std::size_t i = 0U; i < frame.length; ++i) {
        if (!wait_reg_bits(usart1()->SR, (1U << 7U), (1U << 7U), 200000U)) {  // TXE
            return;
        }
        usart1()->DR = frame.data[i];
    }
    (void)wait_reg_bits(usart1()->SR, (1U << 6U), (1U << 6U), 200000U);  // TC
}

}  // namespace p05::stm32
