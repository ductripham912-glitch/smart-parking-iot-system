# TÀI LIỆU NGUYÊN LÝ HOẠT ĐỘNG HỆ THỐNG SMART PARKING (ESP32 & WEB)

---

## 1. TỔNG QUAN HỆ THỐNG

Dự án **Smart Parking ESP32 Web** là một giải pháp quản lý bãi đỗ xe thông minh toàn diện, tích hợp giữa phần cứng vi điều khiển ESP32, nền tảng điện toán đám mây **Firebase Realtime Database (RTDB)** và giao diện điều hành **Web**.

Hệ thống được thiết kế theo mô hình **IoT phân tán đa vi điều khiển**, phân tách độc lập các khối chức năng nhằm đảm bảo khả năng xử lý thời gian thực, độ tin cậy cao và tính năng hoạt động offline khi gặp sự cố mất kết nối mạng.

```
                     ┌──────────────────────────────────────────────┐
                     │         Firebase Realtime Database           │
                     │          & Giao diện Web Quản lý             │
                     └──────────────▲──────────────────▲────────────┘
                                    │ (WiFi)           │ (WiFi)
                           ┌────────┴───────┐   ┌──────┴────────┐
                           │  ESP32 GATE IN │   │ ESP32 GATE OUT│
                           └────────▲───────┘   └───────────────┘
                     (UART2)        │
                ┌───────────────────┴──┐
                │      ESP32 LOT       │
                │  (4 Cảm biến ô đỗ)   │
                └──────────────────────┘
```

Hệ thống gồm 4 môi trường firmware được quản lý qua PlatformIO (`platformio.ini`):
1. **`env:lot`**: Node giám sát 4 ô đỗ xe trong bãi (A1, A2, A3, A4), chạy độc lập, không dùng WiFi, truyền dữ liệu qua UART2.
2. **`env:gate_in`**: Node điều khiển Barrier Cổng Vào, tích hợp đầu đọc thẻ RFID, 2 cảm biến quang hành trình xe, còi buzzer, màn hình LCD 1602 I2C và đóng vai trò cầu nối nhận dữ liệu từ bãi đỗ đẩy lên Firebase.
3. **`env:gate_out`**: Node điều khiển Barrier Cổng Ra, tích hợp đầu đọc thẻ RFID, 2 cảm biến quang hành trình, màn hình đồ họa OLED SSD1306, còi buzzer; tính toán phí đỗ xe và phối hợp thanh toán VietQR với Web.
4. **`env:reset`**: Firmware tiện ích hỗ trợ format sạch phân vùng NVS Flash (xoá cache và hàng đợi) khi cần bảo trì.

---

## 2. KIẾN TRÚC VÀ CẤU HÌNH PHẦN CỨNG (PINOUT)

### 2.1. Cổng Vào (`gate_in`)
| Linh kiện | Chân ESP32 | Giao thức / Chức năng | Ghi chú |
| :--- | :--- | :--- | :--- |
| **RFID-RC522** | SCK=18, MISO=19, MOSI=23, SS=5, RST=4 | SPI phần cứng | Đọc thẻ cư dân / thẻ khách |
| **Servo Barrier** | GPIO 13 | PWM (50Hz) | 90° (Đóng), 180° (Mở) |
| **Cảm biến IR 1 (Arrive)** | GPIO 32 | Digital Input (Pull-up) | Phát hiện xe tới trước cổng |
| **Cảm biến IR 2 (Pass)** | GPIO 33 | Digital Input (Pull-up) | Phát hiện xe đã vượt qua barrier |
| **Màn hình LCD 1602** | SDA=21, SCL=22 | I2C (50kHz, Addr: 0x27/0x3F) | Báo số chỗ trống & chỉ dẫn ô |
| **Còi Buzzer** | GPIO 14 | Digital Output (Active) | Báo trạng thái âm thanh phi chặn |
| **LED trạng thái** | GPIO 2 | Digital Output | Sáng khi mở cổng, chớp khi từ chối |
| **UART2 (Lot Link)** | RX=16, TX=17 | Serial2 (9600 Baud) | Nhận dữ liệu từ board bãi đỗ |

