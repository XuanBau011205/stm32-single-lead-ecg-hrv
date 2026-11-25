"""
Automated Gate G4 Ablation Suite on Real Compiled P05_ECG_Firmware.elf.

Runs all 3 Gate G4 experiments against P05VirtualBoard + UnicornBackend:
  1. Ablation 1 (Q15 vs Soft-Float): Measures DWT->CYCCNT per sample via HIL UART1.
  2. Ablation 2 (250 Hz vs 500 Hz): Measures R-peak detection & RR MAE (us) via HIL UART1.
  3. Ablation 3 (Active RLD vs VREF_GND): Measures 50 Hz common-mode rejection on TP_ECG_OUT.
"""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path
from typing import Any, Dict, List, Optional

from sim import P05VirtualBoard
from sim.bridges import (
    HilPeakOut,
    HilResultAck,
    VirtualUartHilBridge,
    build_hil_config_req,
    build_hil_samples_in,
)
from sim.mcu import UnicornBackend
from sim.plant import PatientECGSource
from sim.scenarios.bringup_g3 import find_default_elf


def _generate_ecg_adc_samples(fs_hz: int, duration_s: float, hr_bpm: float = 72.0) -> List[int]:
    patient = PatientECGSource(
        hr_bpm=hr_bpm,
        hrv_sdnn_ms=0.0,
        cm_50hz_mv=0.0,
        emg_uv_rms=0.0,
        baseline_mv=0.0,
        seed=42,
    )

    dt_s = 1.0 / float(fs_hz)
    n_samples = int(round(duration_s * fs_hz))
    out: List[int] = []
    for i in range(n_samples):
        sig = patient.sample(i * dt_s, dt_s)
        counts = int(round(2048.0 + sig.v_diff_mv * 900.0))
        out.append(max(0, min(4095, counts)))
    return out


def _run_hil_stream(
    elf_path: Path,
    samples: List[int],
    fs_hz: int,
    soft_float: bool,
    interp_enable: bool = True,
) -> Dict[str, Any]:
    board = P05VirtualBoard.from_hardware(initial_soc_pct=85.0, seed=42)
    backend = UnicornBackend()
    board.attach_firmware(backend, elf_path)
    bridge = VirtualUartHilBridge(board.matrix)

    # Boot and qualify 50 ms debounced Leads-Off for safe HIL entry
    board.set_electrodes(ra=False, la=False, rl=True)
    for _ in range(60):
        board.step(1000.0)

    seq = 1
    bridge.inject_rx_frame(
        build_hil_config_req(
            seq=seq,
            fs_hz=fs_hz,
            soft_float=soft_float,
            interp_enable=interp_enable,
            reset_state=True,
        )
    )
    for _ in range(5):
        board.step(1000.0)
    cfg_acks = [r for r in bridge.drain_tx_frames() if isinstance(r, HilResultAck)]
    if not cfg_acks or cfg_acks[0].canonical != 0:
        raise RuntimeError(f"HIL ConfigReq failed (fs={fs_hz}, soft_float={soft_float}): {cfg_acks}")

    chunk_size = 32
    mean_cycles_list: List[int] = []
    max_cycles = 0
    peaks: List[HilPeakOut] = []

    for start in range(0, len(samples), chunk_size):
        seq += 1
        chunk = samples[start : start + chunk_size]
        bridge.inject_rx_frame(build_hil_samples_in(seq=seq, first_sample_idx=start, samples=chunk))
        acks: List[HilResultAck] = []
        for _ in range(40):
            board.step(1000.0)
            cpu = backend.get_cpu_snapshot()
            if cpu.fault:
                raise RuntimeError(f"CPU fault during HIL stream: {cpu.fault}")
            acks.extend(r for r in bridge.drain_tx_frames() if isinstance(r, HilResultAck))
            if any(a.ack_seq == seq for a in acks):
                break

        matched = [a for a in acks if a.ack_seq == seq]
        if not matched or matched[0].canonical != 0:
            raise RuntimeError(f"Missing or non-valid ResultAck for seq={seq}: {matched}")
        ack = matched[0]
        mean_cycles_list.append(ack.mean_cycles)
        max_cycles = max(max_cycles, ack.max_cycles)
        peaks.extend(ack.peaks)

    avg_mean_cycles = sum(mean_cycles_list) / max(1, len(mean_cycles_list))
    return {
        "mean_cycles": avg_mean_cycles,
        "max_cycles": max_cycles,
        "peaks": peaks,
        "rr_intervals_us": [p.rr_interval_us for p in peaks if p.rr_interval_us > 0],
    }


