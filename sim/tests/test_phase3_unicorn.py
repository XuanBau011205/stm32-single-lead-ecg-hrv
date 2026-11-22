"""
Phase 3 Integration Tests — Real ARM Cortex-M3 Firmware Flashed onto P05VirtualBoard via UnicornBackend.

Builds genuine ARM Cortex-M3 Thumb-1/Thumb-2 machine code binaries (with Vector Table,
Reset_Handler, DMA1_Channel1_IRQHandler, EXTI15_10_IRQHandler, EXC_RETURN, WFI) and flashes
them into P05VirtualBoard to verify ALL 10 STM32F103 peripherals end-to-end!
"""

from __future__ import annotations

import struct
import tempfile
import unittest
from pathlib import Path
from typing import Dict, List, Tuple

from sim import P05VirtualBoard
from sim.mcu import McuPowerMode, UnicornBackend


class ThumbAsm:
    """Tiny pure-Python ARMv7-M (Cortex-M3) Thumb assembler for self-contained firmware images."""

    def __init__(self, base_addr: int = 0x08000100) -> None:
        self.base = base_addr
        self.code = bytearray()
        self.labels: Dict[str, int] = {}
        self._lit_fixups: List[Tuple[int, int, int]] = []    # (offset, rt, value32)
        self._branch_fixups: List[Tuple[int, str, str]] = []  # (offset, kind, label)

    def here(self) -> int:
        return self.base + len(self.code)

    def label(self, name: str) -> None:
        self.labels[name] = self.here()

    def _emit16(self, hw: int) -> None:
        self.code.extend(struct.pack("<H", hw & 0xFFFF))

    def ldr_lit(self, rt: int, val32: int) -> None:
        self._lit_fixups.append((len(self.code), rt & 7, val32 & 0xFFFFFFFF))
        self._emit16(0x4800 | ((rt & 7) << 8))

    def str_imm(self, rt: int, rn: int, offset_bytes: int = 0) -> None:
        assert offset_bytes % 4 == 0 and 0 <= offset_bytes <= 124
        self._emit16(0x6000 | ((offset_bytes >> 2) << 6) | ((rn & 7) << 3) | (rt & 7))

    def ldr_imm(self, rt: int, rn: int, offset_bytes: int = 0) -> None:
        assert offset_bytes % 4 == 0 and 0 <= offset_bytes <= 124
        self._emit16(0x6800 | ((offset_bytes >> 2) << 6) | ((rn & 7) << 3) | (rt & 7))

    def movs_imm(self, rd: int, imm8: int) -> None:
        self._emit16(0x2000 | ((rd & 7) << 8) | (imm8 & 0xFF))

    def adds_imm(self, rd: int, imm8: int) -> None:
        self._emit16(0x3000 | ((rd & 7) << 8) | (imm8 & 0xFF))

    def subs_imm(self, rd: int, imm8: int) -> None:
        self._emit16(0x3800 | ((rd & 7) << 8) | (imm8 & 0xFF))

    def orrs(self, rd: int, rm: int) -> None:
        self._emit16(0x4300 | ((rm & 7) << 3) | (rd & 7))

    def tst(self, rn: int, rm: int) -> None:
        self._emit16(0x4200 | ((rm & 7) << 3) | (rn & 7))

    def cmp_imm(self, rn: int, imm8: int) -> None:
        self._emit16(0x2800 | ((rn & 7) << 8) | (imm8 & 0xFF))

    def beq(self, label: str) -> None:
        self._branch_fixups.append((len(self.code), "BEQ", label))
        self._emit16(0xD000)

    def bne(self, label: str) -> None:
        self._branch_fixups.append((len(self.code), "BNE", label))
        self._emit16(0xD100)

    def b(self, label: str) -> None:
        self._branch_fixups.append((len(self.code), "B", label))
        self._emit16(0xE000)

    def nop(self) -> None:
        self._emit16(0xBF00)

    def wfi(self) -> None:
        self._emit16(0xBF30)

    def bx_lr(self) -> None:
        self._emit16(0x4770)

    def write32(self, addr: int, val: int, r_addr: int = 0, r_val: int = 1) -> None:
        """Macro: *(volatile uint32_t*)addr = val."""
        self.ldr_lit(r_addr, addr)
        self.ldr_lit(r_val, val)
        self.str_imm(r_val, r_addr, 0)

    def finalize_image(self, vector_map: Dict[int, str], init_sp: int = 0x20005000) -> bytes:
        """Emit complete STM32F103 Flash image starting at 0x08000000 with Vector Table + Literal Pool."""
        # Align literal pool to 4 bytes
        if len(self.code) % 4 != 0:
            self.nop()

        lit_pool_map: Dict[int, int] = {}
        for off, rt, val in self._lit_fixups:
            if val not in lit_pool_map:
                lit_pool_map[val] = self.here()
                self.code.extend(struct.pack("<I", val))
            lit_addr = lit_pool_map[val]
            pc_aligned = ((self.base + off) + 4) & ~3
            delta = lit_addr - pc_aligned
            assert delta >= 0 and delta % 4 == 0 and (delta >> 2) <= 255, f"Literal out of range: {delta}"
            hw = 0x4800 | (rt << 8) | (delta >> 2)
            self.code[off : off + 2] = struct.pack("<H", hw)

        for off, kind, target_lbl in self._branch_fixups:
            target = self.labels[target_lbl]
            delta = target - (self.base + off + 4)
            assert delta % 2 == 0
            if kind == "BEQ":
                hw = 0xD000 | ((delta >> 1) & 0xFF)
            elif kind == "BNE":
                hw = 0xD100 | ((delta >> 1) & 0xFF)
            else:
                hw = 0xE000 | ((delta >> 1) & 0x7FF)
            self.code[off : off + 2] = struct.pack("<H", hw)

        # Build 256-byte Vector Table (64 vectors * 4 bytes = 0x100)
        assert self.base == 0x08000100
        vtab = bytearray(0x100)
        vtab[0:4] = struct.pack("<I", init_sp)
        for vec_idx, lbl in vector_map.items():
            addr = self.labels[lbl] | 1  # Thumb bit
            vtab[vec_idx * 4 : vec_idx * 4 + 4] = struct.pack("<I", addr)
        return bytes(vtab) + bytes(self.code)


