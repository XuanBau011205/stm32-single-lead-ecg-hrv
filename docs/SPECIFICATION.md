# P05: A Battery-Powered Single-Lead ECG with On-Device Fixed-Point R-Peak Detection and HRV Analysis

> **Đề tài thực hành – nghiên cứu (Mã đề tài: P05)**  
> **Đơn vị:** Trường Điện – Điện tử, Đại học Bách khoa Hà Nội (HUST)  
> **Vi điều khiển lõi:** STM32F103C8T6 (ARM Cortex-M3 @ 72 MHz, không FPU, 64 KB Flash, 20 KB SRAM)  
> **Triết lý thiết kế:** Tối giản hóa phần cứng tương tự (Minimalist AFE) — Dồn trọng tâm xử lý tín hiệu số dấu phẩy tĩnh (`Q15` DSP), nội suy dưới mức mẫu và kiểm thử tự động Hardware-in-the-Loop (HIL).

---

## 1. Tổng quan & Mục tiêu Nghiệm thu (KPIs)

Dự án thiết kế một thiết bị ghi điện tim 1 đạo trình chạy pin Li-ion an toàn tuyệt đối về điện, thực hiện phát hiện đỉnh R theo thời gian thực và tính toán các chỉ số biến thiên nhịp tim (HRV) bằng toán học số nguyên trực tiếp trên vi điều khiển không có bộ xử lý dấu phẩy động (FPU).

| Hạng mục đánh giá | Chỉ tiêu nghiệm thu (G4) | Thiết bị / Tập dữ liệu chuẩn đối chiếu |
| :--- | :--- | :--- |
| **Nhiễu quy về đầu vào (Input-referred noise)** | $\le 30\ \mu\text{V}_{\text{pp}}$ (băng tần $0,5–40\ \text{Hz}$) | Máy phát hàm + Cầu suy hao $1000:1$ + Oscilloscope |
| **Độ nhạy ($\text{Se}$) & Giá trị dự đoán dương ($\text{PPV}$)** | $\ge 99\%$ trên các bản ghi chọn lọc | Bộ dữ liệu chuẩn **MIT-BIH Arrhythmia Database** (nạp qua UART) |
| **Sai số khoảng cách đỉnh R ($\text{MAE}_{RR}$)** | $\le 5\ \text{ms}$ | Đai ngực **Polar H10** / Máy ECG chuẩn ($\ge 15$ tình nguyện viên) |
| **Giới hạn đồng thuận nhịp tim (LoA 95% HR)** | Nằm trong khoảng $\pm 2\ \text{bpm}$ | Phân tích **Bland–Altman** (5 phút nghỉ + 5 phút sau vận động) |
| **Dòng ngủ toàn mạch (Stop Mode)** | $\le 1{,}5 \times (I_{\mathrm{MCU\,Stop}} + \sum I_q) \approx 48{,}15\ \mu\text{A}$ | **Nordic PPK2** chèn qua jumper `J_IMEAS` (IC `INA219` trên bo có độ phân giải $100\ \mu\text{A}$ với trở shunt $0{,}1\ \Omega$ nên không đo được mức $\mu\text{A}$, chỉ dùng đo dòng động khi hoạt động) |

---

## 2. Kiến trúc Phần cứng Tối giản (Minimalist Hardware Architecture)

Hệ thống loại bỏ hoàn toàn các khối ngoại vi rườm rà (màn hình hiển thị, mạch lọc Notch 50 Hz phần cứng, IC cách ly USB đắt tiền) bằng cách tách bạch hai chế độ vận hành: **(1) Chế độ đo trên người** chạy 100% bằng pin Li-ion và ghi thẻ microSD (ngắt hoàn toàn kết nối dây với máy tính để đảm bảo an toàn điện y sinh), và **(2) Chế độ kiểm thử HIL / Đọc dữ liệu** qua cổng UART khi đã tháo điện cực.

