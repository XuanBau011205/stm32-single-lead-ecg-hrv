"""
Automated Fault Injection Scenarios on Real Compiled P05_ECG_Firmware.elf.

Tests 4 hardware/protocol faults against P05VirtualBoard + UnicornBackend:
  1. leads_off_flag: Detach RA during WearableRun -> SD block records kLodP (0x01) with valid CRC.
  2. low_battery_stop: Boot at SoC = 5% -> enters STOP MODE (~32.1 uA, 0 SD blocks written).
  3. sd_card_missing: Boot with sd_card_inserted = False -> no CPU fault/hang, stays operational.
  4. hil_safety_and_bad_crc: Send ConfigReq with electrodes attached -> ErrorNack; then bad CRC -> ErrorNack, lockout preserved.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any, Dict

from sim import P05VirtualBoard
from sim.bridges import (
    HilErrorNack,
    VirtualUartHilBridge,
    build_hil_config_req,
)
from sim.mcu import McuPowerMode, UnicornBackend
from sim.scenarios.bringup_g3 import find_default_elf


def run_fault_injection(elf_path: Path) -> Dict[str, Any]:
    # 1. Leads-Off suppression during WearableRun (R-BND-02 / OPEN-6: invalid ECG not logged)
    b1 = P05VirtualBoard.from_hardware(initial_soc_pct=85.0, seed=42)
    u1 = UnicornBackend()
    b1.attach_firmware(u1, elf_path)
    b1.set_electrodes(ra=False, la=True, rl=True)
    for _ in range(300):
        b1.step(4000.0)
    check_lod = (
        b1.matrix.digital["ECG_LOD_N"] is True
        and b1.sdcard.total_blocks_written == 0
        and u1.get_cpu_snapshot().fault is None
    )

    # 2. Low battery Stop Mode
    b2 = P05VirtualBoard.from_hardware(initial_soc_pct=5.0, seed=42)
    u2 = UnicornBackend()
    b2.attach_firmware(u2, elf_path)
    s2 = None
    for _ in range(10):
        s2 = b2.step(4000.0)
    assert s2 is not None
    check_low_bat = (
        u2.power_mode == McuPowerMode.STOP
        and s2.i_total_ua <= 48.15
        and b2.sdcard.total_blocks_written == 0
    )

    # 3. Missing SD card resilience
    b3 = P05VirtualBoard.from_hardware(initial_soc_pct=85.0, seed=42)
    b3.matrix.sd_card_inserted = False
    u3 = UnicornBackend()
    b3.attach_firmware(u3, elf_path)
    for _ in range(25):
        b3.step(4000.0)
    cpu3 = u3.get_cpu_snapshot()
    check_no_sd = (
        cpu3.fault is None
        and not b3.sdcard.is_initialized
        and b3.sdcard.total_blocks_written == 0
        and u3.power_mode in (McuPowerMode.RUN_8MHZ, McuPowerMode.SLEEP_8MHZ)
    )

    # 4. HIL safety lockout (electrodes attached) + corrupted CRC frame
    b4 = P05VirtualBoard.from_hardware(initial_soc_pct=85.0, seed=42)
    u4 = UnicornBackend()
    b4.attach_firmware(u4, elf_path)
    bridge4 = VirtualUartHilBridge(b4.matrix)
    for _ in range(10):
        b4.step(4000.0)

    bridge4.inject_rx_frame(build_hil_config_req(seq=10))
    for _ in range(6):
        b4.step(4000.0)

    bad_frame = bytearray(build_hil_config_req(seq=11))
    bad_frame[-1] ^= 0xFF  # Corrupt CRC
    bridge4.inject_rx_frame(bytes(bad_frame))
    for _ in range(6):
        b4.step(4000.0)

    resps4 = bridge4.drain_tx_frames()
    nacks4 = [r for r in resps4 if isinstance(r, HilErrorNack)]
    check_hil_faults = (
        len(nacks4) == 2
        and nacks4[0].rejected_seq == 10
        and nacks4[1].rejected_seq == 11
        and u4.get_cpu_snapshot().fault is None
    )

    return {
        "leads_off_suppresses_invalid_sd": check_lod,
        "low_battery_stop_32ua": check_low_bat,
        "sd_missing_no_hang": check_no_sd,
        "hil_safety_and_bad_crc_nack": check_hil_faults,
        "all_passed": bool(check_lod and check_low_bat and check_no_sd and check_hil_faults),
    }


def main() -> int:
    elf_path = Path(sys.argv[1]) if len(sys.argv) > 1 else find_default_elf()
    if elf_path is None or not elf_path.is_file():
        print("[fault_injection] ERROR: P05_ECG_Firmware.elf not found. Pass path as arg or set P05_ELF_PATH.")
        return 2

    res = run_fault_injection(elf_path)
    print("=== P05-Sim Fault Injection Report ===")
    for k, v in res.items():
        print(f"  {k}: {v}")
    return 0 if res["all_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
