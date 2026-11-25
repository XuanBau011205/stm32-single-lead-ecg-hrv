"""
SD 512-byte Block Inspector matching firmware/Core/sd_block_formatter.cpp &
firmware/Core/sensor_storage_boundary.cpp byte-for-byte.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple, Union

from sim.plant.microsd_spi import VirtualMicroSD

SD_SYNC = b"ECG5"
SD_VERSION = 0x01
SD_TYPE_RAW_ECG = 0x01
SD_TYPE_RR_HRV = 0x02
SD_MAX_RAW_SAMPLES = 245
SD_MAX_RR_RECORDS = 46

FLAG_LOD_P = 1 << 0
FLAG_LOD_N = 1 << 1
FLAG_SOFT_FLOAT = 1 << 2
FLAG_FS_500HZ = 1 << 3
FLAG_PING_PONG_OVERFLOW_GAP = 1 << 4
FLAG_LOW_BATTERY = 1 << 5
FLAG_INA219_FAULT = 1 << 6
FLAG_BAT_SENSE_FAULT = 1 << 7
SD_FLAGS_MASK = 0x00FF


def crc16_ccitt_false(data: bytes) -> int:
    """CRC-16/CCITT-FALSE (poly=0x1021, init=0xFFFF, no xorout)."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


@dataclass(frozen=True)
class ParsedSdPeak:
    peak_sample_idx: int
    sub_sample_offset_q15: int
    rr_interval_us: int


@dataclass(frozen=True)
class ParsedSdRawEcgBlock:
    lba: int
    block_seq: int
    timestamp_ms: int
    flags: int
    sample_count: int
    samples: Tuple[int, ...]


@dataclass(frozen=True)
class ParsedSdRrHrvBlock:
    lba: int
    block_seq: int
    timestamp_ms: int
    flags: int
    valid_rr_count: int
    hrv_valid: bool
    vbat_mv: int
    ina219_current_ua: int
    ina219_bus_mv: int
    mean_rr_us: int
    sdnn_us: int
    rmssd_us: int
    hr_bpm_x100: int
    pnn50_permille: int
    peaks: Tuple[ParsedSdPeak, ...]
    ina219_valid: bool
    battery_valid: bool


@dataclass
class SdInspectionReport:
    total_sectors: int = 0
    valid_raw_blocks: List[ParsedSdRawEcgBlock] = field(default_factory=list)
    valid_rr_hrv_blocks: List[ParsedSdRrHrvBlock] = field(default_factory=list)
    invalid_sectors: List[Tuple[int, str]] = field(default_factory=list)
    seq_gaps: List[Tuple[int, int]] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return (
            len(self.invalid_sectors) == 0
            and len(self.seq_gaps) == 0
            and (len(self.valid_raw_blocks) + len(self.valid_rr_hrv_blocks)) > 0
        )