def build_p05_bringup_firmware() -> bytes:
    """
    Assemble a genuine STM32F103C8T6 binary that tests EVERY P05 subsystem:
      1. RCC: HSE 8 MHz -> PLL 72 MHz + PA8 MCO = HSE (8 MHz)
      2. DWT->CYCCNT measurement stored to SRAM 0x20000000
      3. GPIOA/B: wake AD8232 (PA3=1), enable AO3401 microSD (PB0=0), enable 2N7002 (PB5=1)
      4. SPI1: send CMD0 to VirtualMicroSD -> store R1 (0x01) at SRAM 0x20000004
      5. I2C1: read INA219 Bus Voltage (Reg 0x02) -> store 16-bit raw at SRAM 0x20000008
      6. USART1: transmit "P05!" out PA9 DBG_TX
      7. TIM2 (250 Hz TRGO) + ADC1 (PA0 ECG_OUT) + DMA1_Ch1 (8 samples circular -> SRAM 0x20000100)
         with DMA1_Channel1_IRQHandler incrementing SRAM 0x2000000C (HTIF/TCIF)
      8. After 2 DMA IRQs (HT+TC): shut down AD8232, AO3401, 2N7002, INA219 PD, set all GPIOs Analog,
         configure EXTI12 falling-edge (SW_WAKE), set SCB->SCR.SLEEPDEEP=1, execute WFI -> 32.1 uA STOP!
      9. On SW_WAKE press: EXTI15_10_IRQHandler writes 0xCAFE to SRAM 0x20000010 and wakes CPU!
    """
    a = ThumbAsm(0x08000100)

    # --- Reset_Handler ---
    a.label("Reset_Handler")

    # 1. RCC: HSEON (bit 16) + PLLON (bit 24); CFGR = MCO=HSE(0b110<<24) | PLLMUL=9x(7<<18) | PLLSRC=HSE(1<<16) | SW=PLL(2)
    a.write32(0x40021004, (0b110 << 24) | (7 << 18) | (1 << 16) | 2)
    a.write32(0x40021000, (1 << 24) | (1 << 16) | 1)

    # 2. Enable DWT->CYCCNT (CoreDebug DEMCR.TRCENA=bit 24, DWT->CYCCNT=0, DWT->CTRL.CYCCNTENA=bit 0)
    a.write32(0xE000EDFC, 1 << 24)
    a.write32(0xE0001004, 0)
    a.write32(0xE0001000, 1)
    # Run 10-iteration loop and store DWT->CYCCNT to SRAM 0x20000000
    a.movs_imm(2, 10)
    a.label("dwt_loop")
    a.subs_imm(2, 1)
    a.bne("dwt_loop")
    a.ldr_lit(0, 0xE0001004)
    a.ldr_imm(1, 0, 0)
    a.ldr_lit(0, 0x20000000)
    a.str_imm(1, 0, 0)

    # 3. GPIOA/B setup: PA3=1 (AD8232 ON), PA4=0 (SD_CS LOW), PB0=0 (AO3401 ON), PB5=1 (2N7002 ON)
    a.write32(0x40010800, 0x00022000)  # PA3, PA4 Output PP, others Analog
    a.write32(0x40010804, 0x00000000)
    a.write32(0x4001080C, (1 << 3))    # PA3=1, PA4=0 (SD_CS active)
    a.write32(0x40010C00, 0x00200002)  # PB0, PB5 Output PP, others Analog
    a.write32(0x40010C04, 0x00040000)  # PB12 Input floating (100k pull-up)
    a.write32(0x40010C0C, (1 << 5))    # PB0=0 (SD ON), PB5=1 (BAT_SENSE ON)

    # 4. Wait 1 short Sleep tick so AO3401 C_SD1 charges up via board step (use SysTick or skip via direct SPI after first step)
    # Wait: let's do I2C1 & USART1 first, and spin briefly, or use TIM2! Even cleaner: do I2C1 first (INA219 on 3V3_MAIN is already 3.3V!)
    # 4a. I2C1 Read INA219 @ 0x40 Reg 0x02 (Bus Voltage)
    a.write32(0x40005400, 0x0001)  # I2C1 CR1.PE=1
    a.write32(0x40005400, 0x0101)  # START
    a.write32(0x40005410, 0x80)    # Addr 0x40 << 1 | W(0)
    a.write32(0x40005410, 0x02)    # Pointer = Reg 0x02 (Bus Voltage)
    a.write32(0x40005400, 0x0101)  # Repeated START (flushes pointer write!)
    a.write32(0x40005410, 0x81)    # Addr 0x40 << 1 | R(1)
    # Read 2 bytes MSB, LSB from I2C1->DR (0x40005410), then issue STOP using R3/R4
    a.ldr_lit(0, 0x40005410)
    a.ldr_imm(1, 0, 0)             # MSB -> R1
    a.ldr_imm(2, 0, 0)             # LSB -> R2
    a.write32(0x40005400, 0x0201, r_addr=3, r_val=4)  # STOP (preserve R1, R2!)
    # Pack (MSB << 8 | LSB) -> store at SRAM 0x20000008
    a._emit16(0x0209)              # LSLS R1, R1, #8
    a.orrs(1, 2)
    a.ldr_lit(0, 0x20000008)
    a.str_imm(1, 0, 0)

    # 5. USART1 transmit "P05!" (0x50, 0x30, 0x35, 0x21)
    a.write32(0x4001380C, 0x200C)  # UE | TE | RE
    for ch in b"P05!":
        a.write32(0x40013804, ch)

    # 6. Configure DMA1_Ch1 (Circular, 8 halfwords -> SRAM 0x20000100, HTIE | TCIE | MINC | CIRC | MSIZE=16b | EN)
    a.write32(0x40020010, 0x4001244C)  # CPAR = &ADC1->DR
    a.write32(0x40020014, 0x20000100)  # CMAR = 0x20000100
    a.write32(0x4002000C, 8)           # CNDTR = 8 samples
    a.write32(0x40020008, (1 << 10) | (1 << 8) | (1 << 7) | (1 << 5) | (1 << 2) | (1 << 1) | 1)

    # Enable NVIC IRQ 11 (DMA1_Channel1) + IRQ 40 (EXTI15_10)
    a.write32(0xE000E100, 1 << 11)        # ISER0 bit 11
    a.write32(0xE000E104, 1 << (40 - 32)) # ISER1 bit 8

    # Configure ADC1: SQ1=0 (PA0 ECG_OUT), CR2 = EXTTRIG(bit 20) | DMA(bit 8) | ADON(bit 0)
    a.write32(0x40012434, 0)
    a.write32(0x40012408, (1 << 20) | (1 << 8) | 1)

    # Configure TIM2: 72 MHz / (719+1) = 100 kHz; ARR = 399 -> 250 Hz TRGO (CR2.MMS = 0b010 << 4)
    a.write32(0x40000028, 719)
    a.write32(0x4000002C, 399)
    a.write32(0x40000004, 0b010 << 4)
    a.write32(0x40000000, 1)  # CEN = 1

    # 7. Sleep between DMA interrupts until dma_irq_count (SRAM 0x2000000C) >= 2 (HT + TC!)
    a.label("wait_dma")
    a.wfi()  # SLEEP_8M (SLEEPDEEP=0) while waiting for DMA1_Ch1 HTIF/TCIF!
    a.ldr_lit(0, 0x2000000C)
    a.ldr_imm(1, 0, 0)
    a.cmp_imm(1, 2)
    a.bne("wait_dma")

    # 8. Now that AO3401 has charged 3V3_SD to 3.3V during DMA steps -> Send SPI1 CMD0 to VirtualMicroSD!
    a.write32(0x40013000, 0x0044)  # SPI1 SPE | MSTR
    for b_val in (0x40, 0x00, 0x00, 0x00, 0x00, 0x95):
        a.write32(0x4001300C, b_val)
    a.write32(0x4001300C, 0xFF)    # Clock out R1 (0x01) after 1-byte NCR delay!
    a.ldr_lit(0, 0x4001300C)
    a.ldr_imm(1, 0, 0)
    a.ldr_lit(0, 0x20000004)
    a.str_imm(1, 0, 0)

    # 9. Prepare clean STOP MODE (32.1 uA!):
    #    - Stop TIM2, put INA219 into Power-Down (0x0000), turn off AD8232 (PA3=0), AO3401 (PB0=1), 2N7002 (PB5=0)
    a.write32(0x40000000, 0)       # TIM2 off
    a.write32(0x40005400, 0x0101)  # I2C1 START
    a.write32(0x40005410, 0x80)    # 0x40 W
    a.write32(0x40005410, 0x00)    # Reg 0x00 Config
    a.write32(0x40005410, 0x00)    # MSB = 0x00
    a.write32(0x40005410, 0x00)    # LSB = 0x00 (Power-Down!)
    a.write32(0x40005400, 0x0201)  # STOP
    a.write32(0x4001080C, (1 << 4))  # PA3=0 (AD8232 SDN), PA4=1 (SD_CS HIGH)
    a.write32(0x40010C0C, (1 << 0))  # PB0=1 (AO3401 OFF), PB5=0 (2N7002 OFF)
    # Configure EXTI12 falling-edge (SW_WAKE PB12)
    a.write32(0x4001040C, 1 << 12)  # FTSR bit 12
    a.write32(0x40010400, 1 << 12)  # IMR bit 12
    # Set SCB->SCR.SLEEPDEEP = 1 (0x04) & PWR->CR.PDDS = 0 -> STOP MODE!
    a.write32(0x40007000, 0x00000001)  # LPDS=1, PDDS=0
    a.write32(0xE000ED10, 0x00000004)  # SLEEPDEEP=1
    a.wfi()                            # <--- ENTERS 32.1 uA STOP MODE UNTIL SW_WAKE PRESSED!

    # 10. Woken up by SW_WAKE! Clear SLEEPDEEP and write 0xBEEF to SRAM 0x20000014
    a.write32(0xE000ED10, 0x00000000)
    a.write32(0x20000014, 0xBEEF)
    a.label("done_loop")
    a.b("done_loop")

    # --- DMA1_Channel1_IRQHandler (Vector 16 + 11 = 27) ---
    a.label("DMA1_Ch1_IRQ")
    a.write32(0x40020004, 0x0F)    # DMA1->IFCR = clear all Ch1 flags
    a.ldr_lit(0, 0x2000000C)
    a.ldr_imm(1, 0, 0)
    a.adds_imm(1, 1)
    a.str_imm(1, 0, 0)
    a.bx_lr()                      # Returns via EXC_RETURN (0xFFFFFFF9)!

    # --- EXTI15_10_IRQHandler (Vector 16 + 40 = 56) ---
    a.label("EXTI15_10_IRQ")
    a.write32(0x40010414, 1 << 12)  # EXTI->PR = clear line 12
    a.write32(0x20000010, 0xCAFE)   # Mark wakeup ISR executed!
    a.bx_lr()

    return a.finalize_image({
        1: "Reset_Handler",
        16 + 11: "DMA1_Ch1_IRQ",
        16 + 40: "EXTI15_10_IRQ",
    })


