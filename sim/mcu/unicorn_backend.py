"""
UnicornBackend — Real ARM Cortex-M3 (.elf / .bin / .hex) Execution Backend for P05VirtualBoard.

Flashes compiled ARM Thumb-2 firmware into STM32F103C8T6 memory (64 KB Flash @ 0x08000000,
20 KB SRAM @ 0x20000000), connects every peripheral register access to `STM32F103MMIO` +
`CortexM3Nvic` + `VirtualPinMatrix`, supports genuine `WFI` Sleep/Stop Mode + EXTI12 wakeup,
`DWT->CYCCNT` cycle counting, and SRAM Stack Watermark supervision!
"""

from __future__ import annotations

import struct
from pathlib import Path
from typing import Optional

from unicorn import (
    UC_ARCH_ARM,
    UC_HOOK_CODE,
    UC_MODE_MCLASS,
    UC_MODE_THUMB,
    UC_PROT_ALL,
    UC_PROT_READ,
    UC_PROT_WRITE,
    Uc,
    UcError,
)
from unicorn.arm_const import (
    UC_ARM_REG_CPSR,
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_SP,
    UC_CPU_ARM_CORTEX_M3,
)

from sim.core.pin_matrix import VirtualPinMatrix
from sim.mcu.backend_base import (
    CpuSnapshot,
    FirmwareInfo,
    IFirmwareBackend,
    McuPowerMode,
    StepTelemetry,
)
from sim.mcu.cortex_m3_nvic import EXC_TRAP_BASE, EXC_TRAP_SIZE, CortexM3Nvic
from sim.mcu.elf_loader import FLASH_BASE, FLASH_SIZE, SRAM_BASE, SRAM_SIZE, LoadedImage, load_firmware_image
from sim.mcu.stm32f103_mmio import PERIPH_BASE, PERIPH_SIZE, STM32F103MMIO

PPB_BASE = 0xE0000000
PPB_SIZE = 0x00100000  # 1 MB (DWT, NVIC, SysTick, SCB, CoreDebug)
SRAM_STACK_WARN_LIMIT = SRAM_BASE + SRAM_SIZE - 0x800  # 2 KB _Min_Stack_Size watermark (0x20004800)


