# Cổng Kiểm tra G3: Bring-up Phần cứng (Hardware Bring-up)

- **Mã đề tài:** P05
- **Thời điểm:** Tuần 6
- **Trọng tâm:** Lắp ráp, hàn linh kiện dán SMD, kiểm tra thông mạch, kiểm tra cấp nguồn và xác minh hoạt động từng khối chức năng ngoại vi.

## Checklist Sản phẩm Bàn giao (Deliverables Checklist)

- [ ] **1. Hoàn thiện Lắp ráp Prototype:**
  - Hàn và làm sạch hoàn thiện $\ge 2$ bo mạch nguyên mẫu P05.
  - Kiểm tra ngắn mạch các rail nguồn trước khi cấp điện ($3\text{V}3$, $V_{\mathrm{BAT}}$, $V_{\mathrm{DDA}}$).
- [ ] **2. Nghiệm thu theo Bảng kiểm Bring-up (Checklist Phụ lục B):**
  - Điện áp đầu ra LDO MCP1700 ổn định tại $3{,}3\ \text{V} \pm 2\%$.
  - Mạch sạc pin MCP73831 tự ngắt khi sạc đầy $4{,}2\ \text{V}$.
  - Transistor P-MOSFET AO3401 ngắt nguồn MicroSD hoàn toàn khi $PB0 = \text{HIGH}$.
- [ ] **3. Giao tiếp Nạp & Xung nhịp Vi điều khiển:**
  - ST-Link / J-Link nhận diện vi điều khiển qua cổng SWD (SWDIO, SWCLK, NRST).
  - Dao động ký (Oscilloscope) kiểm tra xung nhịp ra tại `TP_MCO`:
    - HSE dao động ổn định $8{,}000\ \text{MHz}$.
    - LSE dao động ổn định $32{,}768\ \text{kHz}$.
- [ ] **4. Bring-up Ngoại vi Cốt lõi:**
  - **AFE AD8232:** Đo tín hiệu ra tại `TP_ECG_OUT`, chân ngắt SDN tắt được mạch analog.
  - **I2C INA219:** Giao tiếp đọc bus voltage và current thành công qua bus I2C1.
  - **SPI MicroSD:** Giao tiếp SPI1 khởi tạo thẻ nhớ (CMD0, CMD8, ACMD41) và ghi/đọc sector 512 bytes thành công.
