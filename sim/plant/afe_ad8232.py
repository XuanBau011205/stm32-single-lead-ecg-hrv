"""
AFE_AD8232Model — Physical analog front-end model parameterized directly from P05 BOM.

Implements:
  - 2nd-order High-Pass Filter (~0.5 Hz from C_HP1/2=0.33uF, R_HP1/2=10M)
  - 2nd-order Low-Pass Filter (~40 Hz from R_LP1/2=1M, C_LP1=1.5nF, C_LP2=10nF)
  - Total Passband Gain = 1100x (IA=100x, OpAmp=1 + R_G1/R_G2 = 11x)
  - VREF = VDDA / 2 = 1.65 V output mid-supply bias + soft rail clipping
  - J_RLD 3-pin jumper CMRR switching:
      * ACTIVE_RLD (80 dB CMRR) vs VREF_GND (42 dB CMRR) -> Ablation 3!
  - PA3 (ECG_SDN) Shutdown control (170 uA Active -> 0.5 uA Shutdown)
  - PA1/PA2 (ECG_LOD_P / ECG_LOD_N) DC Leads-Off detection when RA or LA detaches
"""

from __future__ import annotations

import math
import random
from dataclasses import dataclass
from typing import Any, Dict

from sim.core.pin_matrix import RldJumperMode, VirtualPinMatrix
from sim.plant.patient_ecg import PatientSignalSample


class _BiquadSection:
    """Direct Form II Transposed 2nd-order IIR biquad section (pure Python)."""

    def __init__(self, b0: float, b1: float, b2: float, a1: float, a2: float) -> None:
        self.b0, self.b1, self.b2 = b0, b1, b2
        self.a1, self.a2 = a1, a2
        self.z1 = 0.0
        self.z2 = 0.0

    def reset(self) -> None:
        self.z1 = 0.0
        self.z2 = 0.0

    def process(self, x: float) -> float:
        y = self.b0 * x + self.z1
        self.z1 = self.b1 * x - self.a1 * y + self.z2
        self.z2 = self.b2 * x - self.a2 * y
        return y


def _design_butterworth_2nd(fc_hz: float, fs_hz: float, highpass: bool) -> _BiquadSection:
    """Bilinear-transform 2nd-order Butterworth HPF or LPF (Q = 1/sqrt(2))."""
    fc = max(0.01, min(fc_hz, 0.45 * fs_hz))
    k = math.tan(math.pi * fc / fs_hz)
    q = math.sqrt(0.5)
    norm = 1.0 / (1.0 + k / q + k * k)
    if highpass:
        b0 = norm
        b1 = -2.0 * norm
        b2 = norm
    else:
        b0 = k * k * norm
        b1 = 2.0 * b0
        b2 = b0
    a1 = 2.0 * (k * k - 1.0) * norm
    a2 = (1.0 - k / q + k * k) * norm
    return _BiquadSection(b0, b1, b2, a1, a2)


@dataclass
class AfeStatus:
    active: bool
    current_ua: float
    ecg_out_v: float
    lod_p: bool
    lod_n: bool
    cmrr_db: float


