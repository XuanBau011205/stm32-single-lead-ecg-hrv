"""
INA219Device — Register-accurate I2C1 slave model (address 0x40, R_SHUNT = 0.1 Ohm).

Matches TI INA219 datasheet:
  - Reg 0x00: Configuration (reset 0x399F; bits [2:0] == 0b000 -> Power-Down 6.0 uA)
  - Reg 0x01: Shunt Voltage (signed 16-bit, LSB = 10 uV -> 100 uA resolution on 0.1 Ohm!)
  - Reg 0x02: Bus Voltage   (bits [15:3] = V_bus / 4 mV, bit 1 = CNVR)
  - Reg 0x03: Power         (unsigned 16-bit)
  - Reg 0x04: Current       (signed 16-bit = (Shunt_Reg * Cal_Reg) // 4096)
  - Reg 0x05: Calibration   (16-bit, bit 0 forced 0 per datasheet)
"""

from __future__ import annotations

import struct
from typing import Any, Dict, Tuple


class INA219Device:
    """I2C1 slave model of U_INA (INA219AIDR @ 0x40) on the P05 PCB."""

    REG_CONFIG = 0x00
    REG_SHUNT_V = 0x01
    REG_BUS_V = 0x02
    REG_POWER = 0x03
    REG_CURRENT = 0x04
    REG_CALIBRATION = 0x05

    def __init__(self, power_params: Dict[str, Any]) -> None:
        self.i2c_addr: int = int(power_params.get("ina219_i2c_addr", 0x40))
        self.r_shunt_ohm: float = float(power_params.get("r_shunt_ohm", 0.1))
        self.shunt_lsb_v: float = float(power_params.get("ina219_shunt_lsb_uv", 10.0)) * 1e-6
        self.iq_active_ua: float = float(power_params.get("ina219_iq_active_ua", 700.0))
        self.iq_powerdown_ua: float = float(power_params.get("ina219_iq_powerdown_ua", 6.0))

        self.reset()

    def reset(self) -> None:
        self._pointer: int = self.REG_CONFIG
        self._reg_config: int = 0x399F  # Datasheet POR default (Continuous Shunt & Bus)
        self._reg_cal: int = 0x0000
        self._bus_v: float = 3.3
        self._load_current_a: float = 0.0

    @property
    def is_power_down(self) -> bool:
        return (self._reg_config & 0x0007) == 0

    @property
    def current_ua(self) -> float:
        if self._bus_v < 2.7:
            return 0.0
        return self.iq_powerdown_ua if self.is_power_down else self.iq_active_ua

    def update_measurements(self, bus_v: float, load_current_ua: float) -> None:
        """Feed true rail voltage and downstream load current through R_SHUNT."""
        self._bus_v = max(0.0, float(bus_v))
        self._load_current_a = float(load_current_ua) * 1e-6

    def _shunt_reg_signed(self) -> int:
        # V_shunt = I_load * R_shunt; quantized to 10 uV LSB (= 100 uA per LSB on 0.1 Ohm!)
        v_shunt = self._load_current_a * self.r_shunt_ohm
        counts = int(round(v_shunt / self.shunt_lsb_v))
        return max(-32768, min(32767, counts))

    def _bus_reg(self) -> int:
        # 4 mV per LSB in bits [15:3], CNVR (bit 1) = 1
        counts = max(0, min(8191, int(round(self._bus_v / 0.004))))
        return ((counts << 3) | 0x0002) & 0xFFFF

    def _current_reg_signed(self) -> int:
        if self._reg_cal == 0:
            return 0
        val = int((self._shunt_reg_signed() * self._reg_cal) / 4096)
        return max(-32768, min(32767, val))

    def _power_reg(self) -> int:
        if self._reg_cal == 0:
            return 0
        bus_counts = (self._bus_reg() >> 3) & 0x1FFF
        val = int((abs(self._current_reg_signed()) * bus_counts) / 5000)
        return max(0, min(65535, val))

    def read_quantized_current_ua(self) -> float:
        """Return INA219's quantized current reading (reflecting real 100 uA shunt LSB)."""
        if self.is_power_down:
            return 0.0
        v_shunt_quant = self._shunt_reg_signed() * self.shunt_lsb_v
        return (v_shunt_quant / self.r_shunt_ohm) * 1e6

    # --- I2C1 Slave Callbacks ---
    def i2c_write(self, addr_7bit: int, data: bytes) -> bool:
        if addr_7bit != self.i2c_addr or not data:
            return False
        reg = data[0]
        if reg > self.REG_CALIBRATION:
            return False
        self._pointer = reg
        if len(data) >= 3:
            val = struct.unpack(">H", data[1:3])[0]
            if reg == self.REG_CONFIG:
                if val & 0x8000:
                    self.reset()
                else:
                    self._reg_config = val & 0x7FFF
            elif reg == self.REG_CALIBRATION:
                self._reg_cal = val & 0xFFFE  # Bit 0 is always 0 per datasheet
        return True

    def i2c_read(self, addr_7bit: int, length: int) -> Tuple[bool, bytes]:
        if addr_7bit != self.i2c_addr or length <= 0:
            return False, b""
        if self._pointer == self.REG_CONFIG:
            raw = self._reg_config
        elif self._pointer == self.REG_SHUNT_V:
            raw = self._shunt_reg_signed() & 0xFFFF
        elif self._pointer == self.REG_BUS_V:
            raw = self._bus_reg()
        elif self._pointer == self.REG_POWER:
            raw = self._power_reg()
        elif self._pointer == self.REG_CURRENT:
            raw = self._current_reg_signed() & 0xFFFF
        elif self._pointer == self.REG_CALIBRATION:
            raw = self._reg_cal
        else:
            raw = 0
        payload = struct.pack(">H", raw)
        return True, payload[:length]
