"""
PowerTreeModel — Physical power supply, battery, charger, MOSFET switches, and Virtual PPK2.

Models the exact P05 power chain from hardware/bom/P05_ECG_BOM.xlsx & docs/SPECIFICATION.md §4:
  [J_USB 5V] -> [MCP73831 (R_PROG=4.7k -> 212.8 mA) + LED_STAT] -> [Li-ion 1S 500 mAh]
       -> [MCP1700-3.3V (Iq=1.6 uA)] -> [J_IMEAS (Virtual PPK2)] -> [R_SHUNT=0.1R + INA219]
       -> Rail 3V3_MAIN & VDDA (via FB1)
       -> AO3401 P-MOSFET (PB0 active-low) -> Rail 3V3_SD (microSD)
       -> 2N7002 N-MOSFET (PB5 active-high) -> R_DIV1/R_DIV2 (100k/100k) -> BAT_SENSE (PB1)
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict

from sim.core.pin_matrix import VirtualPinMatrix
from sim.mcu.backend_base import McuPowerMode


@dataclass
class PowerBreakdown:
    vbus_v: float
    vbat_v: float
    soc_pct: float
    charging: bool
    i_charge_ma: float
    led_stat_on: bool
    rail_3v3_v: float
    rail_3v3_sd_v: float
    i_mcu_ua: float
    i_ldo_ua: float
    i_afe_ua: float
    i_ina219_ua: float
    i_sd_ua: float
    i_bat_div_ua: float
    i_gpio_leak_ua: float
    i_total_ua: float           # Virtual Nordic PPK2 reading at J_IMEAS (uA)
    est_battery_hours: float


class PowerTreeModel:
    """Complete electrical power model & Virtual Nordic PPK2 current aggregator."""

    def __init__(self, power_params: Dict[str, Any], mcu_params: Dict[str, Any], initial_soc_pct: float = 85.0) -> None:
        self.vbus_nominal_v: float = float(power_params.get("vbus_nominal_v", 5.0))
        self.capacity_mah: float = float(power_params.get("battery_capacity_mah", 500.0))
        self.r_int_ohm: float = float(power_params.get("battery_r_int_ohm", 0.12))
        self.ichg_max_ma: float = float(power_params.get("mcp73831_ichg_ma", 212.77))
        self.ldo_vout_v: float = float(power_params.get("ldo_vout_v", 3.3))
        self.ldo_dropout_v: float = float(power_params.get("ldo_dropout_v", 0.178))
        self.ldo_iq_ua: float = float(power_params.get("ldo_iq_ua", 1.6))
        self.r_div1_ohm: float = float(power_params.get("r_div1_ohm", 100e3))
        self.r_div2_ohm: float = float(power_params.get("r_div2_ohm", 100e3))
        self.bat_div_ratio: float = self.r_div2_ohm / (self.r_div1_ohm + self.r_div2_ohm)

        # MCU current lookup from SPECIFICATION.md §4
        self._mcu_currents_ua: Dict[McuPowerMode, float] = {
            McuPowerMode.OFF: 0.0,
            McuPowerMode.STOP: float(mcu_params.get("i_stop_ua", 24.0)),
            McuPowerMode.SLEEP_8MHZ: float(mcu_params.get("i_sleep_8mhz_ma", 1.8)) * 1000.0,
            McuPowerMode.RUN_8MHZ: float(mcu_params.get("i_run_8mhz_ma", 4.2)) * 1000.0,
            McuPowerMode.RUN_72MHZ: float(mcu_params.get("i_run_72mhz_ma", 27.0)) * 1000.0,
        }

        self.soc_pct: float = max(0.0, min(100.0, float(initial_soc_pct)))
        self._sd_cap_v: float = 0.0  # Soft-start voltage on C_SD1 (4.7 uF)
        self.last_breakdown: PowerBreakdown = self._zero_breakdown()

    def _zero_breakdown(self) -> PowerBreakdown:
        return PowerBreakdown(0.0, 3.85, self.soc_pct, False, 0.0, False, 3.3, 0.0,
                              0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0)

    def set_battery_soc(self, soc_pct: float) -> None:
        self.soc_pct = max(0.0, min(100.0, float(soc_pct)))

    @staticmethod
    def _ocv_from_soc(soc_pct: float) -> float:
        """Piecewise-linear realistic 1S Li-ion open-circuit voltage curve (3.00 V -> 4.20 V)."""
        s = max(0.0, min(100.0, soc_pct))
        pts = [(0.0, 3.00), (10.0, 3.45), (25.0, 3.68), (50.0, 3.80), (80.0, 4.00), (100.0, 4.20)]
        for i in range(len(pts) - 1):
            s0, v0 = pts[i]
            s1, v1 = pts[i + 1]
            if s <= s1:
                t = (s - s0) / (s1 - s0)
                return v0 + t * (v1 - v0)
        return 4.20

    def update(
        self,
        matrix: VirtualPinMatrix,
        mcu_mode: McuPowerMode,
        i_afe_ua: float,
        i_ina219_ua: float,
        i_sd_ua: float,
        dt_s: float,
    ) -> PowerBreakdown:
        """Advance battery, charger, LDO, MOSFET rails, and PPK2 power breakdown."""
        vbus = self.vbus_nominal_v if matrix.usb_connected else 0.0
        matrix.rails["VBUS"] = vbus

        # 1. MCP73831 Li-ion charger (CC/CV) + LED_STAT
        ocv = self._ocv_from_soc(self.soc_pct)
        charging = False
        i_charge_ma = 0.0
        if vbus >= 4.35 and self.soc_pct < 99.8:
            charging = True
            if self.soc_pct < 85.0:
                i_charge_ma = self.ichg_max_ma  # Constant Current (~212.8 mA)
            else:
                # Constant Voltage taper down to 10% termination threshold
                frac = max(0.1, (100.0 - self.soc_pct) / 15.0)
                i_charge_ma = self.ichg_max_ma * frac
        led_stat = charging  # MCP73831 STAT pin pulls LED_STAT cathode LOW while charging
        matrix.led_stat_on = led_stat

        # 2. Compute load currents (MCU + GPIO leakage penalty in Stop Mode + AFE + INA219 + SD + BatDiv)
        if not matrix.j_imeas_closed:
            # Jumper J_IMEAS open with no ammeter -> downstream 3V3_MAIN unpowered!
            i_mcu_ua = 0.0
            i_gpio_leak_ua = 0.0
            i_afe_eff = 0.0
            i_ina_eff = 0.0
            i_sd_eff = 0.0
            i_ldo_ua = self.ldo_iq_ua
        else:
            i_mcu_ua = self._mcu_currents_ua.get(mcu_mode, 0.0)
            # In Stop Mode, any unused pin left in digital INPUT_FLOATING leaks ~5 uA (educational + realistic!)
            leaking_pins = matrix.count_non_analog_floating_pins() if mcu_mode == McuPowerMode.STOP else 0
            i_gpio_leak_ua = leaking_pins * 5.0
            i_afe_eff = float(i_afe_ua)
            i_ina_eff = float(i_ina219_ua)
            i_sd_eff = float(i_sd_ua)
            i_ldo_ua = self.ldo_iq_ua

        # 3. Switched Battery Divider (2N7002 controlled by PB5 BAT_SENSE_EN)
        vbat_est = min(4.20, ocv + (i_charge_ma * 1e-3) * self.r_int_ohm)
        if matrix.digital["BAT_SENSE_EN"] and matrix.j_imeas_closed:
            i_bat_div_ua = (vbat_est / (self.r_div1_ohm + self.r_div2_ohm)) * 1e6
            bat_sense_v = vbat_est * self.bat_div_ratio
        else:
            i_bat_div_ua = 0.0
            bat_sense_v = 0.0
        matrix.analog["BAT_SENSE"] = bat_sense_v

        # 4. Total system current measured by Virtual Nordic PPK2 (uA)
        # Note: Total battery/supply draw includes LDO Iq + downstream load + bat divider
        i_downstream_ua = i_mcu_ua + i_gpio_leak_ua + i_afe_eff + i_ina_eff + i_sd_eff
        i_total_ua = i_ldo_ua + i_downstream_ua + i_bat_div_ua

        # 5. Update Battery VBAT & SoC via Coulomb counting
        net_batt_ma = i_charge_ma - (i_total_ua * 1e-3)
        delta_mah = net_batt_ma * (dt_s / 3600.0)
        self.soc_pct = max(0.0, min(100.0, self.soc_pct + (delta_mah / self.capacity_mah) * 100.0))
        vbat = max(2.8, min(4.22, self._ocv_from_soc(self.soc_pct) + (net_batt_ma * 1e-3) * self.r_int_ohm))
        matrix.rails["VBAT"] = vbat

        # 6. MCP1700-3.3V LDO output rails
        v_3v3_pre = max(0.0, min(self.ldo_vout_v, vbat - self.ldo_dropout_v))
        matrix.rails["3V3_PRE"] = v_3v3_pre
        if matrix.j_imeas_closed and v_3v3_pre >= 2.0:
            v_shunt_drop = (i_downstream_ua * 1e-6) * 0.1
            v_main = max(0.0, v_3v3_pre - v_shunt_drop)
        else:
            v_main = 0.0
        matrix.rails["3V3_IN"] = v_3v3_pre if matrix.j_imeas_closed else 0.0
        matrix.rails["3V3_MAIN"] = v_main
        matrix.rails["VDDA"] = v_main

        # 7. AO3401 P-MOSFET for microSD 3V3_SD rail (PB0 SD_PWR_EN is Active-Low!)
        sd_target_v = v_main if (not matrix.digital["SD_PWR_EN"]) else 0.0
        # Soft-start RC tau ~ 0.3 ms for C_SD1 = 4.7 uF
        alpha = min(1.0, dt_s / 0.0003) if dt_s > 0.0 else 1.0
        self._sd_cap_v += alpha * (sd_target_v - self._sd_cap_v)
        if abs(sd_target_v - self._sd_cap_v) < 0.01:
            self._sd_cap_v = sd_target_v
        matrix.rails["3V3_SD"] = self._sd_cap_v

        # 8. Estimated battery runtime (usable 80% factor per SPECIFICATION.md §4B)
        i_ma = max(1e-6, i_total_ua * 1e-3)
        est_hours = (self.capacity_mah * 0.8 * (self.soc_pct / 100.0)) / i_ma

        self.last_breakdown = PowerBreakdown(
            vbus_v=vbus,
            vbat_v=vbat,
            soc_pct=self.soc_pct,
            charging=charging,
            i_charge_ma=i_charge_ma,
            led_stat_on=led_stat,
            rail_3v3_v=v_main,
            rail_3v3_sd_v=self._sd_cap_v,
            i_mcu_ua=i_mcu_ua,
            i_ldo_ua=i_ldo_ua,
            i_afe_ua=i_afe_eff,
            i_ina219_ua=i_ina_eff,
            i_sd_ua=i_sd_eff,
            i_bat_div_ua=i_bat_div_ua,
            i_gpio_leak_ua=i_gpio_leak_ua,
            i_total_ua=i_total_ua,
            est_battery_hours=est_hours,
        )
        return self.last_breakdown
