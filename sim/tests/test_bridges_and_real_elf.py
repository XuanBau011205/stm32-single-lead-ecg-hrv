"""Unit & integration tests for sim/bridges and real P05_ECG_Firmware.elf scenarios."""

from __future__ import annotations

import socket
import struct
import tempfile
import time
import unittest
from pathlib import Path

from sim.bridges import (
    FLAG_INA219_FAULT,
    FLAG_LOD_P,
    SD_SYNC,
    SD_TYPE_RAW_ECG,
    SD_TYPE_RR_HRV,
    SD_VERSION,
    HilErrorNack,
    HilResultAck,
    ParsedSdRawEcgBlock,
    ParsedSdRrHrvBlock,
    VirtualUartHilBridge,
    build_hil_config_req,
    build_hil_samples_in,
    crc16_ccitt_false,
    extract_frames,
    parse_hil_response,
    parse_sd_block,
)
from sim.core import VirtualPinMatrix
from sim.scenarios.ablation_suite import run_ablation_suite
from sim.scenarios.bringup_g3 import find_default_elf, run_bringup_g3
from sim.scenarios.fault_injection import run_fault_injection


def _make_raw_sd_sector(block_seq: int = 1, flags: int = 0, sample_count: int = 245) -> bytearray:
    buf = bytearray(512)
    buf[0:4] = SD_SYNC
    buf[4] = SD_VERSION
    buf[5] = SD_TYPE_RAW_ECG
    struct.pack_into("<HIIH", buf, 6, flags, block_seq, 1234, sample_count)
    for i in range(sample_count):
        struct.pack_into("<h", buf, 18 + 2 * i, 2000 + (i % 100))
    crc = crc16_ccitt_false(bytes(buf[:510]))
    struct.pack_into("<H", buf, 510, crc)
    return buf


def _make_rr_sd_sector(block_seq: int = 2, flags: int = FLAG_INA219_FAULT) -> bytearray:
    buf = bytearray(512)
    buf[0:4] = SD_SYNC
    buf[4] = SD_VERSION
    buf[5] = SD_TYPE_RR_HRV
    struct.pack_into("<HII", buf, 6, flags, block_seq, 5678)
    buf[16] = 1
    buf[17] = 1
    struct.pack_into("<HhH", buf, 18, 4050, 320, 3300)
    struct.pack_into("<IIIHH", buf, 24, 833000, 25000, 18000, 7200, 125)
    struct.pack_into("<IhI", buf, 40, 200, 1024, 833000)
    crc = crc16_ccitt_false(bytes(buf[:510]))
    struct.pack_into("<H", buf, 510, crc)
    return buf


