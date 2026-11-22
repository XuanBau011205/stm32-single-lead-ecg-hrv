"""
IFirmwareBackend — Unified Firmware Runtime Socket for P05VirtualBoard.

Allows P05VirtualBoard (Phase 1) to plug directly into:
  - Phase 2: Fast SIL C-DLL Backend (p05_sil.dll compiled from firmware/DSP + HIL)
  - Phase 3: Unicorn Engine ARM Cortex-M3 Backend (flashes real P05_ECG_Firmware.elf/.bin)
  - Default: NoFirmwareStub (idle hardware-only bring-up / power-state testing)
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Optional

from sim.core.pin_matrix import VirtualPinMatrix


class McuPowerMode(str, Enum):
    OFF = "OFF"
    STOP = "STOP"          # Stop Mode (LDO On, LSE RTC On): ~24.0 uA
    SLEEP_8MHZ = "SLEEP_8M"  # Sleep between samples @ 8 MHz: ~1.8 mA
    RUN_8MHZ = "RUN_8M"      # Run @ 8 MHz (Q15 optimized): ~4.2 mA
    RUN_72MHZ = "RUN_72M"    # Run @ 72 MHz (PLL 9x, Soft-Float baseline): ~27.0 mA


@dataclass
class FirmwareInfo:
    name: str
    backend_kind: str
    path: Optional[Path] = None
    flash_bytes: int = 0
    sram_bytes: int = 0
    entry_point: int = 0


@dataclass
class CpuSnapshot:
    power_mode: McuPowerMode = McuPowerMode.STOP
    sysclk_hz: float = 0.0
    pc: int = 0
    sp: int = 0x20005000
    dwt_cyccnt: int = 0
    fault: Optional[str] = None


@dataclass
class StepTelemetry:
    power_mode: McuPowerMode
    sysclk_hz: float
    cycles_executed: int = 0
    irq_count: int = 0


class IFirmwareBackend(ABC):
    """Abstract interface every firmware execution backend must implement."""

    @abstractmethod
    def load_firmware(self, path: Path) -> FirmwareInfo:
        """Flash/load firmware binary (.elf/.bin/.hex or .dll) into the virtual MCU."""

    @abstractmethod
    def attach_matrix(self, matrix: VirtualPinMatrix) -> None:
        """Bind backend MMIO/HAL hooks to the board's VirtualPinMatrix."""

    @abstractmethod
    def reset(self) -> None:
        """Perform hardware reset (NRST falling/rising edge)."""

    @abstractmethod
    def step(self, dt_us: float) -> StepTelemetry:
        """Advance MCU execution by `dt_us` microseconds."""

    @abstractmethod
    def get_cpu_snapshot(self) -> CpuSnapshot:
        """Inspect current MCU state (PC, SP, SYSCLK, DWT->CYCCNT, power mode)."""


class HardwareOnlyBackend(IFirmwareBackend):
    """
    Default stub when no firmware binary is flashed yet.
    Lets tests and the interactive UI directly set MCU power mode & GPIO pins
    to verify analog AFE, Power Tree, INA219, and microSD physics independently.
    """

    def __init__(self) -> None:
        self._matrix: Optional[VirtualPinMatrix] = None
        self.power_mode: McuPowerMode = McuPowerMode.STOP
        self.sysclk_hz: float = 0.0
        self.dwt_cyccnt: int = 0

    def load_firmware(self, path: Path) -> FirmwareInfo:
        return FirmwareInfo(name=path.name, backend_kind="hardware_only_stub", path=path)

    def attach_matrix(self, matrix: VirtualPinMatrix) -> None:
        self._matrix = matrix

    def set_mode(self, mode: McuPowerMode) -> None:
        self.power_mode = mode
        if mode == McuPowerMode.RUN_72MHZ:
            self.sysclk_hz = 72e6
        elif mode in (McuPowerMode.RUN_8MHZ, McuPowerMode.SLEEP_8MHZ):
            self.sysclk_hz = 8e6
        else:
            self.sysclk_hz = 0.0

    def reset(self) -> None:
        self.dwt_cyccnt = 0
        self.set_mode(McuPowerMode.RUN_8MHZ)

    def step(self, dt_us: float) -> StepTelemetry:
        cycles = 0
        if self.power_mode in (McuPowerMode.RUN_8MHZ, McuPowerMode.RUN_72MHZ):
            cycles = int(self.sysclk_hz * dt_us * 1e-6)
            self.dwt_cyccnt = (self.dwt_cyccnt + cycles) & 0xFFFFFFFF
        return StepTelemetry(
            power_mode=self.power_mode,
            sysclk_hz=self.sysclk_hz,
            cycles_executed=cycles,
        )

    def get_cpu_snapshot(self) -> CpuSnapshot:
        return CpuSnapshot(
            power_mode=self.power_mode,
            sysclk_hz=self.sysclk_hz,
            dwt_cyccnt=self.dwt_cyccnt,
        )
