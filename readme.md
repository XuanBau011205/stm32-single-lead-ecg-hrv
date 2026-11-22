# P05 — Battery-Powered Single-Lead ECG & On-Device Fixed-Point HRV Monitor

[![MCU](https://img.shields.io/badge/MCU-STM32F103C8T6-03234B?logo=stmicroelectronics)](docs/SPECIFICATION.md)
[![AFE](https://img.shields.io/badge/AFE-AD8232%20LFCSP--20-0067B9)](docs/SPECIFICATION.md)
[![PCB](https://img.shields.io/badge/PCB-2--Layer%2080x55mm-2E7D32)](hardware/P05_ECG.PcbDoc)
[![ERC](https://img.shields.io/badge/Schematic%20ERC-0%20Errors%20%7C%200%20Warnings-brightgreen)](hardware/P05_ECG/P05_ECG.SchDoc)
[![DRC](https://img.shields.io/badge/PCB%20DRC-0%20Violations-brightgreen)](hardware/gerber/Design%20Rule%20Check%20-%20P05_ECG.drc)
[![Stop Current](https://img.shields.io/badge/Stop%20Mode%20Target-32.1%20%C2%B5A-blue)](docs/SPECIFICATION.md)

A battery-powered, electrically safe **single-lead ECG acquisition & real-time HRV analysis device** built on **STM32F103C8T6** (ARM Cortex-M3 @ 72 MHz, no FPU) and **AD8232** minimalist biomedical AFE. Designed for zero-heap **Q15 fixed-point DSP**, parabolic sub-sample R-peak interpolation, automated **MIT-BIH Hardware-in-the-Loop (HIL)** verification, and ultra-low-power battery operation.

---

## PCB Layout Showcase (JLCPCB Gerber Render — `80.0 × 55.0 mm`, 2-Layer)

| **Top Layer (4-Zone Partitioned Layout)** | **Bottom Layer (Solid GND Plane >90% + 125 Stitching Vias)** |
| :---: | :---: |
| ![P05 ECG Top Layer](docs/images/pcb_top.png) | ![P05 ECG Bottom Layer](docs/images/pcb_bottom.png) |

---

## Key Engineering Highlights

### 1. Hardware & Mixed-Signal 2-Layer PCB (`ERC = 0`, `DRC = 0 Violations`)
- **Strict Spatial Partitioning (`80.0 × 55.0 mm`, 91 components):** Analog AFE (`AD8232`, `BAV199` ESD, `J_ECG`, `J_RLD`) strictly isolated on the left (`X < 28 mm`); MCU, 8 MHz HSE / 32.768 kHz LSE crystals, SPI microSD, and Power/Charger strictly placed on the right (`X > 31 mm`).
- **EMC / Grounding Strategy:** **Single Solid Bottom GND Plane (>90% unbroken)** with **125 GND stitching vias** and **zero high-speed digital traces** intruding into the analog AFE zone — preventing return-current slot antennas while isolating `VDDA` via Ferrite Bead `FB1 (600 Ω @ 100 MHz)` + `1 µF / 100 nF / 10 nF` decoupling.
- **Analog & Clock Integrity:** Symmetric differential ECG input routing (`0` vias), selectable **Active RLD vs. `VREF_1V65` Grounding** via 3-pin jumper `J_RLD` (for CMRR ablation), and planar 0-crossing routing for `Y1 (8 MHz)` and `Y2 (32.768 kHz)` over solid ground.
- **Ultra-Low-Power Power Tree (`32.1 µA` Stop Mode target):**
  - 1S Li-ion charging via `USB-C (5V-only)` → `MCP73831` (`R_PROG = 4.7 kΩ`) + `SMAJ5.0A` TVS.
  - Ultra-low-Iq `MCP1700-3.3V` (`1.6 µA`) → `J_IMEAS` (Nordic PPK2 measurement header) → `0.1 Ω` shunt + `INA219` (`I2C1`) dynamic current monitor.
  - High-side P-MOSFET **`AO3401`** (`PB0`) completely cuts `3V3_SD` to microSD in sleep (`0 µA` leak); low-side N-MOSFET **`2N7002`** (`PB5`) disconnects the `100 kΩ / 100 kΩ` battery voltage divider when idle.
- **EDA Design Automation:** Custom Altium DelphiScripts ([AutoPlace_P05.pas](hardware/P05_ECG/AutoPlace_P05.pas), [RouteCritical_P05.pas](hardware/P05_ECG/RouteCritical_P05.pas), [FixDRC_P05.pas](hardware/P05_ECG/FixDRC_P05.pas)) for mathematically verified zero-collision placement, critical analog/crystal pre-routing, and silkscreen/DRC cleanup.

### 2. System Architecture

```text
[Li-ion 1S 500mAh] ◄── [USB-C 5V + SMAJ5.0A + MCP73831]
        │
        ▼
[MCP1700-3.3V] ──► [J_IMEAS (PPK2)] ──► [0.1Ω Shunt + INA219 (I2C1)]
                                                  │
            ┌─────────────────────────────────────┴──────────────────────────┐
            ▼ (3V3_MAIN)                                                     ▼ (VDDA via FB1 + LC)
┌───────────────────────────┐                                    ┌──────────────────────────┐
│  STM32F103C8T6 (LQFP-48)  │◄──── PA0 (ADC1_IN0) ───────────────┤  AD8232 AFE (LFCSP-20)   │
│  • 8MHz HSE + 32.768kHz   │◄──── PA1, PA2 (LOD+, LOD-) ────────┤  • BAV199 ESD Protection │
│  • Zero-Heap Q15 DSP      │───── PA3 (ECG_SDN) ───────────────►│  • J_RLD (Active / VREF) │
│  • Ping-Pong DMA + DWT    │                                    └──────────────────────────┘
└──┬──────────┬──────────┬──┘
   │          │          └──► PB0 (AO3401 Power-Gate) ──► [microSD TF-01A (SPI1, 512B Blocks)]
   │          └─────────────► PA9, PA10 (UART1 HIL)   ──► [MIT-BIH Automated Testbench]
   └────────────────────────► PA13, PA14, NRST (SWD) + PB12 (SW_WAKE EXTI12) + PA8 (TP_MCO)
```

---

## Target Acceptance KPIs & Power Budget

| Metric | Acceptance Target (Gate G4) | Verification Method |
| :--- | :--- | :--- |
| **Input-Referred Noise** | `<= 30 µVpp` (`0.5–40 Hz`) | Function Generator + 1000:1 Attenuator + Oscilloscope |
| **R-Peak Detection (`Se`, `PPV`)** | `>= 99%` on selected records | MIT-BIH Arrhythmia Database via UART1 HIL ([hil_mitbih_runner.py](scripts/hil_mitbih_runner.py)) |
| **RR Interval Error (`MAE_RR`)** | `<= 5 ms` (sub-sample parabolic) | Polar H10 reference + Bland–Altman (`LoA 95% HR within ±2 bpm`) |
| **Stop Mode Current** | `<= 48.15 µA` (`32.1 µA` typical) | Nordic PPK2 via `J_IMEAS` (MCU `24 µA` + LDO `1.6 µA` + AFE `0.5 µA` + INA219 PD `6 µA` + SD `0 µA`) |
| **Active Battery Life (500 mAh)** | `~52.6 h` (`8 MHz` Q15 + Sleep) vs `~13.1 h` (`72 MHz` Soft-Float) | Ablation 1 (`DWT->CYCCNT` + PPK2 profiling) |

---

## Project Deliverables & Quick Links

| Gate | Milestone | Status | Key Artifacts |
| :---: | :--- | :---: | :--- |
| **G1** | Schematic, Library & Power Budget | ✅ **Completed** | [P05_ECG.SchDoc](hardware/P05_ECG/P05_ECG.SchDoc) (`ERC = 0`), [SPECIFICATION.md](docs/SPECIFICATION.md) |
| **G2** | 2-Layer PCB Layout & Fabrication Pack | ✅ **Completed** | [P05_ECG.PcbDoc](hardware/P05_ECG.PcbDoc) (`DRC = 0`), [Gerber + Drill ZIP](hardware/gerber/P05_ECG_Gerber_JLCPCB.zip), [BOM (.xlsx)](hardware/bom/P05_ECG_BOM.xlsx), [Pick & Place (.csv)](hardware/bom/P05_ECG_PickAndPlace.csv), [DRC Report](hardware/gerber/Design%20Rule%20Check%20-%20P05_ECG.drc) |
| **Sim** | Python Virtual Hardware Twin (`P05-Sim`) | 📐 **Architected** | [SIM_ARCHITECTURE.md](docs/SIM_ARCHITECTURE.md) *(Unicorn ARM Cortex-M3 + Fast SIL + PySide6 Virtual Lab)* |
| **G3** | PCB Assembly, Bring-up & Driver Bring-up | ⏳ **Next** | [G3_bringup.md](docs/G3_bringup.md), [firmware/](firmware/CMakeLists.txt) |
| **G4** | MIT-BIH HIL, 3 Ablations & Paper | ⬜ **Planned** | [G4_acceptance_paper.md](docs/G4_acceptance_paper.md), [scripts/](scripts/) |

---

## Repository Structure

```text
├── hardware/
│   ├── P05_ECG/                 # Altium project (.PrjPcb, .SchDoc, .SchLib, .PcbLib, DelphiScripts)
│   ├── P05_ECG.PcbDoc           # Verified 2-layer 80x55mm PCB (DRC = 0 Violations)
│   ├── gerber/                  # JLCPCB-ready Gerber RS-274X + NC Drill (PTH/NPTH/Slots) ZIP & DRC report
│   ├── bom/                     # Official BOM (43 line items / 91 parts) & Pick-and-Place CSV
│   └── DataSheet/               # Reference datasheets for all ICs & connectors
├── firmware/                    # STM32F103C8T6 CMake zero-heap firmware (Core/, DSP/, HIL/)
├── scripts/                     # MIT-BIH HIL runner, Bland-Altman evaluator, power profiler
└── docs/                        # Specification, Gate G1–G4 checklists, SIM_ARCHITECTURE.md & PCB renders
```

---

<p align="center">
  <img src="qr_code.png" alt="Embedded Systems Portfolio QR Code" width="130" /><br/>
  <sub><b>Scan QR to view full Portfolio:</b> <a href="https://github.com/XuanBau011205/bau-portfolio">https://github.com/XuanBau011205/bau-portfolio</a></sub>
</p>