### 2.2. Cổng Ra (`gate_out`)
| Linh kiện | Chân ESP32 | Giao thức / Chức năng | Ghi chú |
| :--- | :--- | :--- | :--- |
| **RFID-RC522** | SCK=18, MISO=19, MOSI=23, SS=5, RST=4 | SPI phần cứng | Đọc thẻ thanh toán và xác thực ra |
| **Servo Barrier** | GPIO 13 | PWM (50Hz) | 90° (Đóng), 180° (Mở) |
| **Cảm biến IR 1 (Arrive)** | GPIO 32 | Digital Input (Pull-up) | Phát hiện xe chờ xuất bến |
| **Cảm biến IR 2 (Pass)** | GPIO 33 | Digital Input (Pull-up) | Phát hiện xe đã rời khỏi cổng |
| **Màn hình OLED SSD1306** | SDA=21, SCL=22 | I2C (400kHz, Addr: 0x3C/0x3D) | Hiển thị cước phí, QR, trạng thái |
| **Còi Buzzer** | GPIO 14 | Digital Output (Active) | Báo thẻ hợp lệ, chờ trả tiền, từ chối |
| **LED trạng thái** | GPIO 2 | Digital Output | Sáng khi mở / thanh toán, chớp khi lỗi |

### 2.3. Bãi Đỗ Xe (`lot`)
| Cảm biến ô đỗ | Chân ESP32 | Loại ô | Ghi chú |
| :--- | :--- | :--- | :--- |
| **Cảm biến IR ô A1** | GPIO 32 | Ô Cư dân | Active LOW khi có xe đỗ |
| **Cảm biến IR ô A2** | GPIO 33 | Ô Cư dân | Active LOW khi có xe đỗ |
| **Cảm biến IR ô A3** | GPIO 25 | Ô Khách vãng lai | Active LOW khi có xe đỗ |
| **Cảm biến IR ô A4** | GPIO 26 | Ô Khách vãng lai | Active LOW khi có xe đỗ |
| **UART2 TX** | GPIO 17 (Baud 9600) | Gửi sang gate_in | Chuỗi bản tin `$dddd*HH\n` |

---

## 3. NGUYÊN LÝ HOẠT ĐỘNG CHI TIẾT CỦA CÁC PHÂN HỆ

### 3.1. Thư viện lõi nền tảng: `ParkingCore`
Thư viện `ParkingCore` cung cấp hạ tầng kết nối vững chắc cho hai cổng với những nguyên tắc kỹ thuật sau:
1. **Kết nối WiFi thông minh (`WiFiMulti`)**:
   - Chứa danh sách các điểm truy cập WiFi định sẵn trong `secrets.h`. ESP32 tự động quét và kết nối mạng có cường độ sóng (RSSI) tốt nhất.
   - Khi mất mạng, ESP32 tự động thăm dò và tái kết nối ngầm mà không làm treo hệ thống.
2. **Thời gian thực NTP**:
   - Tự động lấy mốc thời gian UTC/Epoch mili giây từ máy chủ NTP (`pool.ntp.org`, `time.google.com`).
   - Mọi bản ghi lịch sử, phiên gửi xe đều dùng mốc mili giây chuẩn quốc tế.
3. **Cơ chế hoạt động Ngoại tuyến (Offline-First / Cache Flash)**:
   - Toàn bộ danh sách cư dân (`/residents`), thẻ khách (`/cards`), và cấu hình giá cước (`/config`) được lưu vào Flash qua thư viện `Preferences` dưới dạng Blob nhị phân.
   - Khi mất kết nối Internet, hệ thống vẫn nhận diện cư dân và thẻ khách từ bộ nhớ cục bộ.
   - Đồng bộ Cache được chia làm 3 bước (`syncStep`) lặp tuần tự qua các vòng lặp: hạn chế tối đa việc chiếm dụng CPU liên tục.
4. **Hàng đợi ghi dữ liệu lưu Flash (Persistent Circular Queue)**:
   - Các lệnh ghi Firebase (`Set`, `Push`, `Delete`) khi gặp lúc offline hoặc mạng nghẽn sẽ được đưa vào hàng đợi vòng (tối đa 30 lệnh) lưu trong NVS Flash.
   - Khi online trở lại, hệ thống tuần tự gửi bù (`flushQueue`) với thuật toán giãn cách thời gian (Exponential Backoff: 2s, 4s, 8s, tối đa 30s).
   - Kể cả khi mất nguồn đột ngột, hàng đợi không bị mất và được phục hồi ngay khi khởi động lại.
