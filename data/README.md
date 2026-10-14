# Quy tắc Quản lý Dữ liệu Thực nghiệm (Data Management Rules)

Thư mục `data/` chứa toàn bộ tập dữ liệu tín hiệu điện tim (ECG), chuỗi RR thực nghiệm và log đo kiểm phục vụ đánh giá đề tài P05.

## 1. Cấu trúc Thư mục

- `calib/`: Dữ liệu dùng cho căn chỉnh thông số thuật toán, hiệu chuẩn ngưỡng và offset phần cứng.
- `test/`: Dữ liệu độc lập phục vụ kiểm định, nghiệm thu các chỉ tiêu KPIs và xuất kết quả bài báo.

## 2. Quy tắc Bắt buộc: Tách bạch Tuyệt đối (Strict Segregation)

> **NGUYÊN TẮC VÀNG:** Tuyệt đối KHÔNG trộn lẫn dữ liệu giữa `calib/` và `test/` (Zero Data Leakage).

1. **Tập hiệu chỉnh (`calib/`):**
   - Được phép sử dụng trong quá trình phát triển để điều chỉnh ngưỡng thích nghi Pan-Tompkins ($SPKI$, $NPKI$), hệ số bộ lọc dải thông và bù sai lệch offset của bộ chuyển đổi ADC/AFE AD8232.
   - Chứa tối đa 20% tổng dung lượng dữ liệu thu thập hoặc các bản ghi MIT-BIH chỉ định dùng cho huấn luyện/cân chỉnh.

2. **Tập kiểm định nghiệm thu (`test/`):**
   - Phải được giữ **hoàn toàn vô hình** trong suốt quá trình xây dựng và tối ưu thuật toán.
   - Chỉ được đưa vào chạy đúng một lần trong pha nghiệm thu cuối cùng (HIL testbench và kiểm định lâm sàng trên $\ge 15$ tình nguyện viên đối chiếu Polar H10).
   - Mọi hành vi dùng dữ liệu trong `test/` để tinh chỉnh ngưỡng hoặc hệ số đều làm mất hiệu lực của kết quả nghiên cứu.

## 3. Định dạng Dữ liệu & Quy ước Đặt tên File

- Tín hiệu ECG nén thô từ thẻ nhớ SD: `SUBJ<XX>_RAW_<FS>HZ_<TIMESTAMP>.bin`
- Dữ liệu chuỗi RR đã giải mã: `SUBJ<XX>_RR_P05.csv`
- Dữ liệu chuẩn Polar H10: `SUBJ<XX>_RR_POLAR.csv`
- Dữ liệu dòng điện: `POWER_<MODE>_INA219.csv` và `POWER_<MODE>_PPK2.csv`
- Không commit các file dữ liệu lớn (`> 5 MB`) lên git; chỉ lưu trữ bản tóm tắt hoặc link lưu trữ ngoài (OneDrive / Google Drive).