def parse_sd_block(
    sector: bytes,
    lba: int = 0,
) -> Tuple[Optional[Union[ParsedSdRawEcgBlock, ParsedSdRrHrvBlock]], Optional[str]]:
    """Decode and validate one 512-byte P05 SD sector."""
    if len(sector) != 512:
        return None, f"bad_length:{len(sector)}"
    if sector[0:4] != SD_SYNC:
        return None, f"bad_sync:{sector[0:4]!r}"
    if sector[4] != SD_VERSION:
        return None, f"bad_version:{sector[4]}"

    trailer = struct.unpack_from("<H", sector, 508)[0]
    if trailer != 0x0000:
        return None, f"bad_trailer:0x{trailer:04X}"

    wire_crc = struct.unpack_from("<H", sector, 510)[0]
    calc_crc = crc16_ccitt_false(sector[:510])
    if wire_crc != calc_crc:
        return None, f"bad_crc:wire=0x{wire_crc:04X},calc=0x{calc_crc:04X}"

    block_type = sector[5]
    flags, block_seq, timestamp_ms = struct.unpack_from("<HII", sector, 6)
    if (flags & ~SD_FLAGS_MASK) != 0:
        return None, f"unknown_flags:0x{flags:04X}"

    if block_type == SD_TYPE_RAW_ECG:
        sample_count = struct.unpack_from("<H", sector, 16)[0]
        if sample_count == 0 or sample_count > SD_MAX_RAW_SAMPLES:
            return None, f"bad_raw_sample_count:{sample_count}"
        samples = struct.unpack_from(f"<{sample_count}h", sector, 18)
        for s in samples:
            if s < 0 or s > 4095:
                return None, f"raw_sample_out_of_range:{s}"
        return ParsedSdRawEcgBlock(
            lba=lba,
            block_seq=block_seq,
            timestamp_ms=timestamp_ms,
            flags=flags,
            sample_count=sample_count,
            samples=samples,
        ), None

    if block_type == SD_TYPE_RR_HRV:
        valid_rr_count = sector[16]
        hrv_valid = sector[17] != 0
        if valid_rr_count > SD_MAX_RR_RECORDS:
            return None, f"bad_rr_count:{valid_rr_count}"
        vbat_mv, cur_div10, bus_mv = struct.unpack_from("<HhH", sector, 18)
        mean_rr_us, sdnn_us, rmssd_us, hr_bpm_x100, pnn50_permille = struct.unpack_from(
            "<IIIHH", sector, 24
        )
        peaks: List[ParsedSdPeak] = []
        for i in range(valid_rr_count):
            off = 40 + 10 * i
            p_idx, p_off, p_rr = struct.unpack_from("<IhI", sector, off)
            peaks.append(ParsedSdPeak(p_idx, p_off, p_rr))
        return ParsedSdRrHrvBlock(
            lba=lba,
            block_seq=block_seq,
            timestamp_ms=timestamp_ms,
            flags=flags,
            valid_rr_count=valid_rr_count,
            hrv_valid=hrv_valid,
            vbat_mv=vbat_mv,
            ina219_current_ua=cur_div10 * 10,
            ina219_bus_mv=bus_mv,
            mean_rr_us=mean_rr_us,
            sdnn_us=sdnn_us,
            rmssd_us=rmssd_us,
            hr_bpm_x100=hr_bpm_x100,
            pnn50_permille=pnn50_permille,
            peaks=tuple(peaks),
            ina219_valid=(flags & FLAG_INA219_FAULT) == 0,
            battery_valid=(flags & FLAG_BAT_SENSE_FAULT) == 0,
        ), None

    return None, f"unknown_block_type:0x{block_type:02X}"


def _inspect_sector_pairs(sectors: List[Tuple[int, bytes]]) -> SdInspectionReport:
    report = SdInspectionReport(total_sectors=len(sectors))
    seqs: List[int] = []
    for lba, data in sorted(sectors, key=lambda kv: kv[0]):
        parsed, err = parse_sd_block(data, lba=lba)
        if err is not None or parsed is None:
            report.invalid_sectors.append((lba, err or "parse_error"))
            continue
        seqs.append(parsed.block_seq)
        if isinstance(parsed, ParsedSdRawEcgBlock):
            report.valid_raw_blocks.append(parsed)
        else:
            report.valid_rr_hrv_blocks.append(parsed)

    for prev_s, cur_s in zip(seqs, seqs[1:]):
        if cur_s != prev_s + 1:
            report.seq_gaps.append((prev_s, cur_s))
    return report


def inspect_sdcard(sdcard: VirtualMicroSD) -> SdInspectionReport:
    """Inspect all sectors written to an in-memory VirtualMicroSD instance."""
    pairs = [(lba, bytes(buf)) for lba, buf in sdcard._sectors.items()]
    return _inspect_sector_pairs(pairs)


def inspect_image_file(path: Path) -> SdInspectionReport:
    """Inspect all 512-byte sectors from a raw .img file on disk."""
    raw = Path(path).read_bytes()
    n_sectors = len(raw) // 512
    pairs: List[Tuple[int, bytes]] = []
    for lba in range(n_sectors):
        sec = raw[lba * 512 : (lba + 1) * 512]
        if any(sec):
            pairs.append((lba, sec))
    return _inspect_sector_pairs(pairs)