5. **Cơ chế nhường quyền CPU (`setBusy`)**:
   - Khi xe đang ở barrier hoặc đang chờ thanh toán, cờ `busyFlag` được kích hoạt.
   - Các tác vụ nặng (quét mạng WiFi, gửi hàng đợi ngầm, đồng bộ dữ liệu) lập tức tạm hoãn để CPU phục vụ tức thời cho cảm biến chống kẹp xe và servo.
6. **Heartbeat & Giám sát thiết bị**:
   - Định kỳ mỗi 15 giây, gửi thông số tình trạng phần cứng lên `/devices/<deviceName>` gồm: địa chỉ IP, RSSI, thời gian hoạt động (uptime), số lệnh hàng đợi, dung lượng RAM Heap còn trống, số lần boot và lý do reset vi điều khiển.

---

### 3.2. Phân hệ Giám sát bãi đỗ (`lot`) và Cầu nối truyền tin (`lot_link.h`)
1. **Cảm biến và chống nhiễu tại bãi đỗ**:
   - 4 cảm biến hồng ngoại đặt tại 4 khoang xe.
   - Bộ lọc Debounce kiểm tra ổn định 600ms, loại bỏ hoàn toàn các trường hợp phản xạ ánh sáng ảo hoặc chập chờn.
2. **Giao thức truyền thông UART Lot-Link**:
   - Khung dữ liệu định dạng ASCII dạng:
     $$\$d_1 d_2 d_3 d_4 * HH \backslash n$$
     *Trong đó:*
     - $d_i$: Trạng thái ô (`1` = có xe, `0` = trống).
     - `$`: Ký tự bắt đầu khung.
     - `*`: Ký tự phân cách Checksum.
     - $HH$: Mã checksum XOR 1 byte dạng Hex của các ký tự trạng thái.
   - Dữ liệu được phát ngay khi có biến động hoặc gửi định kỳ 1 giây/lần.
3. **Xử lý và hiển thị tại Cổng vào (`lot_link.h`)**:
   - Cổng vào lắng nghe UART2, giải mã và đối chiếu Checksum. Nếu sai lệch khung sẽ tự động hủy bỏ.
   - Tự động đồng bộ trạng thái lên Firebase:
     - Ô cư dân: `"co_xe"` hoặc `"giu_cho"`.
     - Ô khách: `"co_xe"` hoặc `"trong"`.
   - Điều khiển màn hình **LCD 1602 (I2C)**:
     - Dòng 1: `Cho khach: X/Y *` (dấu `*` báo online, `!` báo offline).
     - Dòng 2: `Moi vao o A3` (gợi ý ô khách trống đầu tiên) hoặc `HET CHO KHACH`.

---

### 3.3. Phân hệ Cổng vào (`gate_in`)
Quy trình vận hành tại cổng vào diễn ra khép kín theo trình tự:
1. **Quét thẻ RFID**: Khi thẻ đưa vào vùng từ trường của RC522, vi điều khiển đọc UID và lọc chống quét lặp lại trong vòng 2 giây.
2. **Kiểm tra nghiệp vụ**:
   - **Đối với Cư dân**:
     - Kiểm tra tồn tại trong danh sách cư dân (`docRes`).
     - **Chống gian lận (Anti-passback)**: Kiểm tra trạng thái `trongBai`. Nếu xe đã ở trong bãi mà quẹt tiếp ở cổng vào -> Từ chối, còi hú 3 tiếng, ghi log cảnh báo lên Firebase.
     - Nếu hợp lệ -> Chuyển sang trạng thái Mở cổng.
   - **Đối với Khách**:
     - Kiểm tra thẻ đã được đăng ký trên hệ thống và không bị khóa.
     - **Anti-passback**: Thẻ đã có phiên đang đỗ (`/sessions/<UID>`) -> Từ chối.
     - **Kiểm soát sức chứa**: Đếm số xe khách đang trong bãi. Nếu số lượng đã đạt mức tối đa (`khachToiDa`, mặc định 2 xe) -> Từ chối báo hết chỗ.
     - Nếu đủ điều kiện -> Chuyển sang trạng thái Mở cổng.
