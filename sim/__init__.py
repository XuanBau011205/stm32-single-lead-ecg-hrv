"""
P05-Sim: Python Virtual Hardware Twin for P05 Single-Lead ECG (AD8232 + STM32F103C8T6).

Phase 1 packages the verified Altium Gate G1/G2 hardware (ERC=0, DRC=0, 80x55mm, 91 parts)
into an executable Python Virtual Board (`P05VirtualBoard`) with a clean `IFirmwareBackend`
socket ready for real ARM Cortex-M3 (.elf/.bin) and Fast SIL (.dll) firmware execution.
"""

from .board import P05VirtualBoard

__all__ = ["P05VirtualBoard"]
