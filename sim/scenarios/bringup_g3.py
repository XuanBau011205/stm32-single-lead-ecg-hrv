"""
Automated Gate G3 Bring-Up Scenario on Real Compiled P05_ECG_Firmware.elf.

Verifies end-to-end on P05VirtualBoard + UnicornBackend:
  1. Wearable boot: SYSCLK = 8 MHz, TP_MCO = 8 MHz, AD8232 active, 3V3_SD = 3.3 V, SD initialized.
  2. ECG sampling & SD logging: TIM3 + ADC1 + DMA1 writes >= 1 valid 512B ECG5 block (0 CRC errors).
  3. HIL UART1 round-trip: detach RA + LA (> 50 ms debounce), send ConfigReq + SamplesIn -> ResultAck.
  4. Low-battery Stop Mode & SW_WAKE: SoC = 5% -> STOP MODE <= 48.15 uA (~32.1 uA), then SoC = 85% + PB12 -> Wake.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import Any, Dict, Optional

from sim import P05VirtualBoard
from sim.bridges import (
    HilResultAck,
    VirtualUartHilBridge,
    build_hil_config_req,
    build_hil_samples_in,
    inspect_sdcard,
)
from sim.mcu import McuPowerMode, UnicornBackend


def find_default_elf() -> Optional[Path]:
    env_path = os.environ.get("P05_ELF_PATH")
    candidates = []
    if env_path:
        candidates.append(Path(env_path))
    candidates.extend([
        Path("firmware/build/P05_ECG_Firmware.elf"),
        Path("build/P05_ECG_Firmware.elf"),
    ])
    for c in candidates:
        if c.is_file():
            return c
    return None


def run_bringup_g3(elf_path: Path) -> Dict[str, Any]:
    board = P05VirtualBoard.from_hardware(initial_soc_pct=85.0, seed=42)
    backend = UnicornBackend()
    info = board.attach_firmware(backend, elf_path)
    bridge = VirtualUartHilBridge(board.matrix)

    # 1 & 2. Run Wearable mode for 1.2 s (300 steps @ 4 ms / 250 Hz)
    for _ in range(300):
        board.step(4000.0)
        cpu = backend.get_cpu_snapshot()
        if cpu.fault:
            raise RuntimeError(f"CPU fault during WearableRun: {cpu.fault}")

    summ_wear = board.get_summary()
    sd_report = inspect_sdcard(board.sdcard)

    check_boot = (
        summ_wear["sd_initialized"]
        and abs(summ_wear["testpoints"]["TP_MCO_Hz"] - 8_000_000.0) < 1.0
        and summ_wear["rails_v"]["3V3_SD"] >= 3.2
        and board.matrix.digital["ECG_SDN"] is True
        and summ_wear["ina219_quantized_ua"] > 0.0
    )
    check_sd = sd_report.ok and len(sd_report.valid_raw_blocks) >= 1 and sd_report.valid_raw_blocks[0].sample_count == 245

    # 3. HIL UART1 round-trip (detach both RA + LA and wait 60 ms at 1 ms SysTick ticks for 50 ms debounce)
    board.set_electrodes(ra=False, la=False, rl=True)
    for _ in range(60):
        board.step(1000.0)

    bridge.inject_rx_frame(build_hil_config_req(seq=1, fs_hz=250, soft_float=False, interp_enable=True, reset_state=True))
    for _ in range(5):
        board.step(1000.0)

    samples = [2048 + (i % 64) for i in range(20)]
    bridge.inject_rx_frame(build_hil_samples_in(seq=2, first_sample_idx=0, samples=samples))
    for _ in range(10):
        board.step(1000.0)

    hil_responses = bridge.drain_tx_frames()
    acks = [r for r in hil_responses if isinstance(r, HilResultAck)]
    check_hil = (
        len(acks) >= 2
        and acks[0].ack_seq == 1
        and acks[0].canonical == 0
        and acks[1].ack_seq == 2
        and acks[1].canonical == 0
        and acks[1].mean_cycles > 0
    )

    # 4. Low-battery Stop Mode (~32.1 uA) & SW_WAKE recovery
    board.set_electrodes(ra=True, la=True, rl=True)
    board.power.set_battery_soc(5.0)
    board._settle_rails(1000.0)
    board.press_reset_button()
    s_stop = None
    for _ in range(5):
        s_stop = board.step(4000.0)
    assert s_stop is not None
    check_stop = (
        backend.power_mode == McuPowerMode.STOP
        and s_stop.i_total_ua <= 48.15
        and abs(s_stop.i_total_ua - 32.1) <= 1.0
    )

    board.power.set_battery_soc(85.0)
    board._settle_rails(1000.0)
    board.set_wake_button(True)
    for i in range(8):
        if i == 2:
            board.set_wake_button(False)
        board.step(4000.0)
    check_wake = backend.power_mode in (McuPowerMode.RUN_8MHZ, McuPowerMode.SLEEP_8MHZ) and board.sdcard.is_initialized

    results = {
        "firmware": info.name if info else str(elf_path),
        "flash_bytes": info.flash_bytes if info else 0,
        "wearable_boot_mco_ina219_sd": check_boot,
        "sd_raw_512b_crc_ok": check_sd,
        "sd_blocks_written": len(sd_report.valid_raw_blocks),
        "hil_uart1_roundtrip": check_hil,
        "stop_mode_32_1ua": check_stop,
        "stop_ua": round(s_stop.i_total_ua, 2),
        "sw_wake_resume": check_wake,
        "all_passed": bool(check_boot and check_sd and check_hil and check_stop and check_wake),
    }
    return results


def main() -> int:
    elf_path = Path(sys.argv[1]) if len(sys.argv) > 1 else find_default_elf()
    if elf_path is None or not elf_path.is_file():
        print("[bringup_g3] ERROR: P05_ECG_Firmware.elf not found. Pass path as arg or set P05_ELF_PATH.")
        return 2

    res = run_bringup_g3(elf_path)
    print("=== P05-Sim Gate G3 Bring-Up Report ===")
    for k, v in res.items():
        print(f"  {k}: {v}")
    return 0 if res["all_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
