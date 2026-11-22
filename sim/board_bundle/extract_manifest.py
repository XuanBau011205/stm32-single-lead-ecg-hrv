"""
Extract `board_manifest.json` directly (read-only) from verified P05 Altium Gate G2 artifacts:
  - hardware/bom/P05_ECG_PickAndPlace.csv (91 component positions & rotations)
  - hardware/bom/P05_ECG_BOM.xlsx         (43 line items, exact R/C/IC values)
  - hardware/gerber/raw/P05_ECG.GM1       (80.0 x 55.0 mm board outline)
  - hardware/gerber/raw/*.TXT             (PTH/NPTH/Slot drill counts)
  - hardware/gerber/Design Rule Check - P05_ECG.drc (DRC = 0 verification)

Pure Python stdlib (zero third-party dependencies).
Usage:
    py -m sim.board_bundle.extract_manifest
"""

from __future__ import annotations

import csv
import json
import re
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


# Default repository root relative to this module (sim/board_bundle/extract_manifest.py)
DEFAULT_REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST_PATH = Path(__file__).resolve().parent / "board_manifest.json"


# Official STM32F103C8T6 pinout mapping (matched 1-to-1 with docs/SPECIFICATION.md & P05_ECG.SchDoc)
STM32_PINOUT: Dict[str, Dict[str, Any]] = {
    "PA0":  {"net": "ECG_OUT",      "dir": "ANALOG_IN",   "peripheral": "ADC1_IN0",  "target": "U_AFE (AD8232) OUT + TP_ECG_OUT"},
    "PA1":  {"net": "ECG_LOD_P",    "dir": "DIGITAL_IN",  "peripheral": "GPIOA.1",   "target": "U_AFE LO+"},
    "PA2":  {"net": "ECG_LOD_N",    "dir": "DIGITAL_IN",  "peripheral": "GPIOA.2",   "target": "U_AFE LO-"},
    "PA3":  {"net": "ECG_SDN",      "dir": "DIGITAL_OUT", "peripheral": "GPIOA.3",   "target": "U_AFE SDN (Active-Low)"},
    "PA4":  {"net": "SD_CS",        "dir": "DIGITAL_OUT", "peripheral": "SPI1_NSS",  "target": "J_SD CS (Active-Low)"},
    "PA5":  {"net": "SPI1_SCK",     "dir": "AF_OUT",      "peripheral": "SPI1_SCK",  "target": "J_SD CLK"},
    "PA6":  {"net": "SPI1_MISO",    "dir": "AF_IN",       "peripheral": "SPI1_MISO", "target": "J_SD DO"},
    "PA7":  {"net": "SPI1_MOSI",    "dir": "AF_OUT",      "peripheral": "SPI1_MOSI", "target": "J_SD DI"},
    "PA8":  {"net": "MCO_OUT",      "dir": "AF_OUT",      "peripheral": "RCC_MCO",   "target": "TP_MCO (8 MHz HSE check)"},
    "PA9":  {"net": "DBG_TX",       "dir": "AF_OUT",      "peripheral": "USART1_TX", "target": "J_UART TX (HIL / Debug)"},
    "PA10": {"net": "DBG_RX",       "dir": "AF_IN",       "peripheral": "USART1_RX", "target": "J_UART RX (HIL / Debug)"},
    "PA13": {"net": "SWDIO",        "dir": "SWD",         "peripheral": "SWD",       "target": "J_SWD Pin 2"},
    "PA14": {"net": "SWCLK",        "dir": "SWD",         "peripheral": "SWD",       "target": "J_SWD Pin 3"},
    "PB0":  {"net": "SD_PWR_EN",    "dir": "DIGITAL_OUT", "peripheral": "GPIOB.0",   "target": "Q_SD (AO3401) Gate Active-Low + TP_SD_EN"},
    "PB1":  {"net": "BAT_SENSE",    "dir": "ANALOG_IN",   "peripheral": "ADC1_IN9",  "target": "R_DIV1/R_DIV2 midpoint (VBAT/2)"},
    "PB5":  {"net": "BAT_SENSE_EN", "dir": "DIGITAL_OUT", "peripheral": "GPIOB.5",   "target": "Q_BAT (2N7002) Gate Active-High"},
    "PB6":  {"net": "I2C1_SCL",     "dir": "AF_OD",       "peripheral": "I2C1_SCL",  "target": "U_INA (INA219 @ 0x40) SCL"},
    "PB7":  {"net": "I2C1_SDA",     "dir": "AF_OD",       "peripheral": "I2C1_SDA",  "target": "U_INA (INA219 @ 0x40) SDA"},
    "PB12": {"net": "WAKE_BTN",     "dir": "DIGITAL_IN",  "peripheral": "EXTI12",    "target": "SW_WAKE (Active-Low, 100k pull-up, 100nF)"},
    "NRST": {"net": "NRST",         "dir": "RESET",       "peripheral": "SYS_RST",   "target": "SW_RST + J_SWD Pin 4 + C_RST 100nF"},
}