class AFE_AD8232Model:
    """Physics-based AD8232 front-end wired to VirtualPinMatrix."""

    def __init__(self, afe_params: Dict[str, Any], seed: int = 105) -> None:
        self.total_gain: float = float(afe_params.get("total_gain", 1100.0))
        self.hpf_fc_hz: float = float(afe_params.get("hpf_fc_hz", 0.482))
        self.lpf_fc_hz: float = float(afe_params.get("lpf_fc_hz", 41.09))
        self.vref_ratio: float = float(afe_params.get("vref_ratio", 0.5))
        self.iq_active_ua: float = float(afe_params.get("iq_active_ua", 170.0))
        self.iq_shutdown_ua: float = float(afe_params.get("iq_shutdown_ua", 0.5))
        self.input_noise_uvpp: float = float(afe_params.get("input_noise_uvpp", 14.0))
        self.cmrr_active_rld_db: float = float(afe_params.get("cmrr_active_rld_db", 80.0))
        self.cmrr_vref_gnd_db: float = float(afe_params.get("cmrr_vref_gnd_db", 42.0))

        self._rng = random.Random(seed)
        self._fs_hz: float = 0.0
        self._hpf: _BiquadSection = _design_butterworth_2nd(self.hpf_fc_hz, 250.0, highpass=True)
        self._lpf: _BiquadSection = _design_butterworth_2nd(self.lpf_fc_hz, 250.0, highpass=False)
        self._was_active: bool = False
        self.current_ua: float = self.iq_shutdown_ua

    def reset(self) -> None:
        self._hpf.reset()
        self._lpf.reset()
        self._was_active = False
        self.current_ua = self.iq_shutdown_ua

    def _ensure_sample_rate(self, fs_hz: float) -> None:
        if abs(fs_hz - self._fs_hz) > 1e-3:
            self._fs_hz = fs_hz
            self._hpf = _design_butterworth_2nd(self.hpf_fc_hz, fs_hz, highpass=True)
            self._lpf = _design_butterworth_2nd(self.lpf_fc_hz, fs_hz, highpass=False)

    def update(self, matrix: VirtualPinMatrix, sig: PatientSignalSample, dt_s: float) -> AfeStatus:
        """Advance AD8232 analog state and write ECG_OUT, VREF_1V65, ECG_LOD_P/N onto matrix."""
        vdda = matrix.rails["VDDA"]
        vref = vdda * self.vref_ratio
        matrix.rails["VREF_1V65"] = vref

        # Active only when VDDA powered (>= 2.0V) AND PA3 (ECG_SDN) is HIGH
        active = (vdda >= 2.0) and bool(matrix.digital["ECG_SDN"])
        if not active:
            if self._was_active:
                self._hpf.reset()
                self._lpf.reset()
            self._was_active = False
            self.current_ua = self.iq_shutdown_ua if vdda >= 1.5 else 0.0
            matrix.analog["ECG_OUT"] = 0.0
            matrix.digital["ECG_LOD_P"] = False
            matrix.digital["ECG_LOD_N"] = False
            return AfeStatus(False, self.current_ua, 0.0, False, False, 0.0)

        self._was_active = True
        self.current_ua = self.iq_active_ua
        self._ensure_sample_rate(1.0 / dt_s)

        # DC Leads-Off Detection (R_LO_P/N = 10M pull-up to VDDA)
        ra_ok = bool(matrix.electrodes.get("RA", True))
        la_ok = bool(matrix.electrodes.get("LA", True))
        rl_ok = bool(matrix.electrodes.get("RL", True))
        lod_p = not la_ok  # IN+ connected to LA
        lod_n = not ra_ok  # IN- connected to RA
        matrix.digital["ECG_LOD_P"] = lod_p
        matrix.digital["ECG_LOD_N"] = lod_n

        # Effective CMRR depends on physical J_RLD jumper and RL electrode contact!
        if matrix.j_rld_mode == RldJumperMode.ACTIVE_RLD and rl_ok:
            cmrr_db = self.cmrr_active_rld_db
        elif rl_ok:
            cmrr_db = self.cmrr_vref_gnd_db
        else:
            # RL electrode detached -> severe common-mode pickup (~26 dB)
            cmrr_db = 26.0

        if lod_p or lod_n:
            # Open electrode rails output toward supply or ground
            v_out = (vdda - 0.05) if lod_p and not lod_n else 0.05
            matrix.analog["ECG_OUT"] = v_out
            return AfeStatus(True, self.current_ua, v_out, lod_p, lod_n, cmrr_db)

        # Input-referred common-mode 50 Hz leakage = V_cm / 10^(CMRR/20)
        cmrr_lin = 10.0 ** (cmrr_db / 20.0)
        v_50hz_leaked_mv = sig.v_cm_50hz_mv / cmrr_lin

        # Intrinsic AD8232 thermal/1/f noise (~14 uVpp -> ~2.1 uVrms, 6.6*sigma ~ Vpp)
        v_noise_mv = self._rng.gauss(0.0, (self.input_noise_uvpp / 6.6) * 1e-3)

        # Total differential voltage in Volts at IN+/IN-
        v_in_v = (sig.v_diff_mv + sig.v_baseline_mv + sig.v_emg_mv + v_50hz_leaked_mv + v_noise_mv) * 1e-3

        # Pass through 2-pole HPF (0.5 Hz) -> Gain (1100x) -> 2-pole LPF (41 Hz)
        v_hp = self._hpf.process(v_in_v)
        v_lp = self._lpf.process(v_hp * self.total_gain)
        v_out = max(0.05, min(vdda - 0.05, vref + v_lp))

        matrix.analog["ECG_OUT"] = v_out
        return AfeStatus(True, self.current_ua, v_out, lod_p, lod_n, cmrr_db)