def run_ablation_suite(elf_path: Path, output_csv: Optional[Path] = None) -> Dict[str, Any]:
    # Ablation 1: Q15 vs Soft-Float (2.0 s @ 250 Hz)
    samples_250_2s = _generate_ecg_adc_samples(fs_hz=250, duration_s=2.0, hr_bpm=72.0)
    res_q15 = _run_hil_stream(elf_path, samples_250_2s, fs_hz=250, soft_float=False, interp_enable=True)
    res_flt = _run_hil_stream(elf_path, samples_250_2s, fs_hz=250, soft_float=True, interp_enable=True)
    q15_cyc = float(res_q15["mean_cycles"])
    flt_cyc = float(res_flt["mean_cycles"])
    speedup_x = flt_cyc / max(1.0, q15_cyc)
    ab1_ok = q15_cyc > 0.0 and flt_cyc > q15_cyc

    # Ablation 2: 250 Hz vs 500 Hz (3.0 s @ 72 bpm -> GT RR = 833,333 us)
    gt_rr_us = 60_000_000.0 / 72.0
    samples_250_3s = _generate_ecg_adc_samples(fs_hz=250, duration_s=3.0, hr_bpm=72.0)
    samples_500_3s = _generate_ecg_adc_samples(fs_hz=500, duration_s=3.0, hr_bpm=72.0)
    res_250 = _run_hil_stream(elf_path, samples_250_3s, fs_hz=250, soft_float=False, interp_enable=True)
    res_500 = _run_hil_stream(elf_path, samples_500_3s, fs_hz=500, soft_float=False, interp_enable=True)
    rr_250 = res_250["rr_intervals_us"]
    rr_500 = res_500["rr_intervals_us"]
    mae_250_us = sum(abs(rr - gt_rr_us) for rr in rr_250) / len(rr_250) if rr_250 else 999999.0
    mae_500_us = sum(abs(rr - gt_rr_us) for rr in rr_500) / len(rr_500) if rr_500 else 999999.0
    ab2_ok = len(rr_250) >= 2 and len(rr_500) >= 2 and mae_250_us <= 50_000.0 and mae_500_us <= 50_000.0

    # Ablation 3: J_RLD ACTIVE_RLD vs VREF_GND (50 Hz CMRR on real WearableRun ELF)
    board = P05VirtualBoard.from_hardware(initial_soc_pct=85.0, seed=42)
    backend = UnicornBackend()
    board.attach_firmware(backend, elf_path)
    board.patient.qrs_mv = 0.0
    board.patient.emg_uv_rms = 0.0
    board.patient.baseline_mv = 0.0
    board.patient.cm_50hz_mv = 200.0
    board.afe.input_noise_uvpp = 0.0

    def measure_50hz_rms(j_rld_mode: str) -> float:
        board.set_j_rld(j_rld_mode)
        vals: List[float] = []
        for i in range(350):
            s = board.step(4000.0)
            if i >= 150:
                vals.append(s.ecg_out_v - board.matrix.rails["VREF_1V65"])
        return math.sqrt(sum(v * v for v in vals) / len(vals))

    rms_active = measure_50hz_rms("ACTIVE_RLD")
    rms_vref = measure_50hz_rms("VREF_GND")
    cmrr_gain_db = 20.0 * math.log10(rms_vref / max(1e-9, rms_active))
    ab3_ok = cmrr_gain_db > 35.0

    report: Dict[str, Any] = {
        "firmware": elf_path.name,
        "ab1_q15_mean_cycles": round(q15_cyc, 1),
        "ab1_float_mean_cycles": round(flt_cyc, 1),
        "ab1_speedup_x": round(speedup_x, 2),
        "ab1_passed": ab1_ok,
        "ab2_250hz_rr_count": len(rr_250),
        "ab2_250hz_mae_rr_us": round(mae_250_us, 1),
        "ab2_500hz_rr_count": len(rr_500),
        "ab2_500hz_mae_rr_us": round(mae_500_us, 1),
        "ab2_passed": ab2_ok,
        "ab3_rms_active_rld_v": round(rms_active, 6),
        "ab3_rms_vref_gnd_v": round(rms_vref, 6),
        "ab3_cmrr_gain_db": round(cmrr_gain_db, 2),
        "ab3_passed": ab3_ok,
        "all_passed": bool(ab1_ok and ab2_ok and ab3_ok),
    }

    if output_csv is not None:
        output_csv.parent.mkdir(parents=True, exist_ok=True)
        rows = [
            ("ablation1_q15_vs_float", "q15_mean_cycles", report["ab1_q15_mean_cycles"], "cycles", ab1_ok),
            ("ablation1_q15_vs_float", "float_mean_cycles", report["ab1_float_mean_cycles"], "cycles", ab1_ok),
            ("ablation1_q15_vs_float", "speedup", report["ab1_speedup_x"], "x", ab1_ok),
            ("ablation2_250_vs_500hz", "mae_rr_250hz", report["ab2_250hz_mae_rr_us"], "us", ab2_ok),
            ("ablation2_250_vs_500hz", "mae_rr_500hz", report["ab2_500hz_mae_rr_us"], "us", ab2_ok),
            ("ablation3_j_rld", "rms_active_rld", report["ab3_rms_active_rld_v"], "V", ab3_ok),
            ("ablation3_j_rld", "rms_vref_gnd", report["ab3_rms_vref_gnd_v"], "V", ab3_ok),
            ("ablation3_j_rld", "cmrr_gain", report["ab3_cmrr_gain_db"], "dB", ab3_ok),
        ]
        with output_csv.open("w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["ablation", "metric", "value", "unit", "passed"])
            w.writerows(rows)

    return report


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Gate G4 Ablation 1/2/3 on P05_ECG_Firmware.elf.")
    parser.add_argument("elf", nargs="?", type=Path, default=None, help="Path to P05_ECG_Firmware.elf")
    parser.add_argument(
        "--csv",
        type=Path,
        default=Path("data/test/sim_ablation_report.csv"),
        help="Output CSV path (default: data/test/sim_ablation_report.csv)",
    )
    args = parser.parse_args()

    elf_path = args.elf if args.elf is not None else find_default_elf()
    if elf_path is None or not elf_path.is_file():
        print("[ablation_suite] ERROR: P05_ECG_Firmware.elf not found. Pass path as arg or set P05_ELF_PATH.")
        return 2

    res = run_ablation_suite(elf_path, output_csv=args.csv)
    print("=== P05-Sim Gate G4 Ablation Report ===")
    for k, v in res.items():
        print(f"  {k}: {v}")
    print(f"  csv_saved: {args.csv}")
    return 0 if res["all_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
