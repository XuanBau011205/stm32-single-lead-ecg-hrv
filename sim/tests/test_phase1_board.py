"""Unit tests for P05-Sim Phase 1: Board Bundle & Physical Hardware Plant."""

from __future__ import annotations

import math
import struct
import tempfile
import unittest
from pathlib import Path

from sim import P05VirtualBoard
from sim.core import GpioMode
from sim.mcu import McuPowerMode


class TestP05Phase1VirtualBoard(unittest.TestCase):
    def setUp(self) -> None:
        self.board = P05VirtualBoard.from_hardware(rebuild_manifest=False, initial_soc_pct=80.0, seed=123)

    def test_01_manifest_matches_verified_altium_hardware(self) -> None:
        m = self.board.manifest
        b = m["board"]
        p = m["circuit_parameters"]
        self.assertAlmostEqual(b["width_mm"], 80.0, places=2)
        self.assertAlmostEqual(b["height_mm"], 55.0, places=2)
        self.assertEqual(b["component_count"], 91)
        self.assertEqual(b["unique_bom_lines"], 43)
        self.assertEqual(b["drills"]["total"], 196)
        self.assertEqual(b["drc"]["violations"], 0)
        self.assertAlmostEqual(p["power"]["r_shunt_ohm"], 0.1, places=4)
        self.assertAlmostEqual(p["power"]["ina219_current_resolution_ua"], 100.0, places=1)
        self.assertAlmostEqual(p["afe_ad8232"]["total_gain"], 1100.0, places=1)
        self.assertAlmostEqual(p["afe_ad8232"]["hpf_fc_hz"], 0.482, places=2)

    def test_02_stop_mode_power_budget_exact_32_1_ua(self) -> None:
        # Put INA219 into Power-Down (Reg 0x00 = 0x0000), AFE SDN=LOW, SD Off, BatDiv Off, MCU STOP
        self.assertTrue(self.board.matrix.i2c1_write(0x40, bytes([0x00, 0x00, 0x00])))
        self.board.set_mcu_mode(McuPowerMode.STOP)
        self.board.matrix.digital["ECG_SDN"] = False
        self.board.matrix.digital["SD_PWR_EN"] = True
        self.board.matrix.digital["BAT_SENSE_EN"] = False
        s = self.board.step(4000.0)
        self.assertAlmostEqual(s.i_total_ua, 32.1, places=1)
        self.assertLessEqual(s.i_total_ua, 48.15)  # Gate G4 KPI <= 1.5 * 32.1 uA

        # Verify GPIO floating leakage penalty if firmware forgets to set unused pins to Analog!
        self.board.matrix.gpio_modes["PA5"] = GpioMode.INPUT_FLOATING
        self.board.matrix.gpio_modes["PA6"] = GpioMode.INPUT_FLOATING
        s_leak = self.board.step(4000.0)
        self.assertAlmostEqual(s_leak.i_total_ua, 42.1, places=1)

    def test_03_ina219_i2c_registers_and_100ua_quantization(self) -> None:
        self.board.set_mcu_mode(McuPowerMode.RUN_8MHZ)
        self.board.matrix.digital["ECG_SDN"] = True
        self.board.step(4000.0)

        # Read Bus Voltage register (0x02) -> ~3300 mV
        self.assertTrue(self.board.matrix.i2c1_write(0x40, bytes([0x02])))
        ok, raw = self.board.matrix.i2c1_read(0x40, 2)
        self.assertTrue(ok)
        bus_mv = (struct.unpack(">H", raw)[0] >> 3) * 4
        self.assertGreaterEqual(bus_mv, 3280)
        self.assertLessEqual(bus_mv, 3310)

        # Read Shunt Voltage register (0x01) -> 10 uV/LSB on 0.1 Ohm => multiple of 100 uA
        self.assertTrue(self.board.matrix.i2c1_write(0x40, bytes([0x01])))
        ok_s, raw_s = self.board.matrix.i2c1_read(0x40, 2)
        self.assertTrue(ok_s)
        shunt_counts = struct.unpack(">h", raw_s)[0]
        meas_ua = shunt_counts * 100.0
        # MCU 4200 + AFE 170 + INA219 700 = 5070 uA -> quantized to 5100 uA!
        self.assertEqual(meas_ua, 5100.0)

        # Wrong I2C address must NACK
        self.assertFalse(self.board.matrix.i2c1_write(0x41, bytes([0x00])))

    def test_04_microsd_ao3401_power_gating_and_512b_rw(self) -> None:
        # With PB0=HIGH (AO3401 OFF), SD must not respond and must draw 0 uA
        self.board.matrix.digital["SD_PWR_EN"] = True
        self.board.matrix.digital["SD_CS"] = False
        self.board.step(2000.0)
        self.assertEqual(self.board.matrix.spi1_transfer_byte(0x40), 0xFF)
        self.assertEqual(self.board.sdcard.current_ua, 0.0)

        # Power ON via AO3401 (PB0=LOW) and charge C_SD1
        self.board.matrix.digital["SD_PWR_EN"] = False
        for _ in range(5):
            self.board.step(500.0)
        self.assertGreater(self.board.matrix.rails["3V3_SD"], 3.2)

        def send_cmd(cmd: int, arg: int) -> int:
            for b in [0x40 | cmd, (arg >> 24) & 0xFF, (arg >> 16) & 0xFF, (arg >> 8) & 0xFF, arg & 0xFF, 0x95]:
                self.board.matrix.spi1_transfer_byte(b)
            for _ in range(8):
                r = self.board.matrix.spi1_transfer_byte(0xFF)
                if r != 0xFF:
                    return r
            return 0xFF

        self.assertEqual(send_cmd(0, 0), 0x01)
        self.assertEqual(send_cmd(8, 0x1AA), 0x01)
        for _ in range(4):
            self.board.matrix.spi1_transfer_byte(0xFF)
        self.assertIn(send_cmd(55, 0), (0x00, 0x01))
        self.assertEqual(send_cmd(41, 0x40000000), 0x00)
        self.assertTrue(self.board.sdcard.is_initialized)

        # Write 512-byte sector 7 via CMD24
        self.assertEqual(send_cmd(24, 7), 0x00)
        block = b"ECG5" + bytes([(i * 7) & 0xFF for i in range(508)])
        self.board.matrix.spi1_transfer_byte(0xFE)
        for bv in block:
            self.board.matrix.spi1_transfer_byte(bv)
        self.board.matrix.spi1_transfer_byte(0xFF)
        self.assertEqual(self.board.matrix.spi1_transfer_byte(0xFF), 0x05)

        # Verify flash busy current spike (30 mA) and MISO=0x00 while busy
        s_busy = self.board.step(1000.0)
        self.assertEqual(s_busy.i_sd_ua, 30000.0)
        self.assertEqual(self.board.matrix.spi1_transfer_byte(0xFF), 0x00)

        # Wait out busy timer -> read back via CMD17
        self.board.step(5000.0)
        self.assertEqual(send_cmd(17, 7), 0x00)
        self.assertEqual(self.board.matrix.spi1_transfer_byte(0xFF), 0xFE)
        rx = bytes(self.board.matrix.spi1_transfer_byte(0xFF) for _ in range(512))
        self.board.matrix.spi1_transfer_byte(0xFF)
        self.board.matrix.spi1_transfer_byte(0xFF)
        self.assertEqual(rx, block)

        # Cut AO3401 power -> card drops to 0 uA and loses initialized state
        self.board.matrix.digital["SD_PWR_EN"] = True
        for _ in range(5):
            self.board.step(500.0)
        self.assertFalse(self.board.sdcard.is_initialized)
        self.assertEqual(self.board.sdcard.current_ua, 0.0)

    def test_05_afe_j_rld_ablation_and_leads_off(self) -> None:
        self.board.set_mcu_mode(McuPowerMode.RUN_8MHZ)
        self.board.matrix.digital["ECG_SDN"] = True
        self.board.patient.qrs_mv = 0.0
        self.board.patient.emg_uv_rms = 0.0
        self.board.patient.baseline_mv = 0.0
        self.board.patient.cm_50hz_mv = 200.0
        self.board.afe.input_noise_uvpp = 0.0

        def measure_rms(mode: str) -> float:
            self.board.set_j_rld(mode)
            vals = []
            for i in range(350):
                s = self.board.step(4000.0)
                if i >= 150:
                    vals.append(s.ecg_out_v - self.board.matrix.rails["VREF_1V65"])
            return math.sqrt(sum(v * v for v in vals) / len(vals))

        rms_active = measure_rms("ACTIVE_RLD")
        rms_vref = measure_rms("VREF_GND")
        cmrr_gain_db = 20.0 * math.log10(rms_vref / max(1e-9, rms_active))
        self.assertGreater(cmrr_gain_db, 35.0)  # Expected ~38 dB (80 dB vs 42 dB)

        # Detach RA -> ECG_LOD_N = HIGH; detach LA -> ECG_LOD_P = HIGH
        self.board.set_electrodes(ra=False, la=True, rl=True)
        s1 = self.board.step(4000.0)
        self.assertTrue(s1.lod_n)
        self.assertFalse(s1.lod_p)

        self.board.set_electrodes(ra=True, la=False, rl=True)
        s2 = self.board.step(4000.0)
        self.assertFalse(s2.lod_n)
        self.assertTrue(s2.lod_p)

    def test_06_battery_divider_charging_and_csv_export(self) -> None:
        # 2N7002 switched battery divider
        self.board.matrix.digital["BAT_SENSE_EN"] = True
        self.board.step(4000.0)
        self.assertAlmostEqual(self.board.matrix.analog["BAT_SENSE"] * 2.0, self.board.matrix.rails["VBAT"], places=2)
        self.board.matrix.digital["BAT_SENSE_EN"] = False
        self.board.step(4000.0)
        self.assertEqual(self.board.power.last_breakdown.i_bat_div_ua, 0.0)

        # USB-C charging turns on LED_STAT and raises SoC
        soc_before = self.board.power.soc_pct
        self.board.set_usb_connected(True)
        s_chg = self.board.step(1_000_000.0)  # 1 second of charging
        self.assertTrue(s_chg.led_stat)
        self.assertGreater(self.board.power.soc_pct, soc_before)

        # CSV export
        with tempfile.TemporaryDirectory() as td:
            out_csv = Path(td) / "trace.csv"
            rows = self.board.telemetry.export_csv(out_csv)
            self.assertGreaterEqual(rows, 3)
            self.assertTrue(out_csv.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
