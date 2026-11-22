"""Fixed-capacity telemetry ring buffer and CSV exporter for P05-Sim."""

from __future__ import annotations

import csv
from collections import deque
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Deque, List


@dataclass
class TelemetrySample:
    time_s: float
    ecg_patient_mv: float
    ecg_out_v: float
    adc_counts: int
    vbat_v: float
    soc_pct: float
    i_total_ua: float
    i_mcu_ua: float
    i_afe_ua: float
    i_ina219_ua: float
    i_sd_ua: float
    ina219_measured_ua: float
    lod_p: bool
    lod_n: bool
    led_stat: bool
    mcu_mode: str
    r_peak_gt: bool = False


class TelemetryBuffer:
    """Lock-friendly ring buffer feeding both 60 FPS PyQtGraph UI and CSV exports."""

    def __init__(self, max_samples: int = 50000) -> None:
        self._buf: Deque[TelemetrySample] = deque(maxlen=max_samples)

    def append(self, sample: TelemetrySample) -> None:
        self._buf.append(sample)

    def clear(self) -> None:
        self._buf.clear()

    def __len__(self) -> int:
        return len(self._buf)

    def latest(self, count: int = 1000) -> List[TelemetrySample]:
        if count >= len(self._buf):
            return list(self._buf)
        return list(self._buf)[-count:]

    def export_csv(self, path: Path) -> int:
        """Write buffered telemetry samples to a CSV file; returns number of rows written."""
        samples = list(self._buf)
        if not samples:
            return 0
        path.parent.mkdir(parents=True, exist_ok=True)
        fieldnames = list(asdict(samples[0]).keys())
        with path.open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            for s in samples:
                writer.writerow(asdict(s))
        return len(samples)