3. **Điều khiển Barrier và Cảm biến liên động**:
   - Servo quay 180° (nâng cần barrier), còi kêu 1 bíp xác nhận thành công, kích hoạt chế độ `busy`.
   - Xe tiến qua cảm biến trước cổng (`irArrive`) và vượt qua cảm biến sau cổng (`irPass`).
   - Khi xe đã vượt qua hoàn toàn (`irPass` giải phóng vật cản liên tục trong 1 giây) -> Servo đóng về 90°.
   - **Xác nhận giao dịch**: Ghi nhận cư dân vào bãi (`trongBai = true`) hoặc tạo phiên khách mới `/sessions/<UID>` kèm thời điểm `gioVao`. Ghi lịch sử vào `/history`.
   - **Xử lý sự cố (Timeout)**: Nếu mở cổng sau 20 giây mà xe không tiến vào (cả 2 cảm biến đều không thấy xe) -> Barrier tự động đóng lại, phát còi cảnh báo dài, không ghi nhận phiên đỗ.

---

### 3.4. Phân hệ Cổng ra (`gate_out`)
Cổng ra đảm nhiệm vai trò chốt chặn an ninh, tính toán chi phí và điều phối thanh toán không tiền mặt:
1. **Quét thẻ ra**:
   - **Đối với Cư dân**:
     - Kiểm tra trạng thái `residentInside`. Nếu chưa vào bãi mà quẹt ra (vi phạm anti-passback) -> Từ chối.
     - Hợp lệ -> Mở barrier ngay lập tức (miễn phí), cập nhật `trongBai = false`, ghi nhận lịch sử ra bãi.
   - **Đối với Khách**:
     - Đọc dữ liệu phiên `/sessions/<UID>` để lấy thời điểm `gioVao`.
     - Tính tổng thời gian đỗ xe:
       $$\text{Số phút} = \frac{\text{Thời điểm hiện tại} - \text{Giờ vào}}{60000}$$
     - Tính cước phí dựa trên cấu hình:
       $$\text{Số block} = \left\lceil \frac{\text{Số phút}}{\text{blockPhut}} \right\rceil$$
       $$\text{Tổng phí} = \max(1, \text{Số block}) \times \text{donGiaMoiGio}$$
       *(Mặc định: 60 phút/block, 5.000 VNĐ/block; xe vừa vào lập tức tính tối thiểu 1 block).*
2. **Quy trình Thanh toán trực tuyến (Paying State)**:
   - Còi phát 2 tiếng bíp báo thanh toán.
   - Màn hình OLED hiển thị số tiền cần trả (`Phi: XX.000 d`) và thông báo `Quet ma QR tren Web / Cho xac nhan...`.
   - ESP32 đẩy bản tin yêu cầu thanh toán lên Firebase:
     `/sessions/<UID>/thanhToan = { trangThai: "cho_thanh_toan", phi, phut, gio }`
   - Giao diện Web thu ngân/người dùng bắt được bản tin này và tự động tạo mã **VietQR** chuẩn Napas247 tương ứng với số tiền và nội dung chuyển khoản.
   - ESP32 định kỳ mỗi 800ms thăm dò kết quả trên Firebase:
     - Khi hệ thống Web xác nhận tiền đã về tài khoản (`da_thanh_toan` hoặc Web đã chủ động đóng phiên) -> ESP32 chuyển sang mở barrier cho xe ra.
     - Sau 60 giây nếu chưa có thanh toán -> Tự động hủy yêu cầu thanh toán trên Firebase và đóng cổng.
3. **Cơ chế Ân hạn đã thanh toán (Paid Grace Period - 120 giây)**:
   - Trong trường hợp khách đã thanh toán thành công, barrier đã mở nhưng xe gặp sự cố chưa kịp qua (khiến cổng bị timeout 20s tự hạ xuống), khách chỉ cần quẹt lại thẻ trong vòng 120 giây, hệ thống nhận diện đây là xe đã hoàn tất thanh toán và tự động mở lại barrier mà **không tính thêm phí hay bắt trả tiền lần hai**.
