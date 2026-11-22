"""
VirtualMicroSD — SPI1 Mode-0 SDv2/SDHC Card Model (512-byte blocks + write latency).

Implements:
  - Power-gating awareness via 3V3_SD rail (AO3401 P-MOSFET):
      * Power OFF (3V3_SD < 2.7 V) or card removed -> 0.0 uA, SPI returns 0xFF,
        internal card state resets so firmware MUST re-run CMD0/CMD8/ACMD41!
  - Standard SPI SD commands:
      * CMD0  (GO_IDLE_STATE)      -> R1 = 0x01
      * CMD8  (SEND_IF_COND)       -> R7 = 0x01 0x00 0x00 0x01 0xAA
      * CMD55 (APP_CMD)            -> R1 = 0x01 / 0x00
      * ACMD41 (SD_SEND_OP_COND)   -> R1 = 0x00 (Ready!)
      * CMD58 (READ_OCR)           -> R3 = 0x00 0xC0 0xFF 0x80 0x00 (SDHC CCS=1)
      * CMD17 (READ_SINGLE_BLOCK)  -> R1=0x00, token 0xFE, 512 bytes, 2B dummy CRC
      * CMD24 (WRITE_SINGLE_BLOCK) -> R1=0x00, awaits 0xFE + 512B + 2B CRC -> 0x05 + busy 0x00!
  - Realistic flash write busy latency (default 4 ms, configurable stall to stress Ping-Pong DMA)
    with realistic 30 mA active write current pulse visible on Virtual PPK2!
"""

from __future__ import annotations

from collections import deque
from pathlib import Path
from typing import Deque, Dict, Optional

from sim.core.pin_matrix import VirtualPinMatrix