```text
[Pin Li-ion 1S] ──► [IC Sạc MCP73831] (Cổng USB-C chỉ lấy 5V sạc, KHÔNG nối D+/D-)
       │
       ▼
[LDO 3.3V MCP1700] ──► [Jumper J_IMEAS] ──► [Shunt 0.1Ω + INA219 (I2C1)]
                                                     │
                ┌────────────────────────────────────┴───────────────────────┐
                ▼ (Rail 3V3_MAIN)                                            ▼ (Qua Ferrite Bead + 1µF + 10nF)
   ┌─────────────────────────┐                                     ┌──────────────────────┐
   │  STM32F103C8T6 (LQFP48) │◄─────── PA0 (ADC1_IN0) ─────────────┤ AFE AD8232 (Rail VDDA)│
   │                         │◄─────── PA1, PA2 (LOD+, LOD-) ──────┤  + Bảo vệ ESD đầu vào│
   │  • HSE 8MHz + LSE 32kHz │──────── PA3 (Chân SDN tắt AD8232) ─►│  + Jumper 3 chân RLD │
   │  • PA8 -> TP_MCO        │                                     └──────────────────────┘
   └──┬─────────┬─────────┬──┘
      │         │         └──► PB0 (Điều khiển cổng P-MOSFET AO3401) ──► [Khe thẻ microSD (SPI1)]
      │         └────────────► PA9, PA10 (Header UART1: Nạp MIT-BIH / Đọc log khi tháo điện cực)
      └──────────────────────► PA13, PA14, NRST (Header SWD nạp/debug)
```

### Bảng quy hoạch chân vi điều khiển (Pinout Mapping)

| Khối chức năng | Chân STM32F103C8T6 | Tên tín hiệu (Net) | Vai trò & Điểm đo kiểm (Testpoint) |
| :--- | :--- | :--- | :--- |
| **AFE Điện tim (`AD8232`)** | `PA0` (`ADC1_IN0`) | `ECG_OUT` | Ngõ ra tín hiệu tương tự; có chốt đo **`TP_ECG_OUT`** sát chân MCU. |
| | `PA1`, `PA2` | `ECG_LOD_P`, `ECG_LOD_N` | Phát hiện tuột/rời điện cực (Leads-off detection). |
| | `PA3` | `ECG_SDN` | Điều khiển tắt `AD8232` (`SDN = LOW`) khi vào chế độ ngủ. |
| **Lưu trữ (`microSD`)** | `PA4..PA7` | `SD_CS`, `SPI1_SCK/MISO/MOSI` | Giao tiếp SPI1 ghi dữ liệu thô & chuỗi RR theo khối 512 bytes. |
| | `PB0` | `SD_PWR_EN` | Điều khiển cực Gate P-MOSFET `AO3401` ngắt nguồn thẻ SD; có **`TP_SD_EN`**. |
| **Đo dòng tích hợp (`INA219`)** | `PB6`, `PB7` | `I2C1_SCL`, `I2C1_SDA` | Đọc dòng/áp tiêu thụ trên rail chính để đối chiếu với PPK2. |
| **Đo áp pin (`VBAT`)** | `PB1` (`ADC1_IN9`) | `BAT_SENSE` | Đo áp pin qua cầu phân áp tỷ lệ 1:2 ($100\text{ k}\Omega / 100\text{ k}\Omega$). |
| | `PB5` | `BAT_SENSE_EN` | Điều khiển cực Gate N-MOSFET `2N7002` để ngắt cầu phân áp chống rò dòng khi ngủ sâu. |
| **Giao tiếp & Debug** | `PA9`, `PA10` | `DBG_TX`, `DBG_RX` | Header UART1 dùng cho luồng nạp dữ liệu chuẩn MIT-BIH (HIL Test). |
| | `PA13`, `PA14`, `NRST` | `SWDIO`, `SWCLK`, `NRST` | Header nạp và gỡ lỗi chuẩn SWD. |
| | `PA8` | `MCO_OUT` | Đưa ra chốt đo **`TP_MCO`** để kiểm tra xung nhịp HSE khi bring-up. |
| **Điều khiển người dùng** | `PB12` (`EXTI12`) | `WAKE_BTN` | Nút nhấn đánh thức MCU khỏi `Stop Mode` và bắt đầu/dừng phiên đo (kéo lên $100\text{ k}\Omega$ về `3V3_MAIN`, tụ chống dội $100\text{ nF}$, dòng rò $0\ \mu\text{A}$ khi không nhấn). |

