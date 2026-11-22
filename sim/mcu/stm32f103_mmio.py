"""
STM32F103MMIO — Register-level peripheral bridge for STM32F103C8T6 (RM0008).

Bridges Unicorn ARM Cortex-M3 MMIO accesses (0x40000000 .. 0x4002FFFF) directly to
the P05VirtualBoard's VirtualPinMatrix and CortexM3Nvic:
  - RCC (0x40021000)   : HSE 8MHz, PLL 72MHz, LSE 32.768kHz, MCO (PA8 -> TP_MCO)
  - FLASH (0x40022000) : ACR wait states
  - PWR (0x40007000)   : CR (PDDS/LPDS), CSR
  - GPIOA (0x40010800) & GPIOB (0x40010C00): CRL/CRH/IDR/ODR/BSRR/BRR + live mode sync
  - AFIO (0x40010000) & EXTI (0x40010400)  : PB12 (SW_WAKE) -> EXTI12 -> IRQ_EXTI15_10
  - TIM2 (0x40000000) & TIM3 (0x40000400)  : Hardware trigger (TRGO) & update IRQ
  - ADC1 (0x40012400) & DMA1 (0x40020000)  : 12-bit PA0/PB1 + Circular Ping-Pong DMA + HT/TC IRQ
  - SPI1 (0x40013000)  : Synchronous transfer to VirtualMicroSD (PA4..PA7)
  - I2C1 (0x40005400)  : Master TX/RX state machine to INA219 @ 0x40 (PB6/PB7)
  - USART1 (0x40013800): TX/RX FIFOs to VirtualUART1Bridge (PA9/PA10) + RXNE IRQ
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Callable, List, Optional

from sim.core.pin_matrix import GpioMode, VirtualPinMatrix
from sim.mcu.cortex_m3_nvic import (
    CortexM3Nvic,
    IRQ_ADC1_2,
    IRQ_DMA1_CHANNEL1,
    IRQ_EXTI15_10,
    IRQ_TIM2,
    IRQ_TIM3,
    IRQ_USART1,
)

PERIPH_BASE = 0x40000000
PERIPH_SIZE = 0x00030000  # 192 KB covers 0x40000000 .. 0x4002FFFF


@dataclass
class _TimerState:
    irq_num: int
    cr1: int = 0
    cr2: int = 0
    dier: int = 0
    sr: int = 0
    cnt: int = 0
    psc: int = 0
    arr: int = 0xFFFF
    _acc_ticks: float = 0.0

    def reset(self) -> None:
        self.cr1 = 0
        self.cr2 = 0
        self.dier = 0
        self.sr = 0
        self.cnt = 0
        self.psc = 0
        self.arr = 0xFFFF
        self._acc_ticks = 0.0

    def tick(self, dt_us: float, timer_clk_hz: float) -> int:
        """Advance timer by `dt_us`; return number of update events (overflows)."""
        if not (self.cr1 & 0x01) or timer_clk_hz <= 0.0:
            return 0
        tick_hz = timer_clk_hz / (self.psc + 1)
        self._acc_ticks += tick_hz * (dt_us * 1e-6)
        whole = int(self._acc_ticks)
        if whole <= 0:
            return 0
        self._acc_ticks -= whole

        period = max(1, self.arr + 1)
        total = self.cnt + whole
        updates = total // period
        self.cnt = total % period
        if updates > 0:
            self.sr |= 0x01  # UIF
        return updates


@dataclass
class _DmaChannel:
    ccr: int = 0
    cndtr: int = 0
    cpar: int = 0
    cmar: int = 0
    initial_cndtr: int = 0
    cur_idx: int = 0

    def reset(self) -> None:
        self.ccr = 0
        self.cndtr = 0
        self.cpar = 0
        self.cmar = 0
        self.initial_cndtr = 0
        self.cur_idx = 0


class STM32F103MMIO:
    """Complete STM32F103C8T6 MMIO peripheral controller for P05."""

    def __init__(self, nvic: CortexM3Nvic) -> None:
        self.nvic = nvic
        self.matrix: Optional[VirtualPinMatrix] = None
        self._sram_write_fn: Optional[Callable[[int, bytes], None]] = None
        self.reset()

    def attach(
        self,
        matrix: VirtualPinMatrix,
        sram_write_fn: Callable[[int, bytes], None],
    ) -> None:
        self.matrix = matrix
        self._sram_write_fn = sram_write_fn
        matrix.register_exti12_callback(self._on_wake_button_edge)
        self._sync_gpio_to_matrix()

    def reset(self) -> None:
        # RCC (POR defaults from RM0008)
        self.rcc_cr = 0x00000083      # HSION=1, HSIRDY=1
        self.rcc_cfgr = 0x00000000    # SW=HSI (8 MHz)
        self.rcc_ahbenr = 0x00000014
        self.rcc_apb2enr = 0x00000000
        self.rcc_apb1enr = 0x00000000
        self.rcc_bdcr = 0x00000000
        self.rcc_csr = 0x0C000000
        self.sysclk_hz = 8_000_000.0

        # FLASH & PWR
        self.flash_acr = 0x00000030
        self.pwr_cr = 0x00000000
        self.pwr_csr = 0x00000000

        # AFIO & EXTI
        self.afio_mapr = 0x00000000
        self.afio_exticr = [0, 0, 0, 0]
        self.exti_imr = 0x00000000
        self.exti_emr = 0x00000000
        self.exti_rtsr = 0x00000000
        self.exti_ftsr = 0x00000000
        self.exti_pr = 0x00000000

        # GPIOA & GPIOB (POR reset: all floating input 0x44444444)
        self.gpioa_crl = 0x44444444
        self.gpioa_crh = 0x44444444
        self.gpioa_odr = 0x00000010  # PA4 SD_CS defaults HIGH for safety
        self.gpiob_crl = 0x44444444
        self.gpiob_crh = 0x44444444
        self.gpiob_odr = 0x00000001  # PB0 SD_PWR_EN defaults HIGH (AO3401 OFF)

        # TIM2 & TIM3
        self.tim2 = _TimerState(IRQ_TIM2)
        self.tim3 = _TimerState(IRQ_TIM3)

        # ADC1
        self.adc1_sr = 0x00000000
        self.adc1_cr1 = 0x00000000
        self.adc1_cr2 = 0x00000000
        self.adc1_smpr1 = 0x00000000
        self.adc1_smpr2 = 0x00000000
        self.adc1_sqr1 = 0x00000000
        self.adc1_sqr2 = 0x00000000
        self.adc1_sqr3 = 0x00000000  # Bits [4:0] = SQ1 channel (0 = PA0, 9 = PB1)
        self.adc1_dr = 0x00000000

        # DMA1 (7 channels, Ch1 = ADC1)
        self.dma1_isr = 0x00000000
        self.dma1_ch = [_DmaChannel() for _ in range(7)]

        # SPI1
        self.spi1_cr1 = 0x00000000
        self.spi1_cr2 = 0x00000000
        self.spi1_sr = 0x00000002   # TXE = 1
        self.spi1_rx_dr = 0x00FF

        # I2C1 master transaction state machine
        self.i2c1_cr1 = 0x00000000
        self.i2c1_cr2 = 0x00000000
        self.i2c1_sr1 = 0x00000000
        self.i2c1_sr2 = 0x00000000
        self.i2c1_ccr = 0x00000000
        self.i2c1_trise = 0x00000002
        self._i2c_state = "IDLE"     # IDLE | START | ADDR_TX | ADDR_RX
        self._i2c_addr = 0
        self._i2c_tx_buf = bytearray()
        self._i2c_rx_buf = bytearray()
        self._i2c_rx_idx = 0

        # USART1
        self.usart1_sr = 0x000000C0  # TXE=1, TC=1
        self.usart1_brr = 0x00000000
        self.usart1_cr1 = 0x00000000
        self.usart1_cr2 = 0x00000000
        self.usart1_cr3 = 0x00000000

    # --- Wake Button EXTI12 Callback ---
    def _on_wake_button_edge(self, pressed: bool) -> None:
        """Called when user presses (falling edge on PB12) or releases SW_WAKE."""
        # Check AFIO EXTICR4[3:0] selects Port B (0x1) or allow EXTI12 directly
        falling = pressed
        rising = not pressed
        trig = (falling and bool(self.exti_ftsr & (1 << 12))) or (rising and bool(self.exti_rtsr & (1 << 12)))
        if trig:
            self.exti_pr |= 1 << 12
            if self.exti_imr & (1 << 12):
                self.nvic.set_irq_pending(IRQ_EXTI15_10)

    # --- Periodic Peripheral Update (called every simulation quantum dt_us) ---
    def tick(self, dt_us: float, cpu_awake: bool) -> None:
        """Advance TIM2/3, hardware-triggered ADC1+DMA1, and USART1 RX interrupts."""
        if self.matrix is None:
            return

        # Keep GPIO matrix levels synced
        self._sync_gpio_to_matrix()

        # USART1 RXNE check
        if (self.usart1_cr1 & 0x2004) == 0x2004 and len(self.matrix.uart1_rx_fifo) > 0:
            self.usart1_sr |= 0x0020  # RXNE
            if self.usart1_cr1 & 0x0020:  # RXNEIE
                self.nvic.set_irq_pending(IRQ_USART1)

        if not cpu_awake:
            # In Stop Mode APB clocks are frozen; only EXTI12 can wake
            return

        # Advance TIM2 & TIM3
        for tim in (self.tim2, self.tim3):
            updates = tim.tick(dt_us, self.sysclk_hz)
            if updates <= 0:
                continue
            if tim.dier & 0x01:  # UIE
                self.nvic.set_irq_pending(tim.irq_num)
            # Check TRGO Update mode (CR2.MMS bits [6:4] == 0b010) -> triggers ADC1!
            if ((tim.cr2 >> 4) & 0x07) == 0b010:
                if (self.adc1_cr2 & 0x00100001) == 0x00100001:  # ADON + EXTTRIG
                    for _ in range(updates):
                        self._trigger_adc1_conversion()

    # --- Clock & MCO Computation ---
    def _update_clocks(self) -> None:
        sw = self.rcc_cfgr & 0x03
        hse_hz = 8_000_000.0
        hsi_hz = 8_000_000.0
        if sw == 0:
            self.sysclk_hz = hsi_hz
        elif sw == 1:
            self.sysclk_hz = hse_hz
        elif sw == 2:
            pllsrc = (self.rcc_cfgr >> 16) & 0x01
            pllxtpre = (self.rcc_cfgr >> 17) & 0x01
            pllmul_bits = (self.rcc_cfgr >> 18) & 0x0F
            pllmul = min(16, pllmul_bits + 2)
            base_in = (hse_hz / (2.0 if pllxtpre else 1.0)) if pllsrc else (hsi_hz / 2.0)
            self.sysclk_hz = base_in * pllmul
        else:
            self.sysclk_hz = hsi_hz

        # Mirror SW into SWS bits [3:2]
        self.rcc_cfgr = (self.rcc_cfgr & ~0x0C) | (sw << 2)

        # Update PA8 MCO_OUT frequency on VirtualPinMatrix!
        if self.matrix is not None:
            mco = (self.rcc_cfgr >> 24) & 0x07
            if mco == 0b100:
                self.matrix.mco_hz = self.sysclk_hz
            elif mco == 0b101:
                self.matrix.mco_hz = hsi_hz
            elif mco == 0b110:
                self.matrix.mco_hz = hse_hz
            elif mco == 0b111:
                self.matrix.mco_hz = self.sysclk_hz / 2.0
            else:
                self.matrix.mco_hz = 0.0

    # --- GPIO <-> VirtualPinMatrix Bidirectional Sync ---
    @staticmethod
    def _decode_pin_mode(cr_reg: int, local_pin: int) -> GpioMode:
        cfg = (cr_reg >> (local_pin * 4)) & 0x0F
        mode = cfg & 0x03
        cnf = (cfg >> 2) & 0x03
        if mode == 0:
            if cnf == 0:
                return GpioMode.ANALOG
            if cnf == 1:
                return GpioMode.INPUT_FLOATING
            return GpioMode.INPUT_PULL
        if cnf == 0:
            return GpioMode.OUTPUT_PP
        if cnf == 1:
            return GpioMode.OUTPUT_OD
        if cnf == 2:
            return GpioMode.AF_PP
        return GpioMode.AF_OD

    def _sync_gpio_to_matrix(self) -> None:
        if self.matrix is None:
            return
        # Sync MCU digital outputs -> VirtualPinMatrix
        self.matrix.digital["ECG_SDN"] = bool(self.gpioa_odr & (1 << 3))
        self.matrix.digital["SD_CS"] = bool(self.gpioa_odr & (1 << 4))
        self.matrix.digital["SD_PWR_EN"] = bool(self.gpiob_odr & (1 << 0))
        self.matrix.digital["BAT_SENSE_EN"] = bool(self.gpiob_odr & (1 << 5))

        # Sync electrical pin modes (CRL/CRH) -> VirtualPinMatrix (enables realistic Stop Mode leakage check!)
        for pin in range(11):
            reg = self.gpioa_crl if pin < 8 else self.gpioa_crh
            self.matrix.gpio_modes[f"PA{pin}"] = self._decode_pin_mode(reg, pin % 8)
        for pin in (0, 1, 5, 6, 7, 12):
            reg = self.gpiob_crl if pin < 8 else self.gpiob_crh
            self.matrix.gpio_modes[f"PB{pin}"] = self._decode_pin_mode(reg, pin % 8)

    def _read_gpioa_idr(self) -> int:
        val = self.gpioa_odr & 0xFFFF
        if self.matrix is not None:
            if self.matrix.digital["ECG_LOD_P"]:
                val |= 1 << 1
            else:
                val &= ~(1 << 1)
            if self.matrix.digital["ECG_LOD_N"]:
                val |= 1 << 2
            else:
                val &= ~(1 << 2)
        return val

    def _read_gpiob_idr(self) -> int:
        val = self.gpiob_odr & 0xFFFF
        if self.matrix is not None:
            if self.matrix.digital["WAKE_BTN"]:
                val |= 1 << 12
            else:
                val &= ~(1 << 12)
        return val

    # --- ADC1 + DMA1 Channel 1 Circular Ping-Pong Transfer ---
    def _trigger_adc1_conversion(self) -> None:
        if self.matrix is None:
            return
        ch = self.adc1_sqr3 & 0x1F  # SQ1 channel (0 = PA0 ECG_OUT, 9 = PB1 BAT_SENSE)
        sample12 = self.matrix.read_adc_counts(ch)
        self.adc1_dr = sample12 & 0x0FFF
        self.adc1_sr |= 0x02  # EOC = 1

        if self.adc1_cr1 & (1 << 5):  # EOCIE
            self.nvic.set_irq_pending(IRQ_ADC1_2)

        # If ADC1 CR2.DMA (bit 8) and DMA1_Ch1 CCR.EN (bit 0) are enabled -> transfer to SRAM!
        ch1 = self.dma1_ch[0]
        if (self.adc1_cr2 & (1 << 8)) and (ch1.ccr & 0x01) and ch1.cndtr > 0 and self._sram_write_fn is not None:
            msize = (ch1.ccr >> 10) & 0x03  # 0=8b, 1=16b, 2=32b
            elem_bytes = 1 << msize
            minc = bool(ch1.ccr & (1 << 7))
            dst_addr = ch1.cmar + (ch1.cur_idx * elem_bytes if minc else 0)
            if elem_bytes == 1:
                raw = bytes([self.adc1_dr & 0xFF])
            elif elem_bytes == 2:
                raw = struct.pack("<H", self.adc1_dr & 0xFFFF)
            else:
                raw = struct.pack("<I", self.adc1_dr & 0xFFFFFFFF)
            self._sram_write_fn(dst_addr, raw)
            self.adc1_sr &= ~0x02  # DMA read clears EOC

            ch1.cur_idx += 1
            ch1.cndtr -= 1

            # Half-Transfer Interrupt (HTIF1 = bit 2, GIF1 = bit 0)
            half_point = max(1, ch1.initial_cndtr // 2)
            if ch1.cur_idx == half_point:
                self.dma1_isr |= 0x05  # GIF1 | HTIF1
                if ch1.ccr & (1 << 2):  # HTIE
                    self.nvic.set_irq_pending(IRQ_DMA1_CHANNEL1)

            # Transfer-Complete Interrupt (TCIF1 = bit 1, GIF1 = bit 0)
            if ch1.cndtr == 0:
                self.dma1_isr |= 0x03  # GIF1 | TCIF1
                if ch1.ccr & (1 << 1):  # TCIE
                    self.nvic.set_irq_pending(IRQ_DMA1_CHANNEL1)
                if ch1.ccr & (1 << 5):  # CIRC mode -> wrap around!
                    ch1.cndtr = ch1.initial_cndtr
                    ch1.cur_idx = 0

    # --- Top-Level Peripheral MMIO Read/Write (0x40000000 .. 0x4002FFFF) ---
    def mmio_read(self, offset: int, size: int) -> int:
        addr = PERIPH_BASE + offset
        base = addr & 0xFFFFFC00
        reg = addr & 0x000003FF

        # TIM2 / TIM3
        if base == 0x40000000:
            return self._read_tim(self.tim2, reg)
        if base == 0x40000400:
            return self._read_tim(self.tim3, reg)

        # I2C1
        if base == 0x40005400:
            return self._read_i2c1(reg)

        # PWR
        if base == 0x40007000:
            if reg == 0x00:
                return self.pwr_cr
            if reg == 0x04:
                return self.pwr_csr
            return 0

        # AFIO / EXTI
        if base == 0x40010000:
            if reg == 0x04:
                return self.afio_mapr
            if 0x08 <= reg <= 0x14:
                return self.afio_exticr[(reg - 0x08) >> 2]
            return 0
        if base == 0x40010400:
            if reg == 0x00:
                return self.exti_imr
            if reg == 0x04:
                return self.exti_emr
            if reg == 0x08:
                return self.exti_rtsr
            if reg == 0x0C:
                return self.exti_ftsr
            if reg == 0x14:
                return self.exti_pr
            return 0

        # GPIOA / GPIOB
        if base == 0x40010800:
            if reg == 0x00:
                return self.gpioa_crl
            if reg == 0x04:
                return self.gpioa_crh
            if reg == 0x08:
                return self._read_gpioa_idr()
            if reg == 0x0C:
                return self.gpioa_odr
            return 0
        if base == 0x40010C00:
            if reg == 0x00:
                return self.gpiob_crl
            if reg == 0x04:
                return self.gpiob_crh
            if reg == 0x08:
                return self._read_gpiob_idr()
            if reg == 0x0C:
                return self.gpiob_odr
            return 0

        # ADC1
        if base == 0x40012400:
            if reg == 0x00:
                return self.adc1_sr
            if reg == 0x04:
                return self.adc1_cr1
            if reg == 0x08:
                return self.adc1_cr2
            if reg == 0x2C:
                return self.adc1_sqr1
            if reg == 0x30:
                return self.adc1_sqr2
            if reg == 0x34:
                return self.adc1_sqr3
            if reg == 0x4C:
                self.adc1_sr &= ~0x02  # Reading DR clears EOC
                return self.adc1_dr
            return 0

        # SPI1
        if base == 0x40013000:
            if reg == 0x00:
                return self.spi1_cr1
            if reg == 0x04:
                return self.spi1_cr2
            if reg == 0x08:
                return self.spi1_sr
            if reg == 0x0C:
                self.spi1_sr &= ~0x01  # Clear RXNE
                return self.spi1_rx_dr
            return 0

        # USART1
        if base == 0x40013800:
            if reg == 0x00:
                if self.matrix is not None and len(self.matrix.uart1_rx_fifo) > 0:
                    self.usart1_sr |= 0x0020  # RXNE
                else:
                    self.usart1_sr &= ~0x0020
                return self.usart1_sr
            if reg == 0x04:
                if self.matrix is not None and len(self.matrix.uart1_rx_fifo) > 0:
                    b = self.matrix.uart1_rx_fifo.popleft()
                    if not self.matrix.uart1_rx_fifo:
                        self.usart1_sr &= ~0x0020
                    return b
                return 0
            if reg == 0x08:
                return self.usart1_brr
            if reg == 0x0C:
                return self.usart1_cr1
            if reg == 0x10:
                return self.usart1_cr2
            if reg == 0x14:
                return self.usart1_cr3
            return 0

        # DMA1
        if base == 0x40020000:
            if reg == 0x00:
                return self.dma1_isr
            if reg >= 0x08:
                ch_idx = (reg - 0x08) // 0x14
                ch_off = (reg - 0x08) % 0x14
                if 0 <= ch_idx < 7:
                    ch = self.dma1_ch[ch_idx]
                    if ch_off == 0x00:
                        return ch.ccr
                    if ch_off == 0x04:
                        return ch.cndtr
                    if ch_off == 0x08:
                        return ch.cpar
                    if ch_off == 0x0C:
                        return ch.cmar
            return 0

        # RCC
        if base == 0x40021000:
            if reg == 0x00:
                return self.rcc_cr
            if reg == 0x04:
                return self.rcc_cfgr
            if reg == 0x14:
                return self.rcc_ahbenr
            if reg == 0x18:
                return self.rcc_apb2enr
            if reg == 0x1C:
                return self.rcc_apb1enr
            if reg == 0x20:
                return self.rcc_bdcr
            if reg == 0x24:
                return self.rcc_csr
            return 0

        # FLASH
        if base == 0x40022000:
            return self.flash_acr if reg == 0x00 else 0

        return 0

    def mmio_write(self, offset: int, size: int, value: int) -> None:
        addr = PERIPH_BASE + offset
        base = addr & 0xFFFFFC00
        reg = addr & 0x000003FF
        v = value & 0xFFFFFFFF

        # TIM2 / TIM3
        if base == 0x40000000:
            self._write_tim(self.tim2, reg, v)
            return
        if base == 0x40000400:
            self._write_tim(self.tim3, reg, v)
            return

        # I2C1
        if base == 0x40005400:
            self._write_i2c1(reg, v)
            return

        # PWR
        if base == 0x40007000:
            if reg == 0x00:
                self.pwr_cr = v
            elif reg == 0x04:
                self.pwr_csr = v
            return

        # AFIO / EXTI
        if base == 0x40010000:
            if reg == 0x04:
                self.afio_mapr = v
            elif 0x08 <= reg <= 0x14:
                self.afio_exticr[(reg - 0x08) >> 2] = v
            return
        if base == 0x40010400:
            if reg == 0x00:
                self.exti_imr = v
            elif reg == 0x04:
                self.exti_emr = v
            elif reg == 0x08:
                self.exti_rtsr = v
            elif reg == 0x0C:
                self.exti_ftsr = v
            elif reg == 0x14:
                self.exti_pr &= ~v  # Write-1-to-clear!
            return

        # GPIOA
        if base == 0x40010800:
            if reg == 0x00:
                self.gpioa_crl = v
            elif reg == 0x04:
                self.gpioa_crh = v
            elif reg == 0x0C:
                self.gpioa_odr = v & 0xFFFF
            elif reg == 0x10:  # BSRR: lower 16 set, upper 16 reset
                self.gpioa_odr = (self.gpioa_odr | (v & 0xFFFF)) & ~((v >> 16) & 0xFFFF)
            elif reg == 0x14:  # BRR
                self.gpioa_odr &= ~(v & 0xFFFF)
            self._sync_gpio_to_matrix()
            return

        # GPIOB
        if base == 0x40010C00:
            if reg == 0x00:
                self.gpiob_crl = v
            elif reg == 0x04:
                self.gpiob_crh = v
            elif reg == 0x0C:
                self.gpiob_odr = v & 0xFFFF
            elif reg == 0x10:  # BSRR
                self.gpiob_odr = (self.gpiob_odr | (v & 0xFFFF)) & ~((v >> 16) & 0xFFFF)
            elif reg == 0x14:  # BRR
                self.gpiob_odr &= ~(v & 0xFFFF)
            self._sync_gpio_to_matrix()
            return

        # ADC1
        if base == 0x40012400:
            if reg == 0x00:
                self.adc1_sr &= v
            elif reg == 0x04:
                self.adc1_cr1 = v
            elif reg == 0x08:
                # Auto-clear CAL (bit 2) & RSTCAL (bit 3) immediately so calibration loops don't hang!
                swstart = bool(v & (1 << 22))
                self.adc1_cr2 = v & ~((1 << 2) | (1 << 3) | (1 << 22))
                if swstart and (self.adc1_cr2 & 0x01):
                    self._trigger_adc1_conversion()
            elif reg == 0x2C:
                self.adc1_sqr1 = v
            elif reg == 0x30:
                self.adc1_sqr2 = v
            elif reg == 0x34:
                self.adc1_sqr3 = v
            return

        # SPI1
        if base == 0x40013000:
            if reg == 0x00:
                self.spi1_cr1 = v
            elif reg == 0x04:
                self.spi1_cr2 = v
            elif reg == 0x0C:
                tx = v & 0xFF
                rx = self.matrix.spi1_transfer_byte(tx) if self.matrix is not None else 0xFF
                self.spi1_rx_dr = rx
                self.spi1_sr = 0x0003  # TXE=1 | RXNE=1
            return

        # USART1
        if base == 0x40013800:
            if reg == 0x00:
                self.usart1_sr &= v | 0x0080  # Keep TXE=1
            elif reg == 0x04:
                if self.matrix is not None:
                    self.matrix.uart1_tx_fifo.append(v & 0xFF)
                self.usart1_sr |= 0x00C0  # TXE=1, TC=1
            elif reg == 0x08:
                self.usart1_brr = v
            elif reg == 0x0C:
                self.usart1_cr1 = v
            elif reg == 0x10:
                self.usart1_cr2 = v
            elif reg == 0x14:
                self.usart1_cr3 = v
            return

        # DMA1
        if base == 0x40020000:
            if reg == 0x04:  # IFCR: write-1-to-clear ISR flags
                self.dma1_isr &= ~v
            elif reg >= 0x08:
                ch_idx = (reg - 0x08) // 0x14
                ch_off = (reg - 0x08) % 0x14
                if 0 <= ch_idx < 7:
                    ch = self.dma1_ch[ch_idx]
                    if ch_off == 0x00:
                        was_en = bool(ch.ccr & 0x01)
                        ch.ccr = v
                        if (v & 0x01) and not was_en:
                            ch.initial_cndtr = ch.cndtr
                            ch.cur_idx = 0
                    elif ch_off == 0x04:
                        ch.cndtr = v & 0xFFFF
                        ch.initial_cndtr = ch.cndtr
                        ch.cur_idx = 0
                    elif ch_off == 0x08:
                        ch.cpar = v
                    elif ch_off == 0x0C:
                        ch.cmar = v
            return

        # RCC
        if base == 0x40021000:
            if reg == 0x00:
                self.rcc_cr = v
                if v & (1 << 0):
                    self.rcc_cr |= 1 << 1   # HSIRDY
                if v & (1 << 16):
                    self.rcc_cr |= 1 << 17  # HSERDY
                if v & (1 << 24):
                    self.rcc_cr |= 1 << 25  # PLLRDY
                self._update_clocks()
            elif reg == 0x04:
                self.rcc_cfgr = v
                self._update_clocks()
            elif reg == 0x14:
                self.rcc_ahbenr = v
            elif reg == 0x18:
                self.rcc_apb2enr = v
            elif reg == 0x1C:
                self.rcc_apb1enr = v
            elif reg == 0x20:
                self.rcc_bdcr = v
                if v & 0x01:
                    self.rcc_bdcr |= 0x02  # LSERDY
            elif reg == 0x24:
                self.rcc_csr = v
                if v & 0x01:
                    self.rcc_csr |= 0x02  # LSIRDY
            return

        # FLASH
        if base == 0x40022000 and reg == 0x00:
            self.flash_acr = v
            return

    # --- Helpers: TIM & I2C1 ---
    @staticmethod
    def _read_tim(tim: _TimerState, reg: int) -> int:
        if reg == 0x00:
            return tim.cr1
        if reg == 0x04:
            return tim.cr2
        if reg == 0x0C:
            return tim.dier
        if reg == 0x10:
            return tim.sr
        if reg == 0x24:
            return tim.cnt
        if reg == 0x28:
            return tim.psc
        if reg == 0x2C:
            return tim.arr
        return 0

    @staticmethod
    def _write_tim(tim: _TimerState, reg: int, v: int) -> None:
        if reg == 0x00:
            tim.cr1 = v
        elif reg == 0x04:
            tim.cr2 = v
        elif reg == 0x0C:
            tim.dier = v
        elif reg == 0x10:
            tim.sr &= v  # Clear UIF by writing 0
        elif reg == 0x14 and (v & 0x01):  # EGR.UG
            tim.cnt = 0
            tim._acc_ticks = 0.0
        elif reg == 0x24:
            tim.cnt = v & 0xFFFF
        elif reg == 0x28:
            tim.psc = v & 0xFFFF
        elif reg == 0x2C:
            tim.arr = v & 0xFFFF

    def _read_i2c1(self, reg: int) -> int:
        if reg == 0x00:
            return self.i2c1_cr1
        if reg == 0x04:
            return self.i2c1_cr2
        if reg == 0x10:  # DR (Master Receive byte)
            if self._i2c_state == "ADDR_RX" and self._i2c_rx_idx < len(self._i2c_rx_buf):
                b = self._i2c_rx_buf[self._i2c_rx_idx]
                self._i2c_rx_idx += 1
                if self._i2c_rx_idx >= len(self._i2c_rx_buf):
                    self.i2c1_sr1 &= ~0x0040  # Clear RXNE
                return b
            return 0
        if reg == 0x14:  # SR1
            return self.i2c1_sr1
        if reg == 0x18:  # SR2 (reading SR2 after SR1 clears ADDR!)
            if self.i2c1_sr1 & 0x0002:
                self.i2c1_sr1 &= ~0x0002
            return self.i2c1_sr2
        if reg == 0x1C:
            return self.i2c1_ccr
        if reg == 0x20:
            return self.i2c1_trise
        return 0

    def _write_i2c1(self, reg: int, v: int) -> None:
        if reg == 0x00:  # CR1
            if v & 0x8000:  # SWRST
                self._i2c_state = "IDLE"
                self.i2c1_sr1 = 0
                self.i2c1_sr2 = 0
                return
            self.i2c1_cr1 = v & ~0x0300  # START & STOP self-clear
            if v & 0x0100:  # START (or Repeated START)
                if self._i2c_state == "ADDR_TX" and self._i2c_tx_buf and self.matrix is not None:
                    self.matrix.i2c1_write(self._i2c_addr, bytes(self._i2c_tx_buf))
                    self._i2c_tx_buf.clear()
                self._i2c_state = "START"
                self.i2c1_sr1 = 0x0001  # SB = 1
                self.i2c1_sr2 = 0x0003  # MSL | BUSY
            if v & 0x0200:  # STOP
                if self._i2c_state == "ADDR_TX" and self._i2c_tx_buf and self.matrix is not None:
                    self.matrix.i2c1_write(self._i2c_addr, bytes(self._i2c_tx_buf))
                    self._i2c_tx_buf.clear()
                self._i2c_state = "IDLE"
                self.i2c1_sr1 &= ~0x00C7
                self.i2c1_sr2 = 0x0000
        elif reg == 0x04:
            self.i2c1_cr2 = v
        elif reg == 0x10:  # DR
            byte = v & 0xFF
            if self._i2c_state == "START":
                addr7 = byte >> 1
                is_read = bool(byte & 0x01)
                self._i2c_addr = addr7
                self.i2c1_sr1 &= ~0x0001  # Clear SB
                if is_read:
                    ok, data = self.matrix.i2c1_read(addr7, 4) if self.matrix is not None else (False, b"")
                    if ok:
                        self._i2c_state = "ADDR_RX"
                        self._i2c_rx_buf = bytearray(data)
                        self._i2c_rx_idx = 0
                        self.i2c1_sr1 = 0x0042  # ADDR | RXNE
                        self.i2c1_sr2 = 0x0003  # MSL | BUSY (TRA=0)
                    else:
                        self.i2c1_sr1 = 0x0400  # AF (NACK)
                else:
                    # Probe slave with write check (empty probe deferred until STOP/ReSTART; verify addr)
                    self._i2c_state = "ADDR_TX"
                    self._i2c_tx_buf = bytearray()
                    self.i2c1_sr1 = 0x0082  # ADDR | TXE
                    self.i2c1_sr2 = 0x0007  # MSL | BUSY | TRA
            elif self._i2c_state == "ADDR_TX":
                self._i2c_tx_buf.append(byte)
                self.i2c1_sr1 |= 0x0084  # TXE | BTF
        elif reg == 0x14:  # SR1 (clear AF by writing 0)
            self.i2c1_sr1 &= v
        elif reg == 0x1C:
            self.i2c1_ccr = v
        elif reg == 0x20:
            self.i2c1_trise = v
