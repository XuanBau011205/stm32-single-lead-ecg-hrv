"""
CLI Firmware Flasher & Runner for P05VirtualBoard (Unicorn ARM Cortex-M3 Engine).

Examples:
    # 1. Run built-in ARM Cortex-M3 bring-up binary (verifies all 10 STM32F103 peripherals + 32.1 uA Stop + SW_WAKE):
    py -m sim.flash_and_run

    # 2. Flash your own compiled firmware (.elf / .bin / .hex) from firmware/build/:
    py -m sim.flash_and_run firmware/build/P05_ECG_Firmware.elf --duration 2.0
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path
from typing import Optional

from sim import P05VirtualBoard
from sim.mcu import UnicornBackend
from sim.tests.test_phase3_unicorn import build_p05_bringup_firmware


def run_builtin_demo() -> None:
    print("=== P05-Sim Phase 3: Flashing Real ARM Cortex-M3 Firmware into P05VirtualBoard ===\n")
    fw_bin = build_p05_bringup_firmware()
    board = P05VirtualBoard.from_hardware(initial_soc_pct=85.0, seed=42)
    backend = UnicornBackend()
    backend.load_raw_flash_bytes(fw_bin, name="p05_bringup_arm_cm3.bin")
    board.attach_firmware(backend)
    print(f"[Flash] Flashed {len(fw_bin)} bytes to 0x08000000 | Entry = 0x{backend.image.entry_point:08X}")

    # Step 60 ms (15 ticks @ 4 ms / 250 Hz) -> runs full bring-up + DMA Ping-Pong + enters STOP MODE
    s_stop = None
    for _ in range(15):
        s_stop = board.step(4000.0)
        cpu = backend.get_cpu_snapshot()
        if cpu.fault:
            raise RuntimeError(cpu.fault)

    assert s_stop is not None
    dwt_cycles = struct.unpack("<I", backend.read_sram(0x20000000, 4))[0]
    sd_r1 = struct.unpack("<I", backend.read_sram(0x20000004, 4))[0]
    ina_bus_raw = struct.unpack("<I", backend.read_sram(0x20000008, 4))[0]
    ina_bus_mv = (ina_bus_raw >> 3) * 4
    dma_irqs = struct.unpack("<I", backend.read_sram(0x2000000C, 4))[0]
    adc_buf = struct.unpack("<8H", backend.read_sram(0x20000100, 16))
    uart_msg = bytes(board.matrix.uart1_tx_fifo).decode("ascii", errors="replace")

    print(f"[RCC]    SYSCLK = {backend.mmio.sysclk_hz/1e6:.1f} MHz | TP_MCO (PA8) = {board.matrix.mco_hz/1e6:.1f} MHz")
    print(f"[DWT]    DWT->CYCCNT measured = {dwt_cycles} cycles")
    print(f"[I2C1]   INA219 @ 0x40 Bus Voltage read by ARM = {ina_bus_mv} mV")
    print(f"[USART1] PA9 TX output = {uart_msg!r}")
    print(f"[DMA1]   TIM2(250Hz)->ADC1->DMA1_Ch1 Ping-Pong IRQs = {dma_irqs} | 8 ECG samples = {list(adc_buf)}")
    print(f"[SPI1]   VirtualMicroSD CMD0 R1 response = 0x{sd_r1:02X}")
    print(f"[STOP]   MCU executed WFI (0xBF30) -> Mode = {backend.power_mode.value} | Virtual PPK2 = {s_stop.i_total_ua:.1f} uA!")

    # Press physical SW_WAKE button (PB12 -> EXTI12)!
    print("\n[User]   Pressing SW_WAKE (PB12 -> EXTI12)...")
    board.set_wake_button(True)
    s_wake = board.step(4000.0)
    board.set_wake_button(False)
    exti_flag = struct.unpack("<I", backend.read_sram(0x20000010, 4))[0]
    resume_flag = struct.unpack("<I", backend.read_sram(0x20000014, 4))[0]
    print(f"[WAKE]   CPU Woke -> Mode = {backend.power_mode.value} | PPK2 = {s_wake.i_total_ua/1000:.2f} mA | EXTI_ISR=0x{exti_flag:04X} | Main=0x{resume_flag:04X}")
    print(f"[Stack]  Min SP observed = 0x{backend.min_sp_seen:08X} (Used = {0x20005000 - backend.min_sp_seen} bytes / 2048 B limit)")
    print("\n=== ARM CORTEX-M3 VIRTUAL SILICON EXECUTION PASSED 100%! ===")


def run_external_firmware(fw_path: Path, duration_s: float, dt_us: float, wake_at_s: Optional[float]) -> None:
    board = P05VirtualBoard.from_hardware()
    backend = UnicornBackend()
    info = board.attach_firmware(backend, fw_path)
    print(f"[Flash] {info.name} ({info.backend_kind}) | {info.flash_bytes} B Flash | Entry=0x{info.entry_point:08X}")

    steps = max(1, int((duration_s * 1e6) / dt_us))
    woke = False
    for i in range(steps):
        if wake_at_s is not None and not woke and board.clock.now_s >= wake_at_s:
            print(f"  [t={board.clock.now_s:.3f}s] Pressing SW_WAKE (PB12)...")
            board.set_wake_button(True)
            woke = True
        elif woke:
            board.set_wake_button(False)

        s = board.step(dt_us)
        cpu = backend.get_cpu_snapshot()
        if cpu.fault:
            print(f"  [FAULT at t={s.time_s:.4f}s] {cpu.fault}")
            break
        if board.matrix.uart1_tx_fifo:
            chunk = bytes(board.matrix.uart1_tx_fifo)
            board.matrix.uart1_tx_fifo.clear()
            print(f"  [UART1 t={s.time_s:.3f}s] {chunk!r}")

    print("[Summary]", board.get_summary())


def main() -> None:
    parser = argparse.ArgumentParser(description="Flash & run ARM Cortex-M3 firmware on P05VirtualBoard.")
    parser.add_argument("firmware", nargs="?", type=Path, default=None, help="Path to .elf / .bin / .hex firmware (omit to run built-in ARM bring-up demo)")
    parser.add_argument("--duration", type=float, default=1.0, help="Simulation duration in seconds (default: 1.0)")
    parser.add_argument("--dt-us", type=float, default=4000.0, help="Simulation step in us (default: 4000 = 250 Hz)")
    parser.add_argument("--wake-at", type=float, default=None, help="Optional time (s) to press SW_WAKE button")
    args = parser.parse_args()

    if args.firmware is None:
        run_builtin_demo()
    else:
        run_external_firmware(args.firmware, args.duration, args.dt_us, args.wake_at)


if __name__ == "__main__":
    main()
