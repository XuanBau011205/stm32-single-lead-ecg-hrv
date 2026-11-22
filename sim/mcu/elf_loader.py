"""
Firmware Image Loader (.elf, .bin, .hex) for STM32F103C8T6 Virtual Flash (0x08000000).

Loads:
  - ELF32 ARM executables via `pyelftools` (honors exact physical load address p_paddr
    so `.data` initializers `_sidata` land at their exact Flash addresses, plus extracts symbols)
  - Raw `.bin` files flashed at `0x08000000`
  - Intel `.hex` files (Record types 00, 01, 04)
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Tuple

FLASH_BASE = 0x08000000
FLASH_SIZE = 64 * 1024  # 64 KB (STM32F103C8T6)
SRAM_BASE = 0x20000000
SRAM_SIZE = 20 * 1024   # 20 KB


@dataclass
class LoadedImage:
    path: Path
    fmt: str
    segments: List[Tuple[int, bytes]]  # (load_addr, data)
    entry_point: int
    total_flash_bytes: int
    symbols: Dict[str, int] = field(default_factory=dict)
    addr_to_symbol: Dict[int, str] = field(default_factory=dict)


def _load_elf(path: Path) -> LoadedImage:
    from elftools.elf.elffile import ELFFile
    from elftools.elf.sections import SymbolTableSection

    segments: List[Tuple[int, bytes]] = []
    symbols: Dict[str, int] = {}
    addr_to_sym: Dict[int, str] = {}
    total_flash = 0

    with path.open("rb") as f:
        elf = ELFFile(f)
        entry = int(elf.header["e_entry"])
        for seg in elf.iter_segments():
            if seg["p_type"] != "PT_LOAD":
                continue
            filesz = int(seg["p_filesz"])
            if filesz <= 0:
                continue
            paddr = int(seg["p_paddr"])
            data = seg.data()
            segments.append((paddr, data))
            if FLASH_BASE <= paddr < FLASH_BASE + FLASH_SIZE:
                total_flash += len(data)

        for sec in elf.iter_sections():
            if isinstance(sec, SymbolTableSection):
                for sym in sec.iter_symbols():
                    name = sym.name
                    val = int(sym["st_value"])
                    if name and val != 0:
                        symbols[name] = val
                        addr_to_sym[val & ~1] = name

    return LoadedImage(
        path=path,
        fmt="elf",
        segments=segments,
        entry_point=entry,
        total_flash_bytes=total_flash,
        symbols=symbols,
        addr_to_symbol=addr_to_sym,
    )


def _load_bin(path: Path, base_addr: int = FLASH_BASE) -> LoadedImage:
    data = path.read_bytes()
    if len(data) > FLASH_SIZE:
        raise ValueError(f"Binary size ({len(data)} B) exceeds STM32F103C8T6 64 KB Flash!")
    entry = int.from_bytes(data[4:8], "little") if len(data) >= 8 else base_addr
    return LoadedImage(
        path=path,
        fmt="bin",
        segments=[(base_addr, data)],
        entry_point=entry,
        total_flash_bytes=len(data),
    )


def _load_hex(path: Path) -> LoadedImage:
    """Minimal Intel HEX parser (types 00 Data, 01 EOF, 04 Extended Linear Address)."""
    upper_addr = 0
    chunks: List[Tuple[int, bytes]] = []
    total_flash = 0

    for line_no, raw in enumerate(path.read_text(encoding="ascii", errors="ignore").splitlines(), start=1):
        line = raw.strip()
        if not line or not line.startswith(":"):
            continue
        byte_count = int(line[1:3], 16)
        addr16 = int(line[3:7], 16)
        rec_type = int(line[7:9], 16)
        payload = bytes.fromhex(line[9 : 9 + byte_count * 2])
        if rec_type == 0x00:
            full_addr = (upper_addr << 16) | addr16
            chunks.append((full_addr, payload))
            if FLASH_BASE <= full_addr < FLASH_BASE + FLASH_SIZE:
                total_flash += len(payload)
        elif rec_type == 0x01:
            break
        elif rec_type == 0x04 and len(payload) == 2:
            upper_addr = int.from_bytes(payload, "big")
        elif rec_type not in (0x02, 0x03, 0x05):
            raise ValueError(f"Unsupported Intel HEX record type {rec_type:#x} at line {line_no}")

    # Merge contiguous chunks
    chunks.sort(key=lambda x: x[0])
    merged: List[Tuple[int, bytes]] = []
    for addr, data in chunks:
        if merged and merged[-1][0] + len(merged[-1][1]) == addr:
            prev_addr, prev_data = merged[-1]
            merged[-1] = (prev_addr, prev_data + data)
        else:
            merged.append((addr, data))

    entry = FLASH_BASE
    for addr, data in merged:
        if addr <= FLASH_BASE + 4 and addr + len(data) >= FLASH_BASE + 8:
            off = (FLASH_BASE + 4) - addr
            entry = int.from_bytes(data[off : off + 4], "little")
            break

    return LoadedImage(
        path=path,
        fmt="hex",
        segments=merged,
        entry_point=entry,
        total_flash_bytes=total_flash,
    )


def load_firmware_image(path: Path) -> LoadedImage:
    """Auto-detect and load `.elf`, `.bin`, or `.hex` firmware image."""
    p = Path(path)
    if not p.exists():
        raise FileNotFoundError(f"Firmware file not found: {p}")
    suffix = p.suffix.lower()
    if suffix == ".elf":
        return _load_elf(p)
    if suffix == ".bin":
        return _load_bin(p)
    if suffix in (".hex", ".ihex"):
        return _load_hex(p)
    # Fallback: sniff ELF magic header
    head = p.read_bytes()[:4]
    if head == b"\x7fELF":
        return _load_elf(p)
    return _load_bin(p)
