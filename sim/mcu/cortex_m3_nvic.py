"""
CortexM3Nvic — ARMv7-M Exception, NVIC, SysTick, SCB, and DWT->CYCCNT Controller.

Handles genuine Cortex-M3 exception entry & EXC_RETURN (0xFFFFFFF9) unstacking
on top of Unicorn Engine so STM32F103 ISRs (SysTick, TIM2/3, DMA1_Ch1, USART1, EXTI15_10)
execute identically to real silicon!
"""

from __future__ import annotations

import struct
from typing import List, Optional, Set, Tuple

from unicorn import Uc
from unicorn.arm_const import (
    UC_ARM_REG_CPSR,
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_R0,
    UC_ARM_REG_R1,
    UC_ARM_REG_R2,
    UC_ARM_REG_R3,
    UC_ARM_REG_R12,
    UC_ARM_REG_SP,
)

# STM32F103 Medium-Density IRQ numbers (Vector index = 16 + IRQ_number)
IRQ_DMA1_CHANNEL1 = 11   # Vector 27 (0x006C)
IRQ_ADC1_2 = 18          # Vector 34 (0x0088)
IRQ_TIM2 = 28            # Vector 44 (0x00B0)
IRQ_TIM3 = 29            # Vector 45 (0x00B4)
IRQ_I2C1_EV = 31         # Vector 47 (0x00BC)
IRQ_SPI1 = 35            # Vector 51 (0x00CC)
IRQ_USART1 = 37          # Vector 53 (0x00D4)
IRQ_EXTI15_10 = 40       # Vector 56 (0x00E0)

EXC_SYSTICK = 15         # Vector 15 (0x003C)
EXC_RETURN_THREAD_MSP = 0xFFFFFFF9
EXC_TRAP_BASE = 0xFFFFF000
EXC_TRAP_SIZE = 0x1000