INTERACTIVE_CONTROLS = {
    "buttons":    ["SW_RST", "SW_WAKE"],
    "jumpers":    ["J_RLD", "J_IMEAS"],
    "connectors": ["J_ECG", "J_USB", "J_SD", "J_BAT", "J_SWD", "J_UART"],
    "indicators": ["LED_STAT"],
    "testpoints": ["TP_ECG_OUT", "TP_MCO", "TP_SD_EN"],
}


def parse_si_value(comment: str) -> Optional[float]:
    """Parse engineering values like '100K', '1.4M', '0.1R 1%', '0.33uF', '10pF C0G', '8MHz'."""
    s = comment.strip()
    if not s:
        return None
    # Match leading float + optional prefix + optional unit
    m = re.match(r"^([0-9]+(?:\.[0-9]+)?)\s*([pnuµmkMG]?)(?:(Hz|F|R|Ohm|Ω))?(?:\b|\s|$)", s, re.IGNORECASE)
    if not m:
        return None
    val = float(m.group(1))
    prefix = m.group(2)
    unit = (m.group(3) or "").lower()

    # Handle case-sensitive M (mega) vs m (milli) and k/K (kilo)
    scale_map = {
        "p": 1e-12,
        "n": 1e-9,
        "u": 1e-6,
        "µ": 1e-6,
        "m": 1e-3,
        "k": 1e3,
        "K": 1e3,
        "M": 1e6,
        "G": 1e9,
        "": 1.0,
    }
    if prefix == "m" and unit == "hz":
        # Guard against lowercase mhz typo if ever present
        scale = 1e6
    else:
        scale = scale_map.get(prefix, 1.0)
    return val * scale


def parse_pick_and_place(csv_path: Path) -> List[Dict[str, Any]]:
    """Parse Altium metric Pick-and-Place CSV into structured component list."""
    components: List[Dict[str, Any]] = []
    with csv_path.open("r", encoding="utf-8-sig", errors="replace") as f:
        lines = f.readlines()

    header_idx = None
    for i, line in enumerate(lines):
        if line.strip().startswith('"Designator"') or line.strip().startswith("Designator,"):
            header_idx = i
            break
    if header_idx is None:
        raise ValueError(f"Could not find CSV header row in {csv_path}")

    reader = csv.DictReader(lines[header_idx:])
    for row in reader:
        des = (row.get("Designator") or "").strip()
        if not des:
            continue
        comment = (row.get("Comment") or "").strip()
        components.append({
            "designator": des,
            "comment": comment,
            "numeric_value": parse_si_value(comment),
            "layer": (row.get("Layer") or "TopLayer").strip(),
            "footprint": (row.get("Footprint") or "").strip(),
            "x_mm": round(float(row["Center-X(mm)"]), 3),
            "y_mm": round(float(row["Center-Y(mm)"]), 3),
            "rotation_deg": round(float(row["Rotation"]), 1),
            "description": (row.get("Description") or "").strip(),
        })
    return components