@unittest.skipIf(UnicornBackend is None, "unicorn package not installed")
class TestPhase3UnicornCortexM3(unittest.TestCase):
    def test_full_arm_cortex_m3_firmware_on_p05_virtual_board(self) -> None:
        fw_bin = build_p05_bringup_firmware()
        board = P05VirtualBoard.from_hardware(initial_soc_pct=85.0, seed=42)
        backend = UnicornBackend()

        # Flash binary via temporary .bin file to test real file loader too!
        with tempfile.TemporaryDirectory() as td:
            bin_path = Path(td) / "p05_bringup_test.bin"
            bin_path.write_bytes(fw_bin)
            info = board.attach_firmware(backend, bin_path)

        self.assertEqual(info.flash_bytes, len(fw_bin))
        self.assertIsNone(backend.get_cpu_snapshot().fault)

        # Run 40 ms (10 steps @ 4 ms = 250 Hz):
        #   - Boots Reset_Handler, configures 72 MHz PLL + 8 MHz MCO, reads DWT, I2C1 INA219, sends "P05!" UART1
        #   - Captures 8 real AD8232 ECG samples via TIM2 TRGO -> ADC1 -> DMA1_Ch1 circular + 2 DMA IRQs (HT+TC)
        #   - Sends SPI1 CMD0 to VirtualMicroSD, then cleanly enters 32.1 uA STOP MODE on WFI!
        last_sample = None
        for _ in range(15):
            last_sample = board.step(4000.0)
            self.assertIsNone(backend.get_cpu_snapshot().fault)

        assert last_sample is not None

        # 1. Verify RCC PLL 72 MHz & PA8 TP_MCO == 8.0 MHz
        self.assertAlmostEqual(board.matrix.mco_hz, 8_000_000.0, delta=1.0)
        self.assertAlmostEqual(backend.mmio.sysclk_hz, 72_000_000.0, delta=1.0)

        # 2. Verify DWT->CYCCNT counted real executed ARM instructions (> 0)
        dwt_val = struct.unpack("<I", backend.read_sram(0x20000000, 4))[0]
        self.assertGreater(dwt_val, 20)

        # 3. Verify I2C1 read real INA219 @ 0x40 Bus Voltage (~3300 mV)
        ina_bus_raw = struct.unpack("<I", backend.read_sram(0x20000008, 4))[0]
        ina_bus_mv = (ina_bus_raw >> 3) * 4
        self.assertGreaterEqual(ina_bus_mv, 3280)
        self.assertLessEqual(ina_bus_mv, 3310)

        # 4. Verify USART1 transmitted "P05!"
        uart_out = bytes(board.matrix.uart1_tx_fifo)
        self.assertEqual(uart_out, b"P05!")

        # 5. Verify TIM2 + ADC1 + DMA1 captured 8 non-zero 12-bit ECG samples into SRAM 0x20000100 & fired HT+TC IRQs!
        dma_irq_cnt = struct.unpack("<I", backend.read_sram(0x2000000C, 4))[0]
        self.assertGreaterEqual(dma_irq_cnt, 2)
        adc_samples = struct.unpack("<8H", backend.read_sram(0x20000100, 16))
        for s_val in adc_samples:
            self.assertGreater(s_val, 1000)   # Centered around VREF=1.65V -> ~2048 counts!
            self.assertLess(s_val, 3100)

        # 6. Verify SPI1 CMD0 received R1 == 0x01 from VirtualMicroSD!
        sd_r1 = struct.unpack("<I", backend.read_sram(0x20000004, 4))[0]
        self.assertEqual(sd_r1, 0x01)

        # 7. Verify Firmware entered STOP MODE via real WFI (0xBF30) and Virtual PPK2 dropped to EXACTLY 32.1 uA!
        self.assertEqual(backend.get_cpu_snapshot().power_mode, McuPowerMode.STOP)
        self.assertAlmostEqual(last_sample.i_total_ua, 32.1, places=1)

        # 8. Press physical SW_WAKE button (PB12 -> EXTI12) -> verify CPU WAKES UP, runs EXTI15_10_IRQHandler, and resumes main!
        board.set_wake_button(True)
        woke_sample = board.step(4000.0)
        board.set_wake_button(False)
        self.assertIsNone(backend.get_cpu_snapshot().fault)
        self.assertEqual(backend.get_cpu_snapshot().power_mode, McuPowerMode.RUN_72MHZ)
        self.assertGreater(woke_sample.i_total_ua, 25000.0)  # Back to ~27 mA Run 72MHz!

        exti_marker = struct.unpack("<I", backend.read_sram(0x20000010, 4))[0]
        main_resume_marker = struct.unpack("<I", backend.read_sram(0x20000014, 4))[0]
        self.assertEqual(exti_marker, 0xCAFE)
        self.assertEqual(main_resume_marker, 0xBEEF)


if __name__ == "__main__":
    unittest.main(verbosity=2)
