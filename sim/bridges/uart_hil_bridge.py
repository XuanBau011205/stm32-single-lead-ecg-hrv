"""
Virtual UART1 HIL Bridge (PA9/PA10 <-> in-process framer & optional TCP 127.0.0.1:5050).

Matches the wire protocol in firmware/HIL/boundary_parser.cpp & hil_session.cpp:
  Header (8 B): SYNC=0xA5 0x5A, VER=0x01, TYPE, SEQ(u16 LE), PAYLOAD_LEN(u16 LE)
  Payload (0..256 B)
  Trailer (2 B): CRC-16/CCITT-FALSE (u16 LE) over Header + Payload
"""

from __future__ import annotations

import socket
import struct
from dataclasses import dataclass
from typing import List, Optional, Sequence, Tuple, Union

from sim.bridges.sd_inspector import crc16_ccitt_false
from sim.core.pin_matrix import VirtualPinMatrix

HIL_SYNC = b"\xA5\x5A"
HIL_VERSION = 0x01
MSG_CONFIG_REQ = 0x01
MSG_SAMPLES_IN = 0x02
MSG_RESULT_ACK = 0x81
MSG_ERROR_NACK = 0x8F


@dataclass(frozen=True)
class HilPeakOut:
    peak_sample_idx: int
    sub_sample_offset_q15: int
    rr_interval_us: int


@dataclass(frozen=True)
class HilResultAck:
    seq: int
    ack_seq: int
    canonical: int
    detail: int
    min_cycles: int
    mean_cycles: int
    max_cycles: int
    peak_count: int
    hrv_valid: bool
    mean_rr_us: int
    hr_bpm_x100: int
    sdnn_us: int
    rmssd_us: int
    pnn50_permille: int
    peaks: Tuple[HilPeakOut, ...]


@dataclass(frozen=True)
class HilErrorNack:
    seq: int
    rejected_seq: int
    canonical: int
    detail: int


def _wrap_hil_frame(msg_type: int, seq: int, payload: bytes) -> bytes:
    header = struct.pack("<2sBBHH", HIL_SYNC, HIL_VERSION, msg_type, seq & 0xFFFF, len(payload))
    body = header + payload
    crc = crc16_ccitt_false(body)
    return body + struct.pack("<H", crc)


def build_hil_config_req(
    seq: int,
    fs_hz: int = 250,
    soft_float: bool = False,
    interp_enable: bool = True,
    reset_state: bool = True,
) -> bytes:
    payload = struct.pack(
        "<HBBB",
        fs_hz,
        1 if soft_float else 0,
        1 if interp_enable else 0,
        1 if reset_state else 0,
    )
    return _wrap_hil_frame(MSG_CONFIG_REQ, seq, payload)


def build_hil_samples_in(
    seq: int,
    first_sample_idx: int,
    samples: Sequence[int],
) -> bytes:
    count = len(samples)
    payload = struct.pack(f"<IB{count}h", first_sample_idx & 0xFFFFFFFF, count, *samples)
    return _wrap_hil_frame(MSG_SAMPLES_IN, seq, payload)


def parse_hil_response(frame: bytes) -> Tuple[Optional[Union[HilResultAck, HilErrorNack]], Optional[str]]:
    if len(frame) < 10:
        return None, f"short_frame:{len(frame)}"
    sync, ver, msg_type, seq, payload_len = struct.unpack_from("<2sBBHH", frame, 0)
    if sync != HIL_SYNC:
        return None, f"bad_sync:{sync!r}"
    if ver != HIL_VERSION:
        return None, f"bad_version:{ver}"
    if len(frame) != 8 + payload_len + 2:
        return None, f"length_mismatch:{len(frame)}!={8 + payload_len + 2}"
    wire_crc = struct.unpack_from("<H", frame, 8 + payload_len)[0]
    calc_crc = crc16_ccitt_false(frame[: 8 + payload_len])
    if wire_crc != calc_crc:
        return None, f"bad_crc:0x{wire_crc:04X}!=0x{calc_crc:04X}"

    payload = frame[8 : 8 + payload_len]
    if msg_type == MSG_ERROR_NACK:
        if payload_len != 4:
            return None, f"bad_nack_len:{payload_len}"
        rejected_seq, canonical, detail = struct.unpack_from("<HBB", payload, 0)
        return HilErrorNack(seq=seq, rejected_seq=rejected_seq, canonical=canonical, detail=detail), None

    if msg_type == MSG_RESULT_ACK:
        if payload_len < 34:
            return None, f"bad_ack_len:{payload_len}"
        ack_seq, canonical, detail, min_cycles, mean_cycles, max_cycles, peak_count = struct.unpack_from(
            "<HBBIIIB", payload, 0
        )
        expected_len = 34 + 10 * peak_count
        if payload_len != expected_len:
            return None, f"bad_ack_peak_len:{payload_len}!={expected_len}"
        peaks: List[HilPeakOut] = []
        off = 17
        for _ in range(peak_count):
            p_idx, p_off, p_rr = struct.unpack_from("<IhI", payload, off)
            peaks.append(HilPeakOut(p_idx, p_off, p_rr))
            off += 10
        hrv_valid_raw, mean_rr_us, hr_bpm_x100, sdnn_us, rmssd_us, pnn50_permille = struct.unpack_from(
            "<BIHIIH", payload, off
        )
        return HilResultAck(
            seq=seq,
            ack_seq=ack_seq,
            canonical=canonical,
            detail=detail,
            min_cycles=min_cycles,
            mean_cycles=mean_cycles,
            max_cycles=max_cycles,
            peak_count=peak_count,
            hrv_valid=(hrv_valid_raw != 0),
            mean_rr_us=mean_rr_us,
            hr_bpm_x100=hr_bpm_x100,
            sdnn_us=sdnn_us,
            rmssd_us=rmssd_us,
            pnn50_permille=pnn50_permille,
            peaks=tuple(peaks),
        ), None

    return None, f"unknown_msg_type:0x{msg_type:02X}"