def parse_bom_xlsx(xlsx_path: Path) -> List[Dict[str, Any]]:
    """Parse Altium exported BOM .xlsx using pure stdlib zipfile + ElementTree."""
    ns = {"a": "http://schemas.openxmlformats.org/spreadsheetml/2006/main"}
    with zipfile.ZipFile(xlsx_path, "r") as zf:
        shared_strings: List[str] = []
        if "xl/sharedStrings.xml" in zf.namelist():
            ss_root = ET.fromstring(zf.read("xl/sharedStrings.xml"))
            for si in ss_root.findall("a:si", ns):
                texts = [t.text or "" for t in si.findall(".//a:t", ns)]
                shared_strings.append("".join(texts))

        sh_root = ET.fromstring(zf.read("xl/worksheets/sheet1.xml"))
        rows: List[List[str]] = []
        for row_el in sh_root.findall(".//a:sheetData/a:row", ns):
            vals: List[str] = []
            for c_el in row_el.findall("a:c", ns):
                t_attr = c_el.attrib.get("t", "")
                v_el = c_el.find("a:v", ns)
                if t_attr == "s" and v_el is not None and v_el.text is not None:
                    vals.append(shared_strings[int(v_el.text)])
                elif v_el is not None and v_el.text is not None:
                    vals.append(v_el.text)
                else:
                    is_t = c_el.find("a:is/a:t", ns)
                    vals.append(is_t.text if (is_t is not None and is_t.text) else "")
            if any(v.strip() for v in vals):
                rows.append(vals)

    if len(rows) < 2:
        raise ValueError(f"BOM sheet empty in {xlsx_path}")

    headers = [h.strip() for h in rows[0]]
    items: List[Dict[str, Any]] = []
    for r in rows[1:]:
        rec = {headers[i]: (r[i].strip() if i < len(r) else "") for i in range(len(headers))}
        qty_raw = rec.get("Quantity", "0")
        qty = int(float(qty_raw)) if qty_raw else 0
        desigs = [d.strip() for d in rec.get("Designator", "").split(",") if d.strip()]
        items.append({
            "comment": rec.get("Comment", ""),
            "description": rec.get("Description", ""),
            "footprint": rec.get("Footprint", ""),
            "libref": rec.get("LibRef", ""),
            "designators": desigs,
            "quantity": qty,
        })
    return items


def parse_gm1_outline(gm1_path: Path) -> Tuple[float, float]:
    """Parse RS-274X Mechanical 1 Gerber (.GM1, metric 4:4) for board width & height in mm."""
    text = gm1_path.read_text(encoding="ascii", errors="ignore")
    xs = [int(m) / 10000.0 for m in re.findall(r"X(\d+)", text)]
    ys = [int(m) / 10000.0 for m in re.findall(r"Y(\d+)", text)]
    if not xs or not ys:
        raise ValueError(f"No outline coordinates found in {gm1_path}")
    return round(max(xs) - min(xs), 3), round(max(ys) - min(ys), 3)


def parse_drc_report(drc_path: Path) -> Dict[str, Any]:
    """Extract DRC verification status from Altium .drc report."""
    if not drc_path.exists():
        return {"present": False, "violations": None}
    text = drc_path.read_text(encoding="utf-8", errors="ignore")
    m_v = re.search(r"Violations Detected\s*:\s*(\d+)", text)
    m_d = re.search(r"Date\s*:\s*(.+)", text)
    m_t = re.search(r"Time\s*:\s*(.+)", text)
    return {
        "present": True,
        "violations": int(m_v.group(1)) if m_v else None,
        "date": m_d.group(1).strip() if m_d else "",
        "time": m_t.group(1).strip() if m_t else "",
    }


def count_drill_holes(gerber_raw_dir: Path) -> Dict[str, int]:
    """Count round PTH, round NPTH, and slot PTH holes from Excellon NC Drill files."""
    counts = {"pth_round": 0, "npth_round": 0, "pth_slot": 0}
    pth_f = gerber_raw_dir / "P05_ECG-RoundHoles-Plated.TXT"
    npth_f = gerber_raw_dir / "P05_ECG-RoundHoles-NonPlated.TXT"
    slot_f = gerber_raw_dir / "P05_ECG-SlotHoles-Plated.TXT"

    def _count_coords(p: Path) -> int:
        if not p.exists():
            return 0
        body = p.read_text(encoding="ascii", errors="ignore").split("%", 1)[-1]
        return sum(1 for ln in body.splitlines() if re.match(r"^[XY]\d+", ln.strip()))

    counts["pth_round"] = _count_coords(pth_f)
    counts["npth_round"] = _count_coords(npth_f)
    if slot_f.exists():
        body = slot_f.read_text(encoding="ascii", errors="ignore").split("%", 1)[-1]
        counts["pth_slot"] = sum(1 for ln in body.splitlines() if ln.strip() == "M15")
    counts["total"] = counts["pth_round"] + counts["npth_round"] + counts["pth_slot"]
    return counts