class VirtualMicroSD:
    """SPI1 slave model of J_SD (TF-01A microSD slot) with 512-byte sector storage."""

    SECTOR_SIZE = 512

    def __init__(
        self,
        write_latency_ms: float = 4.0,
        i_idle_ua: float = 500.0,
        i_write_ua: float = 30000.0,
        backing_img: Optional[Path] = None,
    ) -> None:
        self.write_latency_us: float = float(write_latency_ms) * 1000.0
        self.i_idle_ua: float = float(i_idle_ua)
        self.i_write_ua: float = float(i_write_ua)
        self.backing_img: Optional[Path] = backing_img

        self._sectors: Dict[int, bytearray] = {}
        self.total_blocks_written: int = 0
        self.total_blocks_read: int = 0
        self._reset_card_state()

    def _reset_card_state(self) -> None:
        self._powered: bool = False
        self._initialized: bool = False
        self._app_cmd_pending: bool = False
        self._cmd_buf: bytearray = bytearray()
        self._tx_queue: Deque[int] = deque()
        self._write_mode: str = "IDLE"  # IDLE | WAIT_TOKEN | RECV_DATA | RECV_CRC
        self._write_sector: int = 0
        self._write_buf: bytearray = bytearray()
        self._crc_rem: int = 0
        self._busy_remaining_us: float = 0.0
        self.current_ua: float = 0.0

    @property
    def is_initialized(self) -> bool:
        return self._initialized

    @property
    def is_busy(self) -> bool:
        return self._busy_remaining_us > 0.0

    def update(self, matrix: VirtualPinMatrix, dt_us: float) -> float:
        """Update power state, write-busy timer, and instantaneous microSD current (uA)."""
        powered_now = matrix.sd_card_inserted and (matrix.rails["3V3_SD"] >= 2.7)
        if not powered_now:
            if self._powered:
                self._reset_card_state()
            self.current_ua = 0.0
            return 0.0

        self._powered = True
        if self._busy_remaining_us > 0.0:
            self._busy_remaining_us = max(0.0, self._busy_remaining_us - dt_us)
            self.current_ua = self.i_write_ua
        else:
            self.current_ua = self.i_idle_ua
        return self.current_ua

    def read_sector_direct(self, sector: int) -> bytes:
        """Inspect raw 512-byte sector directly (used by SD Inspector & unit tests)."""
        data = self._sectors.get(int(sector))
        return bytes(data) if data is not None else bytes(self.SECTOR_SIZE)

    def _commit_sector(self, sector: int, data: bytes) -> None:
        self._sectors[sector] = bytearray(data)
        self.total_blocks_written += 1
        if self.backing_img is not None:
            self.backing_img.parent.mkdir(parents=True, exist_ok=True)
            mode = "r+b" if self.backing_img.exists() else "wb"
            with self.backing_img.open(mode) as f:
                f.seek(sector * self.SECTOR_SIZE)
                f.write(data)

    def spi_transfer_byte(self, tx: int) -> int:
        """Process 1 full-duplex SPI1 byte."""
        if not self._powered:
            return 0xFF

        # 1. If queued response bytes exist, shift them out first
        if self._tx_queue:
            return self._tx_queue.popleft()

        # 2. If card is busy programming flash, return 0x00 until busy timer expires
        if self._busy_remaining_us > 0.0:
            return 0x00

        # 3. Handle CMD24 write data block phase
        if self._write_mode == "WAIT_TOKEN":
            if tx == 0xFE:
                self._write_mode = "RECV_DATA"
                self._write_buf = bytearray()
            return 0xFF

        if self._write_mode == "RECV_DATA":
            self._write_buf.append(tx)
            if len(self._write_buf) == self.SECTOR_SIZE:
                self._write_mode = "RECV_CRC"
                self._crc_rem = 2
            return 0xFF

        if self._write_mode == "RECV_CRC":
            self._crc_rem -= 1
            if self._crc_rem <= 0:
                self._commit_sector(self._write_sector, bytes(self._write_buf))
                self._write_mode = "IDLE"
                self._busy_remaining_us = self.write_latency_us
                return 0x05  # Data Accepted token!
            return 0xFF

        # 4. Command packet assembly (6 bytes starting with 0b01xxxxxx)
        if not self._cmd_buf:
            if (tx & 0xC0) == 0x40:
                self._cmd_buf.append(tx)
            return 0xFF

        self._cmd_buf.append(tx)
        if len(self._cmd_buf) < 6:
            return 0xFF

        cmd = self._cmd_buf[0] & 0x3F
        arg = (
            (self._cmd_buf[1] << 24)
            | (self._cmd_buf[2] << 16)
            | (self._cmd_buf[3] << 8)
            | self._cmd_buf[4]
        )
        self._cmd_buf.clear()
        self._handle_command(cmd, arg)
        return 0xFF  # Standard 1-byte NCR delay before R1 on next clock

    def _handle_command(self, cmd: int, arg: int) -> None:
        if cmd == 0:  # CMD0: GO_IDLE_STATE
            self._initialized = False
            self._app_cmd_pending = False
            self._tx_queue.append(0x01)
            return

        if cmd == 8:  # CMD8: SEND_IF_COND (echo voltage & check pattern)
            self._tx_queue.extend([0x01, 0x00, 0x00, (arg >> 8) & 0x0F, arg & 0xFF])
            return

        if cmd == 55:  # CMD55: APP_CMD
            self._app_cmd_pending = True
            self._tx_queue.append(0x00 if self._initialized else 0x01)
            return

        if cmd == 41 and self._app_cmd_pending:  # ACMD41: SD_SEND_OP_COND
            self._app_cmd_pending = False
            self._initialized = True
            self._tx_queue.append(0x00)
            return

        self._app_cmd_pending = False

        if cmd == 58:  # CMD58: READ_OCR (SDHC CCS=1, 3.3V)
            self._tx_queue.extend([0x00 if self._initialized else 0x01, 0xC0, 0xFF, 0x80, 0x00])
            return

        if not self._initialized:
            self._tx_queue.append(0x05)  # Illegal command before init
            return

        if cmd == 17:  # CMD17: READ_SINGLE_BLOCK
            sector_bytes = self.read_sector_direct(arg)
            self.total_blocks_read += 1
            self._tx_queue.append(0x00)  # R1 OK
            self._tx_queue.append(0xFE)  # Start block token
            self._tx_queue.extend(sector_bytes)
            self._tx_queue.extend([0xFF, 0xFF])  # Dummy CRC16
            return

        if cmd == 24:  # CMD24: WRITE_SINGLE_BLOCK
            self._write_sector = arg
            self._write_mode = "WAIT_TOKEN"
            self._tx_queue.append(0x00)  # R1 OK
            return

        # Unsupported command -> Illegal Command bit
        self._tx_queue.append(0x04)