class UnicornBackend(IFirmwareBackend):
    """Real ARM Cortex-M3 emulator backend powered by Unicorn Engine 2."""

    def __init__(self, max_insns_per_step: int = 20000) -> None:
        self.max_insns_per_step = int(max_insns_per_step)
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        try:
            self.uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M3)
        except Exception:
            pass

        self.nvic = CortexM3Nvic()
        self.mmio = STM32F103MMIO(self.nvic)
        self.matrix: Optional[VirtualPinMatrix] = None
        self.image: Optional[LoadedImage] = None

        self.power_mode: McuPowerMode = McuPowerMode.STOP
        self.fault: Optional[str] = None
        self.min_sp_seen: int = SRAM_BASE + SRAM_SIZE
        self._wfi_hit: bool = False
        self._exc_ret_hit: bool = False
        self._step_cycles: int = 0

        self._init_memory_and_hooks()

    def _init_memory_and_hooks(self) -> None:
        # 1. Alias Flash at 0x00000000 (BOOT0=0) + Main Flash at 0x08000000 (64 KB each)
        self.uc.mem_map(0x00000000, FLASH_SIZE, UC_PROT_READ | 4)  # RX
        self.uc.mem_map(FLASH_BASE, FLASH_SIZE, UC_PROT_READ | 4)  # RX

        # 2. SRAM at 0x20000000 (20 KB, RWX)
        self.uc.mem_map(SRAM_BASE, SRAM_SIZE, UC_PROT_ALL)

        # 3. STM32F103 Peripherals MMIO (0x40000000 .. 0x4002FFFF)
        self.uc.mmio_map(
            PERIPH_BASE,
            PERIPH_SIZE,
            self._on_periph_read,
            None,
            self._on_periph_write,
            None,
        )

        # 4. Cortex-M3 Private Peripheral Bus MMIO (0xE0000000 .. 0xE00FFFFF)
        self.uc.mmio_map(
            PPB_BASE,
            PPB_SIZE,
            self._on_ppb_read,
            None,
            self._on_ppb_write,
            None,
        )

        # 5. EXC_RETURN Trap Page (0xFFFFF000 .. 0xFFFFFFFF) for clean ISR unstacking
        self.uc.mem_map(EXC_TRAP_BASE, EXC_TRAP_SIZE, UC_PROT_READ | 4)
        self.uc.mem_write(EXC_TRAP_BASE, b"\x00\xbf" * (EXC_TRAP_SIZE // 2))  # NOP sled

        # Code hooks: (a) EXC_RETURN trap, (b) Flash instruction hook for WFI & DWT cycles
        self.uc.hook_add(UC_HOOK_CODE, self._on_exc_trap, begin=EXC_TRAP_BASE, end=0xFFFFFFFF)
        self.uc.hook_add(UC_HOOK_CODE, self._on_code_step, begin=0x00000000, end=SRAM_BASE + SRAM_SIZE)

    # --- MMIO Callbacks ---
    def _on_periph_read(self, uc: Uc, offset: int, size: int, user_data: object) -> int:
        return self.mmio.mmio_read(offset, size)

    def _on_periph_write(self, uc: Uc, offset: int, size: int, value: int, user_data: object) -> None:
        self.mmio.mmio_write(offset, size, value)

    def _on_ppb_read(self, uc: Uc, offset: int, size: int, user_data: object) -> int:
        return self.nvic.mmio_read(offset, size)

    def _on_ppb_write(self, uc: Uc, offset: int, size: int, value: int, user_data: object) -> None:
        self.nvic.mmio_write(offset, size, value)

    # --- Code Hooks (EXC_RETURN & WFI & DWT->CYCCNT) ---
    def _on_exc_trap(self, uc: Uc, address: int, size: int, user_data: object) -> None:
        self._exc_ret_hit = True
        uc.emu_stop()

    def _on_code_step(self, uc: Uc, address: int, size: int, user_data: object) -> None:
        # Track stack watermark
        sp = uc.reg_read(UC_ARM_REG_SP)
        if SRAM_BASE <= sp < self.min_sp_seen:
            self.min_sp_seen = sp

        # Check Thumb WFI (0xBF30) / WFE (0xBF20) before executing
        if size == 2:
            opcode = struct.unpack("<H", uc.mem_read(address, 2))[0]
            if opcode in (0xBF30, 0xBF20):
                self._wfi_hit = True
                # Advance PC past the 2-byte WFI so resume continues at the next instruction
                uc.reg_write(UC_ARM_REG_PC, (address + 2) | 1)
                uc.emu_stop()
                return

        # Approximate Cortex-M3 cycle weight: 2-byte ALU = 1 cyc, 4-byte Thumb-2 = 2 cyc
        cyc = 1 if size == 2 else 2
        self._step_cycles += cyc
        self.nvic.add_cycles(cyc)

    def _write_sram_bytes(self, addr: int, data: bytes) -> None:
        if SRAM_BASE <= addr and addr + len(data) <= SRAM_BASE + SRAM_SIZE:
            self.uc.mem_write(addr, data)

    # --- IFirmwareBackend Implementation ---
    def attach_matrix(self, matrix: VirtualPinMatrix) -> None:
        self.matrix = matrix
        self.mmio.attach(matrix, self._write_sram_bytes)

    def load_firmware(self, path: Path) -> FirmwareInfo:
        """Flash `.elf`, `.bin`, or `.hex` image directly into STM32F103C8T6 virtual Flash."""
        self.image = load_firmware_image(path)
        self.load_raw_flash_bytes_from_image(self.image)
        return FirmwareInfo(
            name=self.image.path.name,
            backend_kind=f"unicorn_cortex_m3 ({self.image.fmt})",
            path=self.image.path,
            flash_bytes=self.image.total_flash_bytes,
            sram_bytes=SRAM_SIZE,
            entry_point=self.image.entry_point,
        )

    def load_raw_flash_bytes(self, flash_data: bytes, name: str = "inline.bin") -> FirmwareInfo:
        """Convenience helper to flash an in-memory ARM binary directly at 0x08000000."""
        if len(flash_data) > FLASH_SIZE:
            raise ValueError(f"Binary size {len(flash_data)} exceeds 64 KB Flash")
        self.uc.mem_write(FLASH_BASE, b"\xFF" * FLASH_SIZE)
        self.uc.mem_write(0x00000000, b"\xFF" * FLASH_SIZE)
        self.uc.mem_write(FLASH_BASE, flash_data)
        self.uc.mem_write(0x00000000, flash_data)
        entry = struct.unpack("<I", flash_data[4:8])[0] if len(flash_data) >= 8 else FLASH_BASE
        self.image = LoadedImage(
            path=Path(name),
            fmt="bin",
            segments=[(FLASH_BASE, flash_data)],
            entry_point=entry,
            total_flash_bytes=len(flash_data),
        )
        return FirmwareInfo(
            name=name,
            backend_kind="unicorn_cortex_m3 (raw)",
            flash_bytes=len(flash_data),
            sram_bytes=SRAM_SIZE,
            entry_point=entry,
        )

    def load_raw_flash_bytes_from_image(self, img: LoadedImage) -> None:
        # Erase Flash to 0xFF and write all loadable segments
        self.uc.mem_write(FLASH_BASE, b"\xFF" * FLASH_SIZE)
        self.uc.mem_write(0x00000000, b"\xFF" * FLASH_SIZE)
        for addr, data in img.segments:
            self.uc.mem_write(addr, data)
            if FLASH_BASE <= addr and addr + len(data) <= FLASH_BASE + FLASH_SIZE:
                self.uc.mem_write(addr - FLASH_BASE, data)

    def reset(self) -> None:
        """Hardware Reset: clear SRAM, reset MMIO/NVIC, load initial SP & Reset_Handler from Vector Table."""
        self.nvic.reset()
        self.mmio.reset()
        if self.matrix is not None:
            self.mmio.attach(self.matrix, self._write_sram_bytes)

        self.uc.mem_write(SRAM_BASE, b"\x00" * SRAM_SIZE)
        self.fault = None
        self._wfi_hit = False
        self._exc_ret_hit = False
        self.min_sp_seen = SRAM_BASE + SRAM_SIZE

        if self.image is None:
            self.power_mode = McuPowerMode.STOP
            return

        init_sp, reset_vec = struct.unpack("<2I", self.uc.mem_read(FLASH_BASE, 8))
        if init_sp == 0xFFFFFFFF or reset_vec == 0xFFFFFFFF:
            self.power_mode = McuPowerMode.STOP
            return

        self.uc.reg_write(UC_ARM_REG_SP, init_sp)
        self.uc.reg_write(UC_ARM_REG_LR, 0xFFFFFFFF)
        self.uc.reg_write(UC_ARM_REG_PC, reset_vec | 1)
        self.uc.reg_write(UC_ARM_REG_CPSR, 0x01000000)  # Thumb bit (T=1)
        self.power_mode = McuPowerMode.RUN_8MHZ

    def _active_run_mode(self) -> McuPowerMode:
        return McuPowerMode.RUN_72MHZ if self.mmio.sysclk_hz >= 40_000_000.0 else McuPowerMode.RUN_8MHZ

    def step(self, dt_us: float) -> StepTelemetry:
        """Advance STM32F103 peripherals and execute ARM Cortex-M3 instructions for `dt_us`."""
        if self.image is None or self.fault is not None:
            return StepTelemetry(power_mode=self.power_mode, sysclk_hz=0.0)

        # 1. Tick peripherals (TIM2/3 -> ADC1+DMA1, USART1 RX)
        cpu_awake = self.power_mode not in (McuPowerMode.STOP, McuPowerMode.OFF)
        self.mmio.tick(dt_us, cpu_awake=cpu_awake)

        # 2. Check wakeup from STOP or SLEEP on any enabled pending NVIC exception!
        if self.power_mode in (McuPowerMode.STOP, McuPowerMode.SLEEP_8MHZ):
            if self.nvic.has_enabled_pending_exception():
                self.power_mode = self._active_run_mode()
            else:
                return StepTelemetry(
                    power_mode=self.power_mode,
                    sysclk_hz=0.0 if self.power_mode == McuPowerMode.STOP else self.mmio.sysclk_hz,
                )

        # 3. Execute ARM Cortex-M3 instructions within this quantum's cycle budget
        budget_cycles = max(16, int(self.mmio.sysclk_hz * dt_us * 1e-6))
        insn_cap = min(self.max_insns_per_step, budget_cycles)
        self._step_cycles = 0
        irqs_before = self.nvic.total_irqs_taken
        remaining = insn_cap

        while remaining > 0 and self.fault is None:
            # Dispatch pending interrupt before next instruction slice
            self.nvic.dispatch_pending_exception(self.uc)

            pc = self.uc.reg_read(UC_ARM_REG_PC)
            self._wfi_hit = False
            self._exc_ret_hit = False
            cyc_before = self._step_cycles

            try:
                self.uc.emu_start(pc | 1, FLASH_BASE + FLASH_SIZE, count=remaining)
            except UcError as exc:
                err_pc = self.uc.reg_read(UC_ARM_REG_PC)
                if 0xFFFFFFF0 <= err_pc <= 0xFFFFFFFF and self.nvic.active_exceptions:
                    # Native Cortex-M3 EXC_RETURN (BX LR / POP {PC} with 0xFFFFFFF9)
                    self.nvic.handle_exc_return(self.uc)
                    remaining -= max(1, self._step_cycles - cyc_before)
                    continue
                self.fault = f"HardFault/UcError at PC=0x{err_pc:08X}: {exc}"
                break

            executed = max(1, self._step_cycles - cyc_before)
            remaining -= executed

            if self._exc_ret_hit:
                self.nvic.handle_exc_return(self.uc)
                continue

            if self._wfi_hit:
                # Enter STOP Mode if SCB->SCR.SLEEPDEEP=1 and PWR->CR.PDDS=0, else SLEEP Mode
                if self.nvic.sleepdeep and not (self.mmio.pwr_cr & 0x02):
                    self.power_mode = McuPowerMode.STOP
                else:
                    self.power_mode = McuPowerMode.SLEEP_8MHZ
                break

            # Refresh RUN_8M vs RUN_72M in case firmware just switched PLL
            self.power_mode = self._active_run_mode()
            if not self.nvic.has_enabled_pending_exception():
                break

        return StepTelemetry(
            power_mode=self.power_mode,
            sysclk_hz=0.0 if self.power_mode == McuPowerMode.STOP else self.mmio.sysclk_hz,
            cycles_executed=self._step_cycles,
            irq_count=self.nvic.total_irqs_taken - irqs_before,
        )

    def read_sram(self, addr: int, size: int) -> bytes:
        """Inspect virtual STM32 SRAM directly (for tests & live debugger)."""
        return bytes(self.uc.mem_read(addr, size))

    def get_cpu_snapshot(self) -> CpuSnapshot:
        pc = self.uc.reg_read(UC_ARM_REG_PC) if self.image is not None else 0
        sp = self.uc.reg_read(UC_ARM_REG_SP) if self.image is not None else (SRAM_BASE + SRAM_SIZE)
        return CpuSnapshot(
            power_mode=self.power_mode,
            sysclk_hz=0.0 if self.power_mode == McuPowerMode.STOP else self.mmio.sysclk_hz,
            pc=pc,
            sp=sp,
            dwt_cyccnt=self.nvic.dwt_cyccnt,
            fault=self.fault,
        )