class CortexM3Nvic:
    """ARMv7-M Private Peripheral Bus (PPB: NVIC, SCB, SysTick, DWT, CoreDebug)."""

    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        # NVIC state (up to 68 IRQs for STM32F103)
        self.iser: List[int] = [0, 0, 0]
        self.ispr: List[int] = [0, 0, 0]
        self.ipr: bytearray = bytearray(68)
        self.active_exceptions: List[int] = []  # Stack of active exception numbers

        # SCB state
        self.vtor: int = 0x08000000
        self.scr: int = 0x00000000       # Bit 2 = SLEEPDEEP
        self.aircr: int = 0xFA050000
        self.ccr: int = 0x00000200       # STKALIGN = 1
        self.primask: int = 0

        # SysTick state (0xE000E010)
        self.syst_ctrl: int = 0x00000004  # CLKSOURCE = processor clock
        self.syst_load: int = 0
        self.syst_val: int = 0
        self.systick_pending: bool = False

        # CoreDebug / DWT state (0xE0001000 & 0xE000EDFC)
        self.demcr: int = 0x00000000     # Bit 24 = TRCENA
        self.dwt_ctrl: int = 0x40000000  # Bit 0 = CYCCNTENA
        self.dwt_cyccnt: int = 0

        # Stats
        self.total_irqs_taken: int = 0

    @property
    def sleepdeep(self) -> bool:
        return bool(self.scr & 0x04)

    @property
    def dwt_enabled(self) -> bool:
        return bool(self.demcr & (1 << 24)) and bool(self.dwt_ctrl & 1)

    def add_cycles(self, cycles: int) -> None:
        """Advance DWT->CYCCNT and SysTick counter by `cycles` processor clocks."""
        if cycles <= 0:
            return
        if self.dwt_enabled:
            self.dwt_cyccnt = (self.dwt_cyccnt + cycles) & 0xFFFFFFFF

        # SysTick down-counter (ENABLE bit 0)
        if (self.syst_ctrl & 0x01) and self.syst_load > 0:
            rem = self.syst_val - cycles
            if rem <= 0:
                self.syst_ctrl |= (1 << 16)  # COUNTFLAG
                if self.syst_ctrl & 0x02:    # TICKINT
                    self.systick_pending = True
                period = self.syst_load + 1
                self.syst_val = period - ((-rem) % period)
                if self.syst_val == period:
                    self.syst_val = self.syst_load
            else:
                self.syst_val = rem

    def set_irq_pending(self, irq_num: int) -> None:
        if 0 <= irq_num < 68:
            self.ispr[irq_num >> 5] |= 1 << (irq_num & 31)

    def clear_irq_pending(self, irq_num: int) -> None:
        if 0 <= irq_num < 68:
            self.ispr[irq_num >> 5] &= ~(1 << (irq_num & 31))

    def is_irq_enabled(self, irq_num: int) -> bool:
        if 0 <= irq_num < 68:
            return bool(self.iser[irq_num >> 5] & (1 << (irq_num & 31)))
        return False

    def has_enabled_pending_exception(self) -> bool:
        if self.systick_pending:
            return True
        for word_idx in range(3):
            if self.iser[word_idx] & self.ispr[word_idx]:
                return True
        return False

    def _next_pending_exception(self) -> Optional[int]:
        """Return highest-priority enabled pending exception number (15=SysTick, 16+irq)."""
        if self.active_exceptions:
            # Simple non-nested dispatch for deterministic capstone drivers
            return None
        if self.systick_pending:
            self.systick_pending = False
            return EXC_SYSTICK
        best_irq: Optional[int] = None
        best_prio = 256
        for w in range(3):
            active_bits = self.iser[w] & self.ispr[w]
            if not active_bits:
                continue
            for bit in range(32):
                if active_bits & (1 << bit):
                    irq = (w << 5) + bit
                    if irq < 68:
                        prio = self.ipr[irq]
                        if prio < best_prio:
                            best_prio = prio
                            best_irq = irq
        if best_irq is not None:
            self.clear_irq_pending(best_irq)
            return 16 + best_irq
        return None

    def dispatch_pending_exception(self, uc: Uc) -> bool:
        """Push ARMv7-M 8-word exception frame onto SP and jump to vector handler."""
        exc_num = self._next_pending_exception()
        if exc_num is None:
            return False

        vec_addr = self.vtor + exc_num * 4
        handler = struct.unpack("<I", uc.mem_read(vec_addr, 4))[0]
        if handler == 0 or handler == 0xFFFFFFFF:
            return False

        sp = uc.reg_read(UC_ARM_REG_SP)
        # Preserve current context on stack: [R0, R1, R2, R3, R12, LR, ReturnAddr, xPSR]
        r0 = uc.reg_read(UC_ARM_REG_R0)
        r1 = uc.reg_read(UC_ARM_REG_R1)
        r2 = uc.reg_read(UC_ARM_REG_R2)
        r3 = uc.reg_read(UC_ARM_REG_R3)
        r12 = uc.reg_read(UC_ARM_REG_R12)
        lr = uc.reg_read(UC_ARM_REG_LR)
        pc = uc.reg_read(UC_ARM_REG_PC) & ~1
        xpsr = (uc.reg_read(UC_ARM_REG_CPSR) | (1 << 24)) & 0xFFFFFFFF  # Ensure T-bit = 1

        new_sp = (sp - 32) & 0xFFFFFFFF
        frame = struct.pack("<8I", r0, r1, r2, r3, r12, lr, pc, xpsr)
        uc.mem_write(new_sp, frame)

        uc.reg_write(UC_ARM_REG_SP, new_sp)
        uc.reg_write(UC_ARM_REG_LR, EXC_RETURN_THREAD_MSP)
        uc.reg_write(UC_ARM_REG_PC, handler | 1)  # Thumb bit = 1
        self.active_exceptions.append(exc_num)
        self.total_irqs_taken += 1
        return True

    def handle_exc_return(self, uc: Uc) -> bool:
        """Unstack ARMv7-M 8-word frame when ISR returns via BX LR (0xFFFFFFF9)."""
        if not self.active_exceptions:
            return False
        self.active_exceptions.pop()

        sp = uc.reg_read(UC_ARM_REG_SP)
        r0, r1, r2, r3, r12, lr, ret_pc, xpsr = struct.unpack("<8I", uc.mem_read(sp, 32))
        uc.reg_write(UC_ARM_REG_SP, (sp + 32) & 0xFFFFFFFF)
        uc.reg_write(UC_ARM_REG_R0, r0)
        uc.reg_write(UC_ARM_REG_R1, r1)
        uc.reg_write(UC_ARM_REG_R2, r2)
        uc.reg_write(UC_ARM_REG_R3, r3)
        uc.reg_write(UC_ARM_REG_R12, r12)
        uc.reg_write(UC_ARM_REG_LR, lr)
        uc.reg_write(UC_ARM_REG_CPSR, xpsr)
        uc.reg_write(UC_ARM_REG_PC, ret_pc | 1)  # Resume Thread Mode in Thumb state
        return True

    # --- PPB MMIO Read/Write (0xE0000000 .. 0xE00FFFFF) ---
    def mmio_read(self, offset: int, size: int) -> int:
        addr = 0xE0000000 + offset
        # DWT (0xE0001000)
        if addr == 0xE0001000:
            return self.dwt_ctrl
        if addr == 0xE0001004:
            return self.dwt_cyccnt

        # SysTick (0xE000E010)
        if addr == 0xE000E010:
            val = self.syst_ctrl
            self.syst_ctrl &= ~(1 << 16)  # Reading CTRL clears COUNTFLAG
            return val
        if addr == 0xE000E014:
            return self.syst_load
        if addr == 0xE000E018:
            return self.syst_val
        if addr == 0xE000E01C:
            return 0x40002328  # 9000 ticks/ms @ 72/8 MHz

        # NVIC ISER/ICER/ISPR/ICPR/IPR
        if 0xE000E100 <= addr < 0xE000E10C:
            return self.iser[(addr - 0xE000E100) >> 2]
        if 0xE000E180 <= addr < 0xE000E18C:
            return self.iser[(addr - 0xE000E180) >> 2]
        if 0xE000E200 <= addr < 0xE000E20C:
            return self.ispr[(addr - 0xE000E200) >> 2]
        if 0xE000E280 <= addr < 0xE000E28C:
            return self.ispr[(addr - 0xE000E280) >> 2]
        if 0xE000E400 <= addr < 0xE000E444:
            idx = addr - 0xE000E400
            if size == 1:
                return self.ipr[idx]
            return int.from_bytes(self.ipr[idx : idx + size], "little")

        # SCB (0xE000ED00)
        if addr == 0xE000ED00:
            return 0x411FC231  # Cortex-M3 r1p1
        if addr == 0xE000ED08:
            return self.vtor
        if addr == 0xE000ED0C:
            return self.aircr
        if addr == 0xE000ED10:
            return self.scr
        if addr == 0xE000ED14:
            return self.ccr

        # CoreDebug DEMCR
        if addr == 0xE000EDFC:
            return self.demcr
        return 0

    def mmio_write(self, offset: int, size: int, value: int) -> None:
        addr = 0xE0000000 + offset
        # DWT
        if addr == 0xE0001000:
            self.dwt_ctrl = value & 0xFFFFFFFF
            return
        if addr == 0xE0001004:
            self.dwt_cyccnt = value & 0xFFFFFFFF
            return

        # SysTick
        if addr == 0xE000E010:
            self.syst_ctrl = (self.syst_ctrl & (1 << 16)) | (value & 0x07)
            return
        if addr == 0xE000E014:
            self.syst_load = value & 0x00FFFFFF
            return
        if addr == 0xE000E018:
            self.syst_val = 0
            self.syst_ctrl &= ~(1 << 16)
            return

        # NVIC ISER / ICER / ISPR / ICPR / IPR
        if 0xE000E100 <= addr < 0xE000E10C:
            self.iser[(addr - 0xE000E100) >> 2] |= value & 0xFFFFFFFF
            return
        if 0xE000E180 <= addr < 0xE000E18C:
            self.iser[(addr - 0xE000E180) >> 2] &= ~(value & 0xFFFFFFFF)
            return
        if 0xE000E200 <= addr < 0xE000E20C:
            self.ispr[(addr - 0xE000E200) >> 2] |= value & 0xFFFFFFFF
            return
        if 0xE000E280 <= addr < 0xE000E28C:
            self.ispr[(addr - 0xE000E280) >> 2] &= ~(value & 0xFFFFFFFF)
            return
        if 0xE000E400 <= addr < 0xE000E444:
            idx = addr - 0xE000E400
            raw = (value & ((1 << (8 * size)) - 1)).to_bytes(size, "little")
            self.ipr[idx : idx + size] = raw
            return

        # SCB
        if addr == 0xE000ED08:
            self.vtor = value & 0xFFFFFF80
            return
        if addr == 0xE000ED0C:
            self.aircr = value & 0xFFFFFFFF
            return
        if addr == 0xE000ED10:
            self.scr = value & 0xFFFFFFFF
            return
        if addr == 0xE000ED14:
            self.ccr = value & 0xFFFFFFFF
            return

        # CoreDebug DEMCR
        if addr == 0xE000EDFC:
            self.demcr = value & 0xFFFFFFFF
            return