def extract_circuit_parameters(components: List[Dict[str, Any]]) -> Dict[str, Any]:
    """Derive key physical circuit parameters directly from component values on the PCB."""
    by_des = {c["designator"]: c for c in components}

    def _val(des: str, default: float) -> float:
        c = by_des.get(des)
        if c and c.get("numeric_value") is not None:
            return float(c["numeric_value"])
        return default

    r_shunt = _val("R_SHUNT", 0.1)
    r_div1 = _val("R_DIV1", 100e3)
    r_div2 = _val("R_DIV2", 100e3)
    r_prog = _val("R_PROG", 4.7e3)
    r_g1 = _val("R_G1", 1e6)
    r_g2 = _val("R_G2", 100e3)
    c_hp1 = _val("C_HP1", 0.33e-6)
    c_hp2 = _val("C_HP2", 0.33e-6)
    r_hp1 = _val("R_HP1", 10e6)
    r_hp2 = _val("R_HP2", 10e6)
    r_hp3 = _val("R_HP3", 1.4e6)
    r_lp1 = _val("R_LP1", 1e6)
    r_lp2 = _val("R_LP2", 1e6)
    c_lp1 = _val("C_LP1", 1.5e-9)
    c_lp2 = _val("C_LP2", 10e-9)
    c_sd1 = _val("C_SD1", 4.7e-6)
    f_hse = _val("Y1", 8e6)
    f_lse = _val("Y2", 32.768e3)

    # Derived AD8232 stage math (datasheet Eq. 1-4)
    afe_ia_gain = 100.0
    afe_opamp_gain = 1.0 + (r_g1 / r_g2)
    afe_total_gain = afe_ia_gain * afe_opamp_gain
    # Nominal 2-pole HPF fc ~ 1 / (2*pi*sqrt(R_HP1*C_HP1*R_HP2*C_HP2)) -> ~0.48 Hz (per AD8232 cardiac monitor ~0.5 Hz)
    import math
    afe_hpf_hz = 1.0 / (2.0 * math.pi * math.sqrt(r_hp1 * c_hp1 * r_hp2 * c_hp2)) * 10.0  # scaled by 100x IA/integrator feedback
    # AD8232 datasheet 2-pole HPF formula: fc = 10 / (2 * pi * sqrt(R1*C1*R2*C2)) -> ~0.482 Hz!
    afe_lpf_hz = 1.0 / (2.0 * math.pi * math.sqrt(r_lp1 * r_lp2 * c_lp1 * c_lp2))
    # MCP73831 datasheet: I_REG = 1000 V / R_PROG
    mcp73831_ichg_ma = (1000.0 / r_prog) * 1000.0

    return {
        "power": {
            "vbus_nominal_v": 5.0,
            "vbat_nominal_v": 3.7,
            "vbat_full_v": 4.2,
            "vbat_empty_v": 3.0,
            "battery_capacity_mah": 500.0,
            "battery_r_int_ohm": 0.12,
            "r_prog_ohm": r_prog,
            "mcp73831_ichg_ma": round(mcp73831_ichg_ma, 2),
            "ldo_vout_v": 3.3,
            "ldo_dropout_v": 0.178,
            "ldo_iq_ua": 1.6,
            "r_shunt_ohm": r_shunt,
            "ina219_i2c_addr": 0x40,
            "ina219_shunt_lsb_uv": 10.0,
            "ina219_current_resolution_ua": round((10e-6 / r_shunt) * 1e6, 2),
            "ina219_iq_active_ua": 700.0,
            "ina219_iq_powerdown_ua": 6.0,
            "r_div1_ohm": r_div1,
            "r_div2_ohm": r_div2,
            "bat_divider_ratio": round(r_div2 / (r_div1 + r_div2), 6),
            "c_sd1_f": c_sd1,
        },
        "afe_ad8232": {
            "ia_gain": afe_ia_gain,
            "r_g1_ohm": r_g1,
            "r_g2_ohm": r_g2,
            "opamp_gain": round(afe_opamp_gain, 3),
            "total_gain": round(afe_total_gain, 1),
            "c_hp1_f": c_hp1,
            "c_hp2_f": c_hp2,
            "r_hp1_ohm": r_hp1,
            "r_hp2_ohm": r_hp2,
            "r_hp3_ohm": r_hp3,
            "hpf_fc_hz": round(afe_hpf_hz, 3),
            "r_lp1_ohm": r_lp1,
            "r_lp2_ohm": r_lp2,
            "c_lp1_f": c_lp1,
            "c_lp2_f": c_lp2,
            "lpf_fc_hz": round(afe_lpf_hz, 2),
            "vref_ratio": 0.5,
            "iq_active_ua": 170.0,
            "iq_shutdown_ua": 0.5,
            "input_noise_uvpp": 14.0,
            "cmrr_active_rld_db": 80.0,
            "cmrr_vref_gnd_db": 42.0,
        },
        "mcu_stm32f103c8t6": {
            "flash_origin": "0x08000000",
            "flash_size_bytes": 65536,
            "sram_origin": "0x20000000",
            "sram_size_bytes": 20480,
            "hse_hz": f_hse,
            "lse_hz": f_lse,
            "adc_bits": 12,
            "adc_max": 4095,
            "i_stop_ua": 24.0,
            "i_sleep_8mhz_ma": 1.8,
            "i_run_8mhz_ma": 4.2,
            "i_run_72mhz_ma": 27.0,
        },
    }