def extract_frames(rx_buf: bytearray) -> List[bytes]:
    """Extract all complete 0xA5 0x5A HIL frames from a byte stream buffer."""
    frames: List[bytes] = []
    while len(rx_buf) >= 2:
        idx = rx_buf.find(HIL_SYNC)
        if idx < 0:
            if rx_buf[-1] == 0xA5:
                del rx_buf[:-1]
            else:
                rx_buf.clear()
            break
        if idx > 0:
            del rx_buf[:idx]
        if len(rx_buf) < 8:
            break
        payload_len = struct.unpack_from("<H", rx_buf, 6)[0]
        if payload_len > 256:
            del rx_buf[:2]
            continue
        total_len = 8 + payload_len + 2
        if len(rx_buf) < total_len:
            break
        frames.append(bytes(rx_buf[:total_len]))
        del rx_buf[:total_len]
    return frames


class VirtualUartHilBridge:
    """Bridges VirtualPinMatrix UART1 FIFOs with in-process tests or a TCP client."""

    def __init__(self, matrix: VirtualPinMatrix, host: str = "127.0.0.1", port: int = 5050) -> None:
        self.matrix = matrix
        self.host = host
        self.port = port
        self._tx_stream = bytearray()
        self._server_sock: Optional[socket.socket] = None
        self._client_sock: Optional[socket.socket] = None

    @property
    def bound_port(self) -> int:
        if self._server_sock is not None:
            return int(self._server_sock.getsockname()[1])
        return self.port

    def inject_rx_bytes(self, data: bytes) -> None:
        self.matrix.uart1_rx_fifo.extend(data)

    def inject_rx_frame(self, frame: bytes) -> None:
        self.inject_rx_bytes(frame)

    def drain_tx_frames(self) -> List[Union[HilResultAck, HilErrorNack]]:
        while self.matrix.uart1_tx_fifo:
            self._tx_stream.append(self.matrix.uart1_tx_fifo.popleft())
        out: List[Union[HilResultAck, HilErrorNack]] = []
        for raw in extract_frames(self._tx_stream):
            parsed, err = parse_hil_response(raw)
            if err is not None or parsed is None:
                raise ValueError(f"Invalid HIL response frame from MCU: {err}")
            out.append(parsed)
        return out

    def start(self) -> None:
        if self._server_sock is not None:
            return
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((self.host, self.port))
        srv.listen(1)
        srv.setblocking(False)
        self._server_sock = srv

    def poll(self) -> None:
        """Pump TCP <-> VirtualPinMatrix UART1 FIFOs (call once per simulation tick)."""
        if self._server_sock is None:
            return
        if self._client_sock is None:
            try:
                conn, _ = self._server_sock.accept()
                conn.setblocking(False)
                self._client_sock = conn
            except BlockingIOError:
                pass
        if self._client_sock is not None:
            try:
                rx = self._client_sock.recv(4096)
                if rx:
                    self.matrix.uart1_rx_fifo.extend(rx)
                else:
                    self._client_sock.close()
                    self._client_sock = None
            except BlockingIOError:
                pass
            except OSError:
                self._client_sock.close()
                self._client_sock = None

        if self._client_sock is not None and self.matrix.uart1_tx_fifo:
            tx = bytes(self.matrix.uart1_tx_fifo)
            self.matrix.uart1_tx_fifo.clear()
            try:
                self._client_sock.sendall(tx)
            except OSError:
                self._client_sock.close()
                self._client_sock = None

    def stop(self) -> None:
        if self._client_sock is not None:
            self._client_sock.close()
            self._client_sock = None
        if self._server_sock is not None:
            self._server_sock.close()
            self._server_sock = None
