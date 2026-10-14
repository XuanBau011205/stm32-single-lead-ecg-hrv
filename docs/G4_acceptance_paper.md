# Cổng Kiểm tra G4: Nghiệm thu & Bản thảo Bài báo (Acceptance & Paper Draft)

- **Mã đề tài:** P05
- **Thời điểm:** Tuần 10
- **Trọng tâm:** Kiểm định thực nghiệm lâm sàng, HIL benchmark, phân tích 3 bài toán Ablation và hoàn thiện bản thảo bài báo khoa học.

## Checklist Sản phẩm Bàn giao (Deliverables Checklist)

- [ ] **1. Dữ liệu Thực nghiệm Lâm sàng & Đối chiếu (CSV Datasets):**
  - Thu thập dữ liệu ECG và chuỗi RR trên $\ge 15$ tình nguyện viên (5 phút nghỉ tĩnh + 5 phút sau vận động).
  - Bản ghi đồng bộ với đai đo nhịp tim chuẩn **Polar H10**.
  - Lưu trữ tách bạch tại `data/test/`.
- [ ] **2. Báo cáo Thống kê MAE & Đồ thị Bland–Altman:**
  - Sai số khoảng cách đỉnh R: $\text{MAE}_{RR} \le 5\ \text{ms}$.
  - Giới hạn đồng thuận 95% nhịp tim (LoA 95% HR) nằm trong khoảng $\pm 2\ \text{bpm}$.
  - Kết quả kiểm định thống kê Wilcoxon signed-rank / paired t-test xác nhận không có sai lệch hệ thống có ý nghĩa thống kê ($p > 0{,}05$).
- [ ] **3. Báo cáo Ba Bài toán Ablation Bắt buộc:**
  - **Ablation 1 (Q15 Fixed-Point vs Soft-Float):** Số chu kỳ xung nhịp CPU (`DWT->CYCCNT`), phần trăm năng lượng tiết kiệm và độ lệch HRV.
  - **Ablation 2 (Tần số lấy mẫu 250 Hz vs 500 Hz):** So sánh năng lượng tiêu thụ / dung lượng ghi thẻ nhớ và sai số $\text{MAE}_{RR}$ khi có/không có nội suy đỉnh Parabol.
  - **Ablation 3 (Có Active RLD vs Không có RLD):** Mức triệt tiêu nhiễu nguồn 50 Hz (CMRR thực tế) và độ nhạy phát hiện đỉnh R ($Se$, $PPV$).
- [ ] **4. Nghiệm thu Các Chỉ tiêu KPIs Toàn diện:**
  - Nhiễu quy về đầu vào $\le 30\ \mu\text{V}_{\text{pp}}$ (băng tần $0{,}5–40\ \text{Hz}$).
  - Độ nhạy $Se \ge 99\%$ và $PPV \ge 99\%$ trên cơ sở dữ liệu MIT-BIH Arrhythmia Database.
  - Dòng ngủ toàn mạch Stop Mode $\le 48{,}15\ \mu\text{A}$ (đo bằng Nordic PPK2).
- [ ] **5. Bản thảo Bài báo Khoa học Hoàn chỉnh (Paper Draft):**
  - Bản thảo theo định dạng IEEE/Springer (Latex / Word) đầy đủ các phần: Abstract, Introduction, System Architecture, Fixed-point DSP Pipeline, Experimental Validation, Ablation Results, Conclusion.