### Tính toán tụ tải thạch anh (Crystal Load Capacitance)
Công thức tính tụ tải cho thạch anh ngoài:
$$C_{L1} = C_{L2} = 2 \cdot (C_L - C_{\text{stray}})$$
* **HSE $8\ \text{MHz}$:** Chọn thạch anh có $C_L = 12\ \text{pF}$, ước tính $C_{\text{stray}} \approx 2\ \text{pF} \implies C_{L1} = C_{L2} = 20\ \text{pF}$ (chọn tụ `0603` C0G/NP0 $20\ \text{pF}$).
* **LSE $32,768\ \text{kHz}$:** Chọn thạch anh có $C_L = 6\ \text{pF}$, ước tính $C_{\text{stray}} \approx 1,5\ \text{pF} \implies C_{L1} = C_{L2} = 9\ \text{pF}$ (chọn tụ `0603` C0G/NP0 $8,2\ \text{pF}$ hoặc $10\ \text{pF}$).

---

## 3. Kiến trúc Firmware & Thuật toán Xử lý Tín hiệu (DSP Pipeline)

Toàn bộ mã nguồn được thiết kế theo hướng cấp phát tĩnh (Zero-Heap), tách biệt nguồn dữ liệu đầu vào (từ DMA ADC hoặc từ UART MIT-BIH) và đi qua chuỗi xử lý:

1. **Tiền xử lý & Lọc số đa tầng (`Q15` vs `Soft-Float`):**
   * Bộ lọc chắn dải IIR Biquad Notch $50\ \text{Hz}$ khử nhiễu nguồn lưới.
   * Bộ lọc thông dải làm nổi bật năng lượng phức bộ QRS ($5–15\ \text{Hz}$).
2. **Phát hiện đỉnh R (Pan–Tompkins + Sub-sample Interpolation):**
   * Chuỗi toán tử: Đạo hàm $\rightarrow$ Bình phương $\rightarrow$ Tích phân cửa sổ trượt (Moving-Window Integration) $\rightarrow$ Ngưỡng kép thích nghi kèm cơ chế tìm kiếm ngược (Search-back) và thời gian trơ ($200\ \text{ms}$).
   * **Nội suy Parabol 3 điểm quanh đỉnh cực đại:** Khắc phục sai số lượng tử hóa thời gian khi lấy mẫu ở tần số thấp ($250\ \text{Hz}$ có chu kỳ mẫu $4\ \text{ms}$), đưa độ phân giải thời gian định vị đỉnh R xuống dưới $0,5\ \text{ms}$.
3. **Khử nhiễu chuỗi RR & Phân tích HRV trực tuyến:**
   * Lọc bỏ nhịp ngoại lai/nhiễu cơ bằng bộ lọc thống kê trên cửa sổ trượt trước khi cập nhật các chỉ số miền thời gian: `Mean RR`, `HR`, `SDNN`, `RMSSD`, và `pNN50`.
4. **Ghi thẻ nhớ Bất đồng bộ (Ping-Pong Double Buffering):**
   * Sử dụng bộ đệm đôi kết hợp Timer-triggered ADC + DMA để đảm bảo jitter lấy mẫu bằng 0 ngay cả khi thẻ microSD có độ trễ ghi khối.

---

## 4. Ngân sách Công suất Dự kiến (Power Budget – Gate G1)

