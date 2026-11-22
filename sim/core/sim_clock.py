"""Deterministic microsecond simulation clock for P05-Sim."""

from __future__ import annotations

import time
from dataclasses import dataclass


@dataclass
class SimClock:
    """Master simulation clock supporting uncapped headless (MAXx) and real-time (1x) pacing."""

    now_us: float = 0.0
    tick_count: int = 0
    realtime_scale: float = 0.0  # 0.0 = uncapped MAXx; 1.0 = real-time 1x
    _wall_anchor: float = 0.0
    _sim_anchor_us: float = 0.0

    @property
    def now_s(self) -> float:
        return self.now_us * 1e-6

    @property
    def now_ms(self) -> float:
        return self.now_us * 1e-3

    def reset(self) -> None:
        self.now_us = 0.0
        self.tick_count = 0
        self._wall_anchor = time.perf_counter()
        self._sim_anchor_us = 0.0

    def set_realtime_scale(self, scale: float) -> None:
        self.realtime_scale = max(0.0, float(scale))
        self._wall_anchor = time.perf_counter()
        self._sim_anchor_us = self.now_us

    def advance(self, dt_us: float) -> None:
        if dt_us <= 0.0:
            raise ValueError(f"dt_us must be positive, got {dt_us}")
        self.now_us += dt_us
        self.tick_count += 1
        if self.realtime_scale > 0.0:
            target_wall_dt = ((self.now_us - self._sim_anchor_us) * 1e-6) / self.realtime_scale
            elapsed_wall = time.perf_counter() - self._wall_anchor
            sleep_s = target_wall_dt - elapsed_wall
            if sleep_s > 0.0005:
                time.sleep(sleep_s)
