"""
PatientECGSource — Physiological single-lead ECG & interference generator.

Supports two operating modes:
  1. Synthetic physiological ECG (5-Gaussian P-Q-R-S-T morphology) with:
     - Configurable heart rate (HR bpm) & respiratory sinus arrhythmia (HRV)
     - Exact ground-truth R-peak pulse flag for algorithm validation
     - Controllable 50 Hz mains common-mode interference (for J_RLD Ablation 3!)
     - Controllable EMG muscle noise and 0.25 Hz respiratory baseline wander
  2. External waveform replay (e.g., resampled MIT-BIH Arrhythmia samples in mV)
"""

from __future__ import annotations

import math
import random
from dataclasses import dataclass
from typing import List, Optional, Sequence, Tuple


@dataclass
class PatientSignalSample:
    v_diff_mv: float       # True differential Lead-I/II voltage (RA-LA) in mV
    v_cm_50hz_mv: float    # Common-mode 50 Hz mains pickup on body in mV (attenuated by CMRR / J_RLD)
    v_baseline_mv: float   # Slow respiratory baseline drift in mV
    v_emg_mv: float        # High-frequency muscle artifact in mV
    is_r_peak_gt: bool     # True on the exact step crossing the R-peak apex


# Standard Lead-II P-Q-R-S-T Gaussian components: (phase_rad_offset_from_R, amplitude_rel, width_rad)
_PQRST_WAVES: Tuple[Tuple[float, float, float], ...] = (
    (-1.05,  0.14, 0.22),   # P wave
    (-0.22, -0.15, 0.065),  # Q wave
    ( 0.00,  1.00, 0.055),  # R peak
    ( 0.20, -0.25, 0.065),  # S wave
    ( 1.55,  0.28, 0.34),   # T wave
)


class PatientECGSource:
    """Deterministic physiological ECG source driving the RA/LA/RL electrodes."""

    def __init__(
        self,
        hr_bpm: float = 72.0,
        hrv_sdnn_ms: float = 35.0,
        qrs_mv: float = 1.2,
        cm_50hz_mv: float = 150.0,
        emg_uv_rms: float = 5.0,
        baseline_mv: float = 0.25,
        baseline_hz: float = 0.25,
        seed: int = 42,
    ) -> None:
        self.hr_bpm = float(hr_bpm)
        self.hrv_sdnn_ms = float(hrv_sdnn_ms)
        self.qrs_mv = float(qrs_mv)
        self.cm_50hz_mv = float(cm_50hz_mv)
        self.emg_uv_rms = float(emg_uv_rms)
        self.baseline_mv = float(baseline_mv)
        self.baseline_hz = float(baseline_hz)
        self._rng = random.Random(seed)

        # Phase state in radians: R-peak occurs at phase = 0 (wrapped to [-pi, +pi))
        self._phase: float = -math.pi
        self._prev_phase: float = -math.pi
        self.r_peak_times_s: List[float] = []

        # Optional external waveform buffer (e.g., MIT-BIH record)
        self._ext_samples_mv: Optional[Sequence[float]] = None
        self._ext_fs_hz: float = 250.0

    def load_external_waveform(self, samples_mv: Sequence[float], fs_hz: float = 250.0) -> None:
        """Replay an external ECG waveform (in mV) instead of synthetic PQRST."""
        self._ext_samples_mv = list(samples_mv)
        self._ext_fs_hz = float(fs_hz)

    def clear_external_waveform(self) -> None:
        self._ext_samples_mv = None

    def reset(self, seed: int = 42) -> None:
        self._rng = random.Random(seed)
        self._phase = -math.pi
        self._prev_phase = -math.pi
        self.r_peak_times_s.clear()

    def sample(self, time_s: float, dt_s: float) -> PatientSignalSample:
        """Generate instantaneous body potentials at `time_s` advancing by `dt_s`."""
        is_r_peak = False
        if self._ext_samples_mv is not None and len(self._ext_samples_mv) > 0:
            idx = int(time_s * self._ext_fs_hz) % len(self._ext_samples_mv)
            v_ecg_mv = float(self._ext_samples_mv[idx])
        else:
            # Instantaneous RR interval modulated by respiratory sinus arrhythmia (~0.25 Hz)
            base_rr_s = 60.0 / max(25.0, min(220.0, self.hr_bpm))
            rsa_delta_s = (self.hrv_sdnn_ms * 1e-3) * math.sin(2.0 * math.pi * 0.22 * time_s)
            inst_rr_s = max(0.28, base_rr_s + rsa_delta_s)

            self._prev_phase = self._phase
            self._phase += (2.0 * math.pi * dt_s) / inst_rr_s
            if self._phase >= math.pi:
                self._phase -= 2.0 * math.pi

            # R-peak apex is at phase == 0.0 (crossing from < 0 to >= 0)
            if self._prev_phase < 0.0 <= self._phase and (self._phase - self._prev_phase) < math.pi:
                is_r_peak = True
                self.r_peak_times_s.append(time_s)

            v_norm = 0.0
            for mu, amp, sigma in _PQRST_WAVES:
                # Shortest wrapped angular distance
                d = self._phase - mu
                if d > math.pi:
                    d -= 2.0 * math.pi
                elif d < -math.pi:
                    d += 2.0 * math.pi
                v_norm += amp * math.exp(-0.5 * (d / sigma) ** 2)
            v_ecg_mv = v_norm * self.qrs_mv

        v_cm_50hz = self.cm_50hz_mv * math.sin(2.0 * math.pi * 50.0 * time_s)
        v_baseline = self.baseline_mv * math.sin(2.0 * math.pi * self.baseline_hz * time_s)
        v_emg = (self._rng.gauss(0.0, self.emg_uv_rms) * 1e-3) if self.emg_uv_rms > 0.0 else 0.0

        return PatientSignalSample(
            v_diff_mv=v_ecg_mv,
            v_cm_50hz_mv=v_cm_50hz,
            v_baseline_mv=v_baseline,
            v_emg_mv=v_emg,
            is_r_peak_gt=is_r_peak,
        )
