"""
VirtualPinMatrix — Exact electrical & mechanical netlist bus matching P05_ECG.SchDoc.

Bridges all physical plant models (Patient/AD8232, PowerTree, INA219, microSD, UART1)
with any IFirmwareBackend (Unicorn ARM Cortex-M3 or Fast SIL C-DLL).
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable, Deque, Dict, Optional, Tuple


class GpioMode(str, Enum):
    ANALOG = "ANALOG"
    INPUT_FLOATING = "INPUT_FLOATING"
    INPUT_PULL = "INPUT_PULL"
    OUTPUT_PP = "OUTPUT_PP"
    OUTPUT_OD = "OUTPUT_OD"
    AF_PP = "AF_PP"
    AF_OD = "AF_OD"


class RldJumperMode(str, Enum):
    ACTIVE_RLD = "ACTIVE_RLD"  # J_RLD pins 1-2: AD8232 active Right-Leg Drive (high CMRR)
    VREF_GND = "VREF_GND"      # J_RLD pins 2-3: Passive VREF_1V65 reference grounding (Ablation 3)


@dataclass
class VirtualPinMatrix:
    """Single source of truth for all analog, digital, power, and bus states on the P05 PCB."""

    # --- Power Rails (Volts) ---
    rails: Dict[str, float] = field(default_factory=lambda: {
        "VBUS": 0.0,
        "VBAT": 3.85,
        "3V3_PRE": 3.30,
        "3V3_IN": 3.30,
        "3V3_MAIN": 3.30,
        "VDDA": 3.30,
        "3V3_SD": 0.0,
        "VREF_1V65": 1.65,
    })

    # --- Analog Nets (Volts) ---
    analog: Dict[str, float] = field(default_factory=lambda: {
        "ECG_OUT": 1.65,    # PA0 / ADC1_IN0 / TP_ECG_OUT
        "BAT_SENSE": 0.0,   # PB1 / ADC1_IN9
    })

    # --- Digital GPIO Levels (True = HIGH, False = LOW) ---
    digital: Dict[str, bool] = field(default_factory=lambda: {
        "ECG_LOD_P": False,     # PA1 (input from AD8232 LO+)
        "ECG_LOD_N": False,     # PA2 (input from AD8232 LO-)
        "ECG_SDN": False,       # PA3 (MCU output; False=Shutdown, True=Active)
        "SD_CS": True,          # PA4 (MCU output; Active-Low chip select)
        "SD_PWR_EN": True,      # PB0 / TP_SD_EN (MCU output; Active-Low AO3401 -> True=OFF)
        "BAT_SENSE_EN": False,  # PB5 (MCU output; Active-High 2N7002 -> False=OFF)
        "WAKE_BTN": True,       # PB12 / EXTI12 (Active-Low with 100k pull-up -> True=Released)
        "NRST": True,           # NRST (Active-Low -> True=Running)
    })

    # --- GPIO Electrical Pin Modes (used for accurate Stop Mode leakage audit!) ---
    gpio_modes: Dict[str, GpioMode] = field(default_factory=lambda: {
        "PA0": GpioMode.ANALOG,
        "PA1": GpioMode.ANALOG,
        "PA2": GpioMode.ANALOG,
        "PA3": GpioMode.OUTPUT_PP,
        "PA4": GpioMode.ANALOG,
        "PA5": GpioMode.ANALOG,
        "PA6": GpioMode.ANALOG,
        "PA7": GpioMode.ANALOG,
        "PA8": GpioMode.ANALOG,
        "PA9": GpioMode.ANALOG,
        "PA10": GpioMode.ANALOG,
        "PB0": GpioMode.OUTPUT_PP,
        "PB1": GpioMode.ANALOG,
        "PB5": GpioMode.OUTPUT_PP,
        "PB6": GpioMode.ANALOG,
        "PB7": GpioMode.ANALOG,
        "PB12": GpioMode.INPUT_FLOATING,  # External 100k pull-up keeps PB12 clean at 3.3V (0 uA)
    })

    # --- Clock Outputs ---
    mco_hz: float = 0.0  # PA8 / TP_MCO (8e6 Hz when HSE -> MCO enabled)

    # --- Board Indicators ---
    led_stat_on: bool = False  # LED_STAT red LED driven by MCP73831 STAT pin

    # --- Interactive Mechanical Controls (User / Fault Injector inputs) ---
    sw_rst_pressed: bool = False
    sw_wake_pressed: bool = False
    j_rld_mode: RldJumperMode = RldJumperMode.ACTIVE_RLD
    j_imeas_closed: bool = True  # Jumper J_IMEAS closed (or bridged through Virtual PPK2)
    usb_connected: bool = False
    sd_card_inserted: bool = True
    electrodes: Dict[str, bool] = field(default_factory=lambda: {
        "RA": True,
        "LA": True,
        "RL": True,
    })

    # --- UART1 Byte Streams (PA9 TX MCU->Host, PA10 RX Host->MCU) ---
    uart1_tx_fifo: Deque[int] = field(default_factory=lambda: deque(maxlen=8192))
    uart1_rx_fifo: Deque[int] = field(default_factory=lambda: deque(maxlen=8192))

    # --- Registered Bus Slaves (attached by Plant models) ---
    _spi1_xfer: Optional[Callable[[int], int]] = field(default=None, repr=False)
    _i2c1_write: Optional[Callable[[int, bytes], bool]] = field(default=None, repr=False)
    _i2c1_read: Optional[Callable[[int, int], Tuple[bool, bytes]]] = field(default=None, repr=False)
    _exti12_callbacks: list = field(default_factory=list, repr=False)

    def register_spi1_slave(self, xfer_fn: Callable[[int], int]) -> None:
        self._spi1_xfer = xfer_fn

    def register_i2c1_bus(
        self,
        write_fn: Callable[[int, bytes], bool],
        read_fn: Callable[[int, int], Tuple[bool, bytes]],
    ) -> None:
        self._i2c1_write = write_fn
        self._i2c1_read = read_fn

    def register_exti12_callback(self, cb: Callable[[bool], None]) -> None:
        self._exti12_callbacks.append(cb)

    def set_wake_button(self, pressed: bool) -> None:
        prev = self.sw_wake_pressed
        self.sw_wake_pressed = bool(pressed)
        self.digital["WAKE_BTN"] = not self.sw_wake_pressed  # Active-low
        if self.sw_wake_pressed != prev:
            for cb in self._exti12_callbacks:
                cb(self.sw_wake_pressed)

    def set_reset_button(self, pressed: bool) -> None:
        self.sw_rst_pressed = bool(pressed)
        self.digital["NRST"] = not self.sw_rst_pressed  # Active-low

    def spi1_transfer_byte(self, tx_byte: int) -> int:
        """Transfer 1 byte on SPI1 (respecting SD_CS active-low and 3V3_SD rail)."""
        if self._spi1_xfer is None or self.digital["SD_CS"] or self.rails["3V3_SD"] < 2.7:
            return 0xFF
        return self._spi1_xfer(tx_byte & 0xFF) & 0xFF

    def i2c1_write(self, addr_7bit: int, data: bytes) -> bool:
        """Write bytes to a 7-bit I2C1 slave address (returns True on ACK, False on NACK)."""
        if self._i2c1_write is None or self.rails["3V3_MAIN"] < 2.7:
            return False
        return self._i2c1_write(addr_7bit & 0x7F, bytes(data))

    def i2c1_read(self, addr_7bit: int, length: int) -> Tuple[bool, bytes]:
        """Read `length` bytes from a 7-bit I2C1 slave address."""
        if self._i2c1_read is None or self.rails["3V3_MAIN"] < 2.7:
            return False, b""
        return self._i2c1_read(addr_7bit & 0x7F, length)

    def read_adc_counts(self, channel: int, adc_max: int = 4095) -> int:
        """Quantize an analog pin to 12-bit STM32 ADC counts referenced to VDDA."""
        vdda = self.rails["VDDA"]
        if vdda <= 0.1:
            return 0
        if channel == 0:  # PA0 -> ECG_OUT
            vin = self.analog["ECG_OUT"]
        elif channel == 9:  # PB1 -> BAT_SENSE
            vin = self.analog["BAT_SENSE"]
        else:
            vin = 0.0
        ratio = max(0.0, min(1.0, vin / vdda))
        return int(round(ratio * adc_max))

    def count_non_analog_floating_pins(self) -> int:
        """Count unused pins left in floating digital input mode (causes Stop Mode leakage)."""
        # PB12 has external 100k pull-up to 3V3_MAIN, so it has 0 uA leakage when released.
        leaking = 0
        for pin, mode in self.gpio_modes.items():
            if pin == "PB12":
                continue
            if mode == GpioMode.INPUT_FLOATING:
                leaking += 1
        return leaking

    def read_testpoints(self) -> Dict[str, Any]:
        """Return live probe readings on the 3 physical PCB testpoints."""
        return {
            "TP_ECG_OUT_V": round(self.analog["ECG_OUT"], 4),
            "TP_MCO_Hz": self.mco_hz,
            "TP_SD_EN_V": round(self.rails["3V3_MAIN"] if self.digital["SD_PWR_EN"] else 0.0, 3),
        }