class TestBridgesAndRealElf(unittest.TestCase):
    def test_01_sd_inspector_valid_and_faulty_sectors(self) -> None:
        raw_sec = _make_raw_sd_sector(block_seq=3, flags=FLAG_LOD_P, sample_count=245)
        parsed, err = parse_sd_block(bytes(raw_sec), lba=0)
        self.assertIsNone(err)
        self.assertIsInstance(parsed, ParsedSdRawEcgBlock)
        assert isinstance(parsed, ParsedSdRawEcgBlock)
        self.assertEqual(parsed.block_seq, 3)
        self.assertEqual(parsed.flags, FLAG_LOD_P)
        self.assertEqual(parsed.sample_count, 245)

        rr_sec = _make_rr_sd_sector(block_seq=4, flags=FLAG_INA219_FAULT)
        parsed_rr, err_rr = parse_sd_block(bytes(rr_sec), lba=1)
        self.assertIsNone(err_rr)
        self.assertIsInstance(parsed_rr, ParsedSdRrHrvBlock)
        assert isinstance(parsed_rr, ParsedSdRrHrvBlock)
        self.assertFalse(parsed_rr.ina219_valid)
        self.assertTrue(parsed_rr.battery_valid)
        self.assertEqual(parsed_rr.vbat_mv, 4050)
        self.assertEqual(len(parsed_rr.peaks), 1)

        # Bad CRC
        bad_crc = bytearray(raw_sec)
        bad_crc[510] ^= 0xFF
        self.assertIsNone(parse_sd_block(bytes(bad_crc))[0])

        # Nonzero trailer [508..509]
        bad_tr = bytearray(raw_sec)
        bad_tr[508] = 0x01
        struct.pack_into("<H", bad_tr, 510, crc16_ccitt_false(bytes(bad_tr[:510])))
        self.assertIn("bad_trailer", parse_sd_block(bytes(bad_tr))[1] or "")

        # Unknown flag bits
        bad_fl = _make_raw_sd_sector(flags=0x0100)
        self.assertIn("unknown_flags", parse_sd_block(bytes(bad_fl))[1] or "")

    def test_02_uart_hil_bridge_framing_and_tcp_loopback(self) -> None:
        cfg = build_hil_config_req(seq=7, fs_hz=250, soft_float=False, interp_enable=True, reset_state=True)
        smp = build_hil_samples_in(seq=8, first_sample_idx=0, samples=[2048, 2100, 2200])
        stream = bytearray(b"\x00\xFF" + cfg + smp + b"\xA5")
        frames = extract_frames(stream)
        self.assertEqual(frames, [cfg, smp])
        self.assertEqual(bytes(stream), b"\xA5")

        # Build a synthetic ResultAck and ErrorNack to verify parser
        ack_payload = struct.pack("<HBBIIIBBIHIIH", 8, 0, 0, 1000, 1100, 1200, 0, 0, 800000, 7500, 0, 0, 0)
        ack_hdr = struct.pack("<2sBBHH", b"\xA5\x5A", 1, 0x81, 8, len(ack_payload)) + ack_payload
        ack_frame = ack_hdr + struct.pack("<H", crc16_ccitt_false(ack_hdr))
        parsed_ack, err_ack = parse_hil_response(ack_frame)
        self.assertIsNone(err_ack)
        self.assertIsInstance(parsed_ack, HilResultAck)

        nack_hdr = struct.pack("<2sBBHHHBB", b"\xA5\x5A", 1, 0x8F, 9, 4, 9, 1, 4)
        nack_frame = nack_hdr + struct.pack("<H", crc16_ccitt_false(nack_hdr))
        parsed_nack, err_nack = parse_hil_response(nack_frame)
        self.assertIsNone(err_nack)
        self.assertIsInstance(parsed_nack, HilErrorNack)

        # TCP loopback test on ephemeral port
        matrix = VirtualPinMatrix()
        bridge = VirtualUartHilBridge(matrix, port=0)
        bridge.start()
        try:
            with socket.create_connection(("127.0.0.1", bridge.bound_port), timeout=1.0) as cli:
                cli.sendall(cfg)
                for _ in range(20):
                    bridge.poll()
                    if len(matrix.uart1_rx_fifo) >= len(cfg):
                        break
                    time.sleep(0.005)
                self.assertEqual(bytes(matrix.uart1_rx_fifo), cfg)
                matrix.uart1_tx_fifo.extend(ack_frame)
                bridge.poll()
                rx_back = cli.recv(256)
                self.assertEqual(rx_back, ack_frame)
        finally:
            bridge.stop()

    def test_03_real_elf_bringup_g3_and_fault_injection(self) -> None:
        elf = find_default_elf()
        if elf is None:
            self.skipTest("P05_ECG_Firmware.elf not found (set P05_ELF_PATH to run real ELF tests)")
        g3 = run_bringup_g3(elf)
        self.assertTrue(g3["all_passed"], msg=f"G3 failed: {g3}")
        fi = run_fault_injection(elf)
        self.assertTrue(fi["all_passed"], msg=f"Fault injection failed: {fi}")

    def test_04_real_elf_ablation_suite(self) -> None:
        elf = find_default_elf()
        if elf is None:
            self.skipTest("P05_ECG_Firmware.elf not found (set P05_ELF_PATH to run real ELF tests)")
        with tempfile.TemporaryDirectory() as td:
            out_csv = Path(td) / "ablation.csv"
            ab = run_ablation_suite(elf, output_csv=out_csv)
            self.assertTrue(ab["all_passed"], msg=f"Ablation suite failed: {ab}")
            self.assertTrue(out_csv.exists())
            self.assertGreater(out_csv.stat().st_size, 50)


if __name__ == "__main__":
    unittest.main()