### A. Ngân sách dòng tĩnh ở chế độ Ngủ sâu (Stop Mode)

| Linh kiện | Trạng thái ở chế độ Ngủ | Dòng tĩnh điển hình ($I_{\text{typ}}$) | Dòng tĩnh tối đa ($I_{\text{max}}$) | Cơ chế điều khiển phần cứng/phần mềm |
| :--- | :--- | :---: | :---: | :--- |
| **`STM32F103C8T6`** | `Stop Mode` (LDO On, LSE RTC On) | $24,0\ \mu\text{A}$ | $36,0\ \mu\text{A}$ | Đưa toàn bộ GPIO không dùng về `Analog Input`. |
| **LDO $3,3\ \text{V}$ (`MCP1700`)** | Đang cấp nguồn chờ | $1,6\ \mu\text{A}$ | $4,0\ \mu\text{A}$ | Dòng tĩnh tự nhiên của LDO CMOS. |
| **AFE `AD8232ACPZ`** | Shutdown (`SDN = GND`) | $0,5\ \mu\text{A}$ | $1,0\ \mu\text{A}$ | Kéo chân `PA3` xuống mức thấp. |
| **Đo dòng `INA219`** | Power-Down Mode | $6,0\ \mu\text{A}$ | $15,0\ \mu\text{A}$ | Ghi lệnh Power-Down qua bus I2C1 trước khi ngủ. |
| **Thẻ nhớ `microSD`** | Ngắt nguồn hoàn toàn | $0,0\ \mu\text{A}$ | $0,1\ \mu\text{A}$ | Cắt nguồn VCC bằng P-MOSFET `AO3401` (`PB0 = HIGH`). |
| **Tổng cộng ($\sum I_q$)** | — | **$32,1\ \mu\text{A}$** | **$56,1\ \mu\text{A}$** | **Ngưỡng cho phép $\le 1{,}5 \times 32,1 = 48{,}15\ \mu\text{A}$**. |

### B. Ước tính dòng tiêu thụ khi hoạt động & Thời lượng pin (Pin Li-ion $500\ \text{mAh}, k_{\text{giảm}} = 0,8$)

| Chế độ vận hành | Dòng MCU | Dòng AFE + `INA219` | Dòng `microSD` (Trung bình) | Tổng dòng trung bình ($I_{\text{tb}}$) | Thời lượng pin ước tính |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Chế độ Cơ sở (Run $72\ \text{MHz}$, `Soft-Float`)** | $27,0\ \text{mA}$ | $0,87\ \text{mA}$ | $2,5\ \text{mA}$ | **$\approx 30,4\ \text{mA}$** | **$\approx 13,1\ \text{giờ}$** |
| **Chế độ Tối ưu (Run $8\ \text{MHz}$ + Sleep giữa mẫu, `Q15`)** | $4,2\ \text{mA}$ | $0,87\ \text{mA}$ | $2,5\ \text{mA}$ | **$\approx 7,6\ \text{mA}$** | **$\approx 52,6\ \text{giờ}$** |

---

## 5. Thiết kế Thí nghiệm & Ba bài toán Ablation Bắt buộc

1. **Ablation 1 — Toán học dấu phẩy tĩnh `Q15` vs Dấu phẩy động mềm (`Soft-Float`):**
   * Đo số chu kỳ xung nhịp thực thi (`min` / `mean` / `max`) của từng mẫu bằng thanh ghi phần cứng `DWT->CYCCNT`.
   * Đánh giá mức tiết kiệm năng lượng (qua PPK2) và độ lệch chỉ số HRV giữa hai biểu diễn số học.
2. **Ablation 2 — Tần số lấy mẫu $250\ \text{Hz}$ vs $500\ \text{Hz}$:**
   * Đánh giá sự đánh đổi giữa năng lượng tiêu thụ / dung lượng lưu trữ và sai số $\text{MAE}_{RR}$ (có và không có nội suy đỉnh Parabol).
