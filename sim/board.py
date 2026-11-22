"""
P05VirtualBoard — Unified Python Virtual Hardware Twin for the P05 ECG PCB.

Packages the verified 80.0 x 55.0 mm 2-layer Altium design (91 parts, ERC=0, DRC=0)
into a single executable Python object with an `IFirmwareBackend` socket ready to flash
and run real ARM Cortex-M3 (.elf/.bin) or Fast SIL (.dll) firmware.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, Optional, Tuple

from sim.board_bundle import load_manifest
from sim.core import (
    RldJumperMode,
    SimClock,
    TelemetryBuffer,
    TelemetrySample,
    VirtualPinMatrix,
)
from sim.mcu import (
    CpuSnapshot,
    FirmwareInfo,
    HardwareOnlyBackend,
    IFirmwareBackend,
    McuPowerMode,
)
from sim.plant import (
    AFE_AD8232Model,
    INA219Device,
    PatientECGSource,
    PowerBreakdown,
    PowerTreeModel,
    VirtualMicroSD,
)


class P05VirtualBoard:
    """Complete physical twin of the P05 Single-Lead ECG hardware board."""

    def __init__(
        self,
        manifest: Dict[str, Any],
        initial_soc_pct: float = 85.0,
        sd_backing_img: Optional[Path] = None,
        seed: int = 42,
    ) -> None:
        self.manifest: Dict[str, Any] = manifest
        params = manifest["circuit_parameters"]

        self.clock = SimClock()
        self.matrix = VirtualPinMatrix()
        self.telemetry = TelemetryBuffer()

        # Physical Plant blocks (all parameterized directly from BOM values in board_manifest.json)
        self.patient = PatientECGSource(seed=seed)
        self.afe = AFE_AD8232Model(params["afe_ad8232"], seed=seed + 100)
        self.ina219 = INA219Device(params["power"])
        self.sdcard = VirtualMicroSD(backing_img=sd_backing_img)
        self.power = PowerTreeModel(params["power"], params["mcu_stm32f103c8t6"], initial_soc_pct=initial_soc_pct)

        # Wire physical bus slaves onto the board's VirtualPinMatrix
        self.matrix.register_spi1_slave(self.sdcard.spi_transfer_byte)
        self.matrix.register_i2c1_bus(self._i2c1_write_dispatch, self._i2c1_read_dispatch)

        # Firmware execution socket (default = HardwareOnlyBackend until .elf/.dll is attached)
        self.backend: IFirmwareBackend = HardwareOnlyBackend()
        self.backend.attach_matrix(self.matrix)
        self.firmware_info: Optional[FirmwareInfo] = None

        # Run initial electrical settle pass so rails are valid at t = 0
        self._settle_rails(dt_us=1000.0)

    @classmethod
    def from_hardware(
        cls,
        rebuild_manifest: bool = False,
        initial_soc_pct: float = 85.0,
        sd_backing_img: Optional[Path] = None,
        seed: int = 42,
    ) -> "P05VirtualBoard":
        """Instantiate P05VirtualBoard directly from the Gate G2 Altium hardware artifacts."""
        manifest = load_manifest(rebuild=rebuild_manifest)
        return cls(
            manifest=manifest,
            initial_soc_pct=initial_soc_pct,
            sd_backing_img=sd_backing_img,
            seed=seed,
        )

    # --- Bus Dispatchers (I2C1 has INA219 @ 0x40; extensible to future I2C devices) ---
    def _i2c1_write_dispatch(self, addr_7bit: int, data: bytes) -> bool:
        if addr_7bit == self.ina219.i2c_addr:
            return self.ina219.i2c_write(addr_7bit, data)
        return False

    def _i2c1_read_dispatch(self, addr_7bit: int, length: int) -> Tuple[bool, bytes]:
        if addr_7bit == self.ina219.i2c_addr:
            return self.ina219.i2c_read(addr_7bit, length)
        return False, b""

    # --- Firmware Socket API ---
    def attach_firmware(self, backend: IFirmwareBackend, firmware_path: Optional[Path] = None) -> Optional[FirmwareInfo]:
        """Plug in a firmware backend (Unicorn ARM Cortex-M3 or Fast SIL C-DLL) and optionally flash it."""
        self.backend = backend
        self.backend.attach_matrix(self.matrix)
        if firmware_path is not None:
            self.firmware_info = self.backend.load_firmware(Path(firmware_path))
        self.backend.reset()
        return self.firmware_info

    # --- Interactive Physical Board Controls (match physical switches/jumpers on PCB!) ---
    def press_reset_button(self) -> None:
        """Momentarily press SW_RST (NRST): resets MCU, INA219, AFE filters."""
        self.matrix.set_reset_button(True)
        self.afe.reset()
        self.ina219.reset()
        self.backend.reset()
        self.matrix.set_reset_button(False)

    def set_wake_button(self, pressed: bool) -> None:
        """Press or release SW_WAKE (PB12 / EXTI12)."""
        self.matrix.set_wake_button(pressed)

    def set_j_rld(self, mode: str) -> None:
        """Move 3-pin jumper J_RLD between 'ACTIVE_RLD' (pins 1-2) and 'VREF_GND' (pins 2-3)."""
        self.matrix.j_rld_mode = RldJumperMode(mode)

    def set_j_imeas(self, closed: bool) -> None:
        """Open or close jumper J_IMEAS (Virtual PPK2 ammeter tap)."""
        self.matrix.j_imeas_closed = bool(closed)

    def set_usb_connected(self, connected: bool) -> None:
        """Plug or unplug USB-C 5V charging cable at J_USB."""
        self.matrix.usb_connected = bool(connected)

    def set_sd_inserted(self, inserted: bool) -> None:
        """Insert or eject microSD card in slot J_SD."""
        self.matrix.sd_card_inserted = bool(inserted)

    def set_electrodes(self, ra: bool = True, la: bool = True, rl: bool = True) -> None:
        """Attach or detach individual ECG patient electrodes at J_ECG."""
        self.matrix.electrodes["RA"] = bool(ra)
        self.matrix.electrodes["LA"] = bool(la)
        self.matrix.electrodes["RL"] = bool(rl)

    # --- Low-level Hardware Bring-Up Helpers (useful in HardwareOnlyBackend tests) ---
    def set_mcu_mode(self, mode: McuPowerMode) -> None:
        if isinstance(self.backend, HardwareOnlyBackend):
            self.backend.set_mode(mode)
        else:
            raise RuntimeError("MCU power mode is controlled by flashed firmware when a real backend is attached.")

    def _settle_rails(self, dt_us: float) -> PowerBreakdown:
        cpu = self.backend.get_cpu_snapshot()
        i_sd = self.sdcard.update(self.matrix, dt_us)
        pwr = self.power.update(
            matrix=self.matrix,
            mcu_mode=cpu.power_mode,
            i_afe_ua=self.afe.current_ua,
            i_ina219_ua=self.ina219.current_ua,
            i_sd_ua=i_sd,
            dt_s=dt_us * 1e-6,
        )
        # Update INA219 shunt/bus registers from downstream rail current
        downstream_ua = pwr.i_mcu_ua + pwr.i_gpio_leak_ua + pwr.i_afe_ua + pwr.i_ina219_ua + pwr.i_sd_ua
        self.ina219.update_measurements(bus_v=pwr.rail_3v3_v, load_current_ua=downstream_ua)
        return pwr

    # --- Main Co-Simulation Step ---
    def step(self, dt_us: float = 4000.0, record: bool = True) -> TelemetrySample:
        """
        Advance the entire virtual P05 hardware board by `dt_us` microseconds.
        Default dt_us = 4000 us corresponds to 250 Hz ECG sampling period.
        """
        dt_s = dt_us * 1e-6
        self.clock.advance(dt_us)

        # 1. Sample patient body potentials & advance AD8232 AFE physics
        sig = self.patient.sample(time_s=self.clock.now_s, dt_s=dt_s)
        afe_st = self.afe.update(self.matrix, sig, dt_s=dt_s)

        # 2. Step attached MCU firmware backend (or HardwareOnlyBackend)
        mcu_step = self.backend.step(dt_us)

        # 3. Update microSD power/busy timer & full Power Tree + Virtual PPK2
        i_sd = self.sdcard.update(self.matrix, dt_us)
        pwr = self.power.update(
            matrix=self.matrix,
            mcu_mode=mcu_step.power_mode,
            i_afe_ua=afe_st.current_ua,
            i_ina219_ua=self.ina219.current_ua,
            i_sd_ua=i_sd,
            dt_s=dt_s,
        )

        # 4. Feed true rail current into INA219 shunt voltage register (100 uA LSB!)
        downstream_ua = pwr.i_mcu_ua + pwr.i_gpio_leak_ua + pwr.i_afe_ua + pwr.i_ina219_ua + pwr.i_sd_ua
        self.ina219.update_measurements(bus_v=pwr.rail_3v3_v, load_current_ua=downstream_ua)

        # 5. Build telemetry snapshot
        sample = TelemetrySample(
            time_s=self.clock.now_s,
            ecg_patient_mv=sig.v_diff_mv,
            ecg_out_v=afe_st.ecg_out_v,
            adc_counts=self.matrix.read_adc_counts(0),
            vbat_v=pwr.vbat_v,
            soc_pct=pwr.soc_pct,
            i_total_ua=pwr.i_total_ua,
            i_mcu_ua=pwr.i_mcu_ua,
            i_afe_ua=pwr.i_afe_ua,
            i_ina219_ua=pwr.i_ina219_ua,
            i_sd_ua=pwr.i_sd_ua,
            ina219_measured_ua=self.ina219.read_quantized_current_ua(),
            lod_p=afe_st.lod_p,
            lod_n=afe_st.lod_n,
            led_stat=pwr.led_stat_on,
            mcu_mode=mcu_step.power_mode.value,
            r_peak_gt=sig.is_r_peak_gt,
        )
        if record:
            self.telemetry.append(sample)
        return sample

    def get_summary(self) -> Dict[str, Any]:
        """Return a live diagnostic snapshot of the virtual board (useful for CLI & GUI)."""
        b = self.manifest["board"]
        cpu: CpuSnapshot = self.backend.get_cpu_snapshot()
        return {
            "board": f"{b['width_mm']}x{b['height_mm']} mm ({b['component_count']} parts, DRC={b['drc']['violations']})",
            "time_s": round(self.clock.now_s, 4),
            "mcu_mode": cpu.power_mode.value,
            "sysclk_mhz": round(cpu.sysclk_hz * 1e-6, 3),
            "rails_v": {k: round(v, 3) for k, v in self.matrix.rails.items()},
            "testpoints": self.matrix.read_testpoints(),
            "ppk2_total_ua": round(self.power.last_breakdown.i_total_ua, 2),
            "ina219_quantized_ua": round(self.ina219.read_quantized_current_ua(), 1),
            "led_stat_on": self.matrix.led_stat_on,
            "j_rld": self.matrix.j_rld_mode.value,
            "sd_initialized": self.sdcard.is_initialized,
            "sd_blocks_written": self.sdcard.total_blocks_written,
        }
