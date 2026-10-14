#!/usr/bin/env python3
"""
Hardware-in-the-Loop (HIL) MIT-BIH Testbench Runner.

Mục đích:
- Đọc các bản ghi điện tim chuẩn từ cơ sở dữ liệu MIT-BIH Arrhythmia Database (sử dụng thư viện wfdb).
- Bơm từng mẫu ECG tuần tự qua giao tiếp UART tới vi điều khiển STM32F103C8T6 theo thời gian thực (250/500 Hz).
- Thu nhận thời điểm phát hiện đỉnh R được MCU gửi phản hồi về qua UART.
- Đối chiếu vị trí đỉnh R tìm được với nhãn chuẩn (ground-truth annotation: file .atr).
- Tính toán và đánh giá các chỉ số nghiệm thu:
    * Độ nhạy (Sensitivity): Se = TP / (TP + FN)
    * Giá trị dự đoán dương (Positive Predictive Value): PPV = TP / (TP + FP)
    * Cửa sổ dung sai thời gian phát hiện mặc định: +/- 50 ms.
"""

import argparse
import sys


def parse_args():
    parser = argparse.ArgumentParser(
        description="P05 HIL MIT-BIH Runner: Stream ECG records via UART and evaluate Se/PPV."
    )
    parser.add_argument(
        "--port",
        type=str,
        default="COM3",
        help="Serial port connected to STM32 UART1 header (e.g., COM3, /dev/ttyUSB0).",
    )
    parser.add_argument(
        "--baud",
        type=int,
        default=115200,
        help="UART Baudrate (e.g., 115200 or 921600).",
    )
    parser.add_argument(
        "--record",
        type=str,
        default="100",
        help="MIT-BIH record identifier (e.g., '100', '101', '200').",
    )
    parser.add_argument(
        "--fs",
        type=int,
        default=250,
        help="Target streaming sample rate in Hz (default: 250).",
    )
    parser.add_argument(
        "--tolerance-ms",
        type=float,
        default=50.0,
        help="Acceptance window around ground-truth R-peak in milliseconds (default: 50 ms).",
    )
    parser.add_argument(
        "--output-csv",
        type=str,
        default="data/test/hil_results_100.csv",
        help="Output CSV path for performance evaluation results.",
    )
    return parser.parse_args()


def main():
    """Main HIL streaming and Se/PPV evaluation loop."""
    args = parse_args()
    print(f"[P05 HIL Runner] Starting HIL test on record {args.record} via {args.port} @ {args.baud} baud...")

    # TODO: 1. Load record and annotations using wfdb.rdrecord and wfdb.rdann
    # TODO: 2. Resample ECG signal to target sampling rate (args.fs) if different from 360 Hz
    # TODO: 3. Open serial port using pyserial
    # TODO: 4. Stream samples into STM32 using HIL packet protocol (0xAA header)
    # TODO: 5. Collect detection timestamps emitted from STM32
    # TODO: 6. Match detected peaks against ground truth within +/- args.tolerance_ms
    # TODO: 7. Compute TP, FP, FN, Se (%), PPV (%)
    # TODO: 8. Save evaluation report to args.output_csv

    print("[P05 HIL Runner] Scaffold completed. Implementation pending.")


if __name__ == "__main__":
    main()
