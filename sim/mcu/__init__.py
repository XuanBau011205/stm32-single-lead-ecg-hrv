"""MCU execution backends package (HardwareOnlyBackend + UnicornBackend ARM Cortex-M3)."""

from .backend_base import (
    CpuSnapshot,
    FirmwareInfo,
    HardwareOnlyBackend,
    IFirmwareBackend,
    McuPowerMode,
    StepTelemetry,
)

try:
    from .unicorn_backend import UnicornBackend
except ImportError:  # unicorn optional for Phase-1-only environments
    UnicornBackend = None  # type: ignore

__all__ = [
    "IFirmwareBackend",
    "HardwareOnlyBackend",
    "UnicornBackend",
    "McuPowerMode",
    "FirmwareInfo",
    "CpuSnapshot",
    "StepTelemetry",
]
