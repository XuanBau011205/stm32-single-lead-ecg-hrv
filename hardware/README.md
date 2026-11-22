# Hardware Design Guidelines & Library Conventions (IPC-7351)

Dự án P05 sử dụng Altium Designer để thiết kế phần cứng cho thiết bị đo ECG 1 đạo trình (AFE AD8232 + STM32F103C8T6).

## 1. Cấu trúc Thư mục

- `sch/`: Bản vẽ nguyên lý Schematic (`.SchDoc`, `.PrjPcb`).
- `pcb/`: Bản vẽ bố trí mạch in PCB Layout (`.PcbDoc`).
- `lib/`: Thư viện nguyên lý (`.SchLib`) và thư viện chân cắm (`.PcbLib`, 3D STEP models).
- `gerber/`: Dữ liệu gia công mạch in chuẩn RS-274X, Drill files (NC Drill).
- `bom/`: Bảng kê nguyên vật liệu Bill of Materials (`.xlsx`, `.csv`).

## 2. Quy ước Đặt tên Footprint theo Chuẩn IPC-7351

Tất cả footprint linh kiện dán (SMD) trong `lib/` bắt buộc tuân thủ chuẩn đặt tên IPC-7351 nhằm đảm bảo tính nhất quán và tương thích khi gia công tự động:

### 2.1. Cấu trúc Tên chung
`[Loại linh kiện][Kích thước Dài x Rộng X Chiều cao][Mức mật độ]`

- **Mức mật độ (Density Level):**
  - `M` (Most / Level A): Mật độ thấp, pad chì rộng, thuận tiện cho hàn tay prototype.
  - `N` (Nominal / Level B): Mật độ tiêu chuẩn, phù hợp hàn lò Reflow công nghiệp.
  - `L` (Least / Level C): Mật độ cao, tiết diện pad tối thiểu cho thiết bị di động siêu nhỏ gọn.
  - *Mặc định dự án P05:* Sử dụng hậu tố **`N`** (Nominal) hoặc **`M`** cho các điểm test/bring-up.

### 2.2. Ví dụ Đặt tên Cụ thể
- **Điện trở SMD (Chip Resistors):**
  - Format: `RESC[Chiều dài][Chiều rộng]X[Chiều cao][Mức]` (đơn vị: 0.1 mm)
  - Ví dụ chuẩn 0603 Metric 1608: `RESC1608X55N` (hoặc `RES_0603_1608_N`)
- **Tụ điện SMD (Chip Capacitors):**
  - Format: `CAPC[Chiều dài][Chiều rộng]X[Chiều cao][Mức]`
  - Ví dụ chuẩn 0603 Metric 1608: `CAPC1608X90N` (hoặc `CAP_0603_1608_N`)
  - Ví dụ chuẩn 0805 Metric 2012: `CAPC2012X140N`
- **IC & Vi điều khiển:**
  - `STM32F103C8T6`: `QFP50P900X900X160-48N` (LQFP48, bước chân 0.5 mm, 7x7 mm body)
  - `AD8232ACPZ`: `LFCSP-WQ-20` (4x4 mm, 20 chân, exposed pad)
  - `MCP1700T-3302E/TT`: `SOT23-3`
  - `AO3401A` (P-MOSFET): `SOT23-3`
  - `INA219AIDR`: `SOIC-8`

### 2.3. Quy ước Hướng Chân 1 (Pin 1 Orientation) & 3D Model
- Chân 1 của IC dạng QFP, QFN luôn đặt ở góc trên bên trái (Top-Left) theo chuẩn IPC-7351B.
- Dấu chấm đánh dấu Pin 1 trên lớp Silkscreen phải rõ ràng, không bị pad chì đè lên.
- Mỗi linh kiện trong `.PcbLib` bắt buộc phải có gắn mô hình 3D STEP chính xác chiều cao để kiểm tra va chạm cơ khí (clearance check).