3. **Ablation 3 — Có mạch chân phải (`Active RLD`) vs Không có RLD (`Reference Grounding`):**
   * Sử dụng jumper 3 chân `J_RLD` trên PCB để chuyển đổi trực tiếp giữa chế độ lái chân phải tích cực và nối đất ảo $1,65\ \text{V}$.
   * Định lượng mức suy giảm nhiễu nguồn lưới $50\ \text{Hz}$ (CMRR thực tế) và tỷ lệ phát hiện đỉnh R.

---

## 6. Cấu trúc Thư mục Dự án (Repository Structure)

```text
├── hardware/                  # Project Altium Designer (.PrjPcb), SchLib, PcbLib, Gerber, BOM
├── firmware/                  # Mã nguồn STM32F103C8T6 (Quản lý bằng CMake / STM32Cube)
│   ├── Core/                  # Khởi tạo ngoại vi (ADC+DMA, TIM, SPI, UART, I2C, DWT)
│   ├── DSP/                   # Bộ lọc IIR Q15/Float, Pan-Tompkins, Parabolic Interpolation, HRV
│   └── HIL/                   # Giao thức nhận mẫu MIT-BIH và trả kết quả qua UART
├── scripts/                   # Công cụ Python phục vụ kiểm định & xuất hình bài báo
│   ├── hil_mitbih_runner.py   # Tự động bơm dữ liệu MIT-BIH và chấm điểm Se, PPV
│   ├── bland_altman_eval.py   # Phân tích MAE, RMSE, Bland-Altman, kiểm định Wilcoxon/t-test
│   └── power_profiler.py      # Đối chiếu dữ liệu dòng INA219 với Nordic PPK2
├── data/                      # Dữ liệu thô CSV (tách bạch tập calib/ và test/)
└── docs/                      # Hồ sơ qua 4 cổng G1-G4, nhật ký lỗi lắp ráp và bản thảo bài báo
```

---

## 7. Phân công Nhiệm vụ & Tiến độ 4 Cổng (Gates)

* **SV1 – Phần cứng:** Thiết kế sơ đồ nguyên lý, thư viện chuẩn IPC-7351 + 3D, layout PCB 2 lớp, đặt mạch + stencil, chủ trì hàn SMD và bring-up.
* **SV2 – Firmware:** Xây dựng kiến trúc driver DMA, chuỗi thuật toán DSP `Q15`/`Soft-float`, quản lý năng lượng Stop/Sleep mode và giao thức HIL.
* **SV3 – Đo đạc & Phân tích:** Vận hành thiết bị chuẩn (Polar H10, PPK2, Oscilloscope), thu thập dữ liệu trên $\ge 15$ tình nguyện viên, chạy phân tích thống kê và chấp bút bản thảo.

| Cổng kiểm tra | Thời điểm | Trạng thái | Sản phẩm bàn giao chính |
| :--- | :---: | :---: | :--- |
| **G1 – Duyệt nguyên lý** | Tuần 2 | ⏳ Đang thực hiện | Sơ đồ khối, Schematic (`ERC = 0`), Power Budget, Kế hoạch thiết bị chuẩn |
| **G2 – Duyệt layout & Đặt mạch** | Tuần 3 | ⬜ Chưa bắt đầu | Layout 2 lớp (`DRC = 0`), Review chéo (Phụ lục A), Gerber/BOM, Đặt PCB + Stencil |
| **G3 – Bring-up phần cứng** | Tuần 6 | ⬜ Chưa bắt đầu | Hàn hoàn thiện $\ge 2$ bo, đạt Checklist Phụ lục B, nhận SWD/HSE/AFE/SD |
| **G4 – Nghiệm thu & Bản thảo** | Tuần 10 | ⬜ Chưa bắt đầu | Dữ liệu CSV, phân tích MAE/Bland–Altman, 3 Ablation, Bản thảo bài báo |