# Cổng Kiểm tra G2: Duyệt Layout & Đặt mạch (Layout & Ordering)

- **Mã đề tài:** P05
- **Thời điểm:** Tuần 3
- **Trọng tâm:** Kiểm tra thiết kế mạch in PCB 2 lớp, toàn vẹn tín hiệu, tiếp đất analog/digital, xuất dữ liệu chế tạo và đặt hàng bo mạch + linh kiện.

## Checklist Sản phẩm Bàn giao (Deliverables Checklist)

- [ ] **1. Bản vẽ Bố trí Mạch in PCB 2 lớp (PCB Layout):**
  - Tách biệt vùng tương tự (AFE AD8232) và vùng số (MCU, Clock HSE/LSE, SPI MicroSD).
  - Định tuyến tín hiệu vi sai điện cực ECG đối xứng, bao bọc bởi guard ring.
  - Vòng bảo vệ thạch anh LSE 32.768 kHz và HSE 8.000 MHz cách ly nhiễu xung số.
  - Chạy Design Rule Check (DRC) đạt chuẩn xưởng gia công: **`DRC = 0 Violations`**.
- [ ] **2. Biên bản Review chéo Thiết kế (Phụ lục A):**
  - SV2 (Firmware) và SV3 (Đo đạc) kiểm tra chéo vị trí các test point, header nạp SWD, header UART.
  - Xác nhận khoảng cách chân linh kiện SMD footprint chuẩn IPC-7351.
- [ ] **3. Bộ Dữ liệu Gia công Đầy đủ (Gerber & BOM):**
  - Tập tin Gerber chuẩn RS-274X, Drill file (NC Drill), file tọa độ gắp linh kiện (Pick-and-Place / Centroid).
  - Bảng kê BOM định dạng Excel/CSV đầy đủ mã nhà sản xuất (MPN), nhà cung cấp (LCSC/DigiKey) và linh kiện dự phòng.
- [ ] **4. Đặt Hàng PCB & Stencil:**
  - Hoàn tất gửi đơn đặt mạch in PCB 2 lớp (FR4, ENIG hoặc HASL Lead-Free, độ dày 1.2 mm hoặc 1.6 mm) và mặt nạ kem hàn (SMD Stencil).