def build_manifest(repo_root: Path = DEFAULT_REPO_ROOT, output_path: Path = DEFAULT_MANIFEST_PATH) -> Dict[str, Any]:
    """Build and write `board_manifest.json` from verified Gate G2 hardware artifacts."""
    pnp_path = repo_root / "hardware" / "bom" / "P05_ECG_PickAndPlace.csv"
    bom_path = repo_root / "hardware" / "bom" / "P05_ECG_BOM.xlsx"
    gerber_raw = repo_root / "hardware" / "gerber" / "raw"
    gm1_path = gerber_raw / "P05_ECG.GM1"
    drc_path = repo_root / "hardware" / "gerber" / "Design Rule Check - P05_ECG.drc"

    components = parse_pick_and_place(pnp_path)
    bom_items = parse_bom_xlsx(bom_path)
    width_mm, height_mm = parse_gm1_outline(gm1_path)
    drc = parse_drc_report(drc_path)
    drills = count_drill_holes(gerber_raw)
    params = extract_circuit_parameters(components)

    by_des = {c["designator"]: c for c in components}
    controls_geo: Dict[str, List[Dict[str, Any]]] = {}
    for group, names in INTERACTIVE_CONTROLS.items():
        controls_geo[group] = [
            {
                "designator": name,
                "x_mm": by_des[name]["x_mm"],
                "y_mm": by_des[name]["y_mm"],
                "rotation_deg": by_des[name]["rotation_deg"],
                "footprint": by_des[name]["footprint"],
            }
            for name in names
            if name in by_des
        ]

    bom_total_qty = sum(item["quantity"] for item in bom_items)
    if bom_total_qty != len(components):
        raise ValueError(f"Mismatch between BOM qty ({bom_total_qty}) and Pick-and-Place ({len(components)})")

    manifest: Dict[str, Any] = {
        "schema_version": "1.0",
        "project": "P05 Single-Lead ECG & On-Device HRV Monitor",
        "board": {
            "width_mm": width_mm,
            "height_mm": height_mm,
            "layers": 2,
            "component_count": len(components),
            "unique_bom_lines": len(bom_items),
            "drills": drills,
            "drc": drc,
            "render_images": {
                "top": "docs/images/pcb_top.png",
                "bottom": "docs/images/pcb_bottom.png",
            },
        },
        "pinout": STM32_PINOUT,
        "interactive_controls": controls_geo,
        "circuit_parameters": params,
        "components": components,
        "bom": bom_items,
    }

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")
    return manifest


if __name__ == "__main__":
    m = build_manifest()
    b = m["board"]
    p = m["circuit_parameters"]
    print(f"[P05 Board Bundle] Generated: {DEFAULT_MANIFEST_PATH}")
    print(f"  Board     : {b['width_mm']} x {b['height_mm']} mm | {b['layers']} layers | DRC violations = {b['drc']['violations']}")
    print(f"  Parts     : {b['component_count']} components ({b['unique_bom_lines']} BOM lines) | Drills = {b['drills']['total']}")
    print(f"  AD8232    : Gain = {p['afe_ad8232']['total_gain']}x | HPF = {p['afe_ad8232']['hpf_fc_hz']} Hz | LPF = {p['afe_ad8232']['lpf_fc_hz']} Hz")
    print(f"  Power     : R_SHUNT = {p['power']['r_shunt_ohm']} Ohm (INA219 LSB = {p['power']['ina219_current_resolution_ua']} uA) | MCP73831 I_chg = {p['power']['mcp73831_ichg_ma']} mA")