4. **Đóng cổng và Lưu trữ**:
   - Khi xe qua hẳn cảm biến `irPass`, barrier hạ xuống 90°, màn hình OLED gửi lời chào tạm biệt.
   - Xóa phiên đỗ xe khỏi `/sessions/<UID>`.
   - Ghi chi tiết cuốc gửi xe vào `/history` (giờ vào, giờ ra, số phút đỗ, số tiền thu được, phương thức thanh toán VietQR).

---

## 4. CẤU TRÚC DỮ LIỆU TRÊN FIREBASE REALTIME DATABASE

```json
{
  "config": {
    "donGiaMoiGio": 5000,
    "blockPhut": 60,
    "khachToiDa": 2,
    "o": {
      "A1": { "chuXe": "RESIDENT_01", "loai": "cu_dan" },
      "A2": { "chuXe": "RESIDENT_02", "loai": "cu_dan" }
    }
  },
  "residents": {
    "UID_THE_CU_DAN": {
      "hoTen": "Nguyen Van A",
      "bienSo": "29A-12345",
      "trongBai": false
    }
  },
  "cards": {
    "UID_THE_KHACH": {
      "khoa": false,
      "ngayTao": 1700000000000
    }
  },
  "slots": {
    "A1": "co_xe",
    "A2": "giu_cho",
    "A3": "trong",
    "A4": "co_xe"
  },
  "sessions": {
    "UID_THE_KHACH": {
      "gioVao": 1700000000000,
      "thanhToan": {
        "trangThai": "cho_thanh_toan",
        "phi": 5000,
        "phut": 15,
        "gio": 1700000900000
      }
    }
  },
  "devices": {
    "gate-in": { "ip": "192.168.1.50", "lastSeen": 1700001000000, "rssi": -55, "heap": 185000 },
    "gate-out": { "ip": "192.168.1.51", "lastSeen": 1700001000000, "rssi": -58, "heap": 178000 },
    "lot": { "ip": "UART qua gate-in", "lastSeen": 1700001000000 }
  },
  "history": [
    {
      "ai": "UID_THE_KHACH",
      "doiTuong": "khach",
      "loai": "ra",
      "gioVao": 1700000000000,
      "gio": 1700001800000,
      "phut": 30,
      "phi": 5000,
      "ghiChu": "Cổng ra tự động (VietQR)"
    }
  ]
}
```

---

## 5. TỔNG KẾT CÁC ƯU ĐIỂM KỸ THUẬT NỔI BẬT

1. **Thiết kế lập trình phi chặn (100% Non-blocking)**: Không sử dụng hàm `delay()` làm tê liệt luồng xử lý; các thao tác điều khiển servo, phát âm còi buzzer, quét cảm biến, đọc thẻ RFID và cập nhật màn hình đều chạy trên các máy trạng thái mềm (State Machine) điều khiển bằng `millis()`.
2. **Khả năng chịu lỗi và tính độc lập cao (Fault Tolerance)**: Mô hình cache Flash kết hợp hàng đợi ngoại tuyến đảm bảo hệ thống vận hành liên tục 24/7 bất chấp sự cố rớt mạng WiFi hay gián đoạn máy chủ đám mây.
3. **Phân chia tải phần cứng hợp lý**: Node bãi đỗ tập trung giám sát ô đỗ với tần số cao, giảm thiểu nhiễu và dồn việc xử lý mạng cho Node cổng vào.
4. **An toàn giao thông tuyệt đối tại Barrier**: Cơ chế liên động 2 cảm biến quang (`IR_ARRIVE` và `IR_PASS`) ngăn ngừa hoàn toàn tình trạng cần barrier hạ xuống va chạm vào xe (chống kẹp xe).
5. **Tích hợp thanh toán hiện đại**: Phối hợp nhịp nhàng giữa nhúng IoT và Web quản lý để thực hiện thanh toán động qua mã chuẩn VietQR, nâng cao trải nghiệm người dùng và tự động hóa toàn bộ quy trình thu ngân.
