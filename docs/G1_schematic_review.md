# Cổng Kiểm tra G1: Duyệt Nguyên lý (Schematic Review)

- **Mã đề tài:** P05
- **Thời điểm:** Tuần 2
- **Trọng tâm:** Kiểm tra tính đúng đắn về nguyên lý điện tử tử mạch tương tự AFE AD8232, bộ lọc thụ động, phân bổ nguồn vi điều khiển STM32F103C8T6, bảo vệ ESD và ngân sách công suất.

## Checklist Sản phẩm Bàn giao (Deliverables Checklist)

- [ ] **1. Sơ đồ khối hệ thống (System Block Diagram):**
  - Tách biệt rõ 2 khối vận hành: (1) Đo trên người dùng pin Li-ion 1S + MicroSD; (2) HIL Test qua UART khi tháo điện cực.
  - Phân vùng nguồn số (`3V3_MAIN`) và nguồn tương tự (`VDDA`) có lọc qua Ferrite Bead + tụ giải ghép.
- [ ] **2. Thiết kế Nguyên lý Schematic hoàn chỉnh:**
  - Bản vẽ Altium Designer (`.SchDoc`) đầy đủ chú thích net label, testpoint (`TP_ECG_OUT`, `TP_SD_EN`, `TP_MCO`, `J_IMEAS`).
  - Chạy Electrical Rules Check (ERC) đạt chuẩn: **`ERC = 0 Errors, 0 Warnings`**.
- [ ] **3. Báo cáo Ngân sách Công suất (Power Budget - Gate G1):**
  - Dòng tĩnh chế độ ngủ sâu (Stop Mode) theo linh kiện (STM32, MCP1700, AD8232, INA219, MicroSD).
  - Xác nhận ngưỡng dòng tĩnh cho phép: $\le 1{,}5 \times (I_{\mathrm{MCU\,Stop}} + \sum I_q) \approx 48{,}15\ \mu\text{A}$.
  - Ước tính thời lượng pin 500 mAh ở Chế độ Cơ sở ($\approx 13{,}1$ giờ) và Chế độ Tối ưu ($\approx 52{,}6$ giờ).
- [ ] **4. Kế hoạch Thiết bị Đo chuẩn & Đối chiếu:**
  - Danh sách thiết bị: Nordic PPK2 (đo dòng tĩnh/động), Máy phát hàm + Cầu suy hao 1000:1 (đo nhiễu đầu vào), Đai Polar H10 (chuẩn RR/HR đối chiếu).
