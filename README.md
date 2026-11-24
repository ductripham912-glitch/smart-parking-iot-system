# 🚗 IoT Smart Parking System (ESP32 - Firebase RTDB - Web Dashboard)

[![PlatformIO](https://img.shields.io/badge/PlatformIO-Supported-orange.svg)](https://platformio.org/)
[![ESP32](https://img.shields.io/badge/Hardware-ESP32--WROOM--32-red.svg)](https://www.espressif.com/)
[![Firebase](https://img.shields.io/badge/Backend-Firebase%20RTDB-yellow.svg)](https://firebase.google.com/)
[![Web Dashboard](https://img.shields.io/badge/Frontend-TailwindCSS%20%7C%20JS-blue.svg)](https://tailwindcss.com/)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

Hệ thống quản lý bãi đỗ xe thông minh toàn diện (**End-to-End IoT Solution**), tích hợp chặt chẽ giữa phần cứng đa vi điều khiển **ESP32**, dịch vụ đám mây thời gian thực **Firebase Realtime Database** và giao diện điều hành **Web Dashboard** hiện đại hỗ trợ thanh toán tự động qua **VietQR**.

---

## 🌟 Tính Năng Nổi Bật (Key Highlights)

* **Kiến trúc Đa Vi Điều Khiển Phân Tán (Multi-MCU Architecture):**
  * Tách biệt các phân hệ Cổng vào (*Gate In*), Cổng ra (*Gate Out*) và Giám sát ô đỗ (*Lot*) trên các ESP32 độc lập để đảm bảo hiệu năng xử lý thời gian thực và độ trễ phản hồi cực thấp.
  * Node Bãi đỗ giao tiếp với Node Cổng qua kênh truyền **UART2** tốc độ cao với giao thức kiểm tra toàn vẹn **Checksum XOR**.
* **Độ Sẵn Sàng Cao & Hoạt Động Ngoại Tuyến (Offline-First / Fault Tolerance):**
  * **Bộ nhớ đệm Flash (Blob Cache):** Tự động đồng bộ và lưu thông tin cư dân, thẻ khách và cấu hình vào NVS Flash. Cổng vẫn nhận thẻ và mở barrier bình thường ngay cả khi mất kết nối Internet.
  * **Hàng đợi vòng lưu Flash (Persistent Circular Queue):** Lưu trữ đến 30 giao dịch chưa thể gửi khi rớt mạng; tự động đồng bộ bù (*retry exponential backoff*) ngay khi có mạng trở lại mà không lo mất mát dữ liệu hay thất thoát doanh thu kể cả khi mất nguồn.
* **Cơ Chế Nhường Quyền CPU (`setBusy`):**
  * Tự động hoãn các tác vụ nền tiêu tốn CPU (quét WiFi, đồng bộ cache) trong lúc xe đang qua cổng hoặc đang quẹt thẻ để ưu tiên 100% tài nguyên xử lý cảm biến và servo.
* **An Ninh & Chống Gian Lận (Anti-Passback):**
  * Ngăn chặn hành vi quẹt thẻ 2 lần liên tiếp ở cùng một cổng (cả cư dân lẫn thẻ khách).
  * Tự động kiểm soát sức chứa tối đa của bãi đỗ xe.
* **Cơ Chế Chống Kẹp Xe An Toàn Tuyệt Đối (Safety Interlock):**
  * Hệ thống liên động 2 cảm biến quang hành trình (`IR_ARRIVE` và `IR_PASS`) cùng thuật toán chống rung tín hiệu (*Debounce*) đảm bảo cần barrier chỉ hạ xuống khi xe đã rời khỏi cổng hoàn toàn.
  * Tự động đóng cổng an toàn và hủy phiên nếu quá 20 giây (*Timeout*) mà phương tiện không tiến vào.
* **Tích Hợp Thanh Toán Không Tiền Mặt (VietQR Auto Payment):**
  * Cổng ra tự động tính tiền đỗ theo block phút/giờ và tạo yêu cầu thanh toán trên Firebase.
  * Web Dashboard tự động kết xuất mã **VietQR chuẩn Napas247** động tương ứng với số tiền.
  * Cơ chế ân hạn **120 giây (Paid Grace Period)** cho phép mở lại barrier miễn phí nếu khách đã thanh toán nhưng xe gặp sự cố chưa kịp qua cổng.
* **Kiến Trúc Lập Trình 100% Phi Chặn (Non-blocking):**
  * Không sử dụng hàm `delay()`. Toàn bộ quá trình quét RFID, nhịp còi buzzer, cảm biến IR, màn hình OLED/LCD và kết nối Firebase đều chạy trên các máy trạng thái (*State Machine*) điều khiển bởi timer mềm `millis()`.

---

## 🏛️ Sơ Đồ Kiến Trúc Hệ Thống

```
                     ┌──────────────────────────────────────────────┐
                     │         Firebase Realtime Database           │
                     │          & Web Management Dashboard          │
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

---

## 🔌 Cấu Hình Chân Phần Cứng (Pinout)

### 1. Cổng Vào (`gate_in`)
| Thiết bị | Chân ESP32 | Giao thức / Chức năng | Vai trò |
| :--- | :--- | :--- | :--- |
| **MFRC522** | SCK: 18, MISO: 19, MOSI: 23, SS: 5, RST: 4 | SPI Hardware | Quét thẻ RFID vào |
| **Servo Barrier** | GPIO 13 | PWM 50Hz | Đóng (90°), Mở (180°) |
| **IR Arrive** | GPIO 32 | Digital Input (Pull-up) | Cảm biến trước barrier |
| **IR Pass** | GPIO 33 | Digital Input (Pull-up) | Cảm biến sau barrier |
| **LCD 1602** | SDA: 21, SCL: 22 | I2C (50kHz, Addr: 0x27/0x3F) | Hiển thị chỗ trống & gợi ý ô |
| **Buzzer** | GPIO 14 | Digital Output | Âm báo trạng thái |
| **Status LED** | GPIO 2 | Digital Output | Đèn LED tín hiệu |
| **UART2 Link** | RX: 16, TX: 17 | Serial2 (9600 baud) | Nhận dữ liệu từ Node Bãi đỗ |

### 2. Cổng Ra (`gate_out`)
| Thiết bị | Chân ESP32 | Giao thức / Chức năng | Vai trò |
| :--- | :--- | :--- | :--- |
| **MFRC522** | SCK: 18, MISO: 19, MOSI: 23, SS: 5, RST: 4 | SPI Hardware | Quét thẻ RFID ra |
| **Servo Barrier** | GPIO 13 | PWM 50Hz | Đóng (90°), Mở (180°) |
| **IR Arrive** | GPIO 32 | Digital Input (Pull-up) | Cảm biến trước barrier |
| **IR Pass** | GPIO 33 | Digital Input (Pull-up) | Cảm biến sau barrier |
| **OLED SSD1306** | SDA: 21, SCL: 22 | I2C (400kHz, Addr: 0x3C) | Hiển thị cước phí & mã QR |
| **Buzzer** | GPIO 14 | Digital Output | Âm báo thanh toán / kết quả |
| **Status LED** | GPIO 2 | Digital Output | Đèn LED tín hiệu |

### 3. Bãi Đỗ Xe (`lot`)
| Thiết bị | Chân ESP32 | Vai trò |
| :--- | :--- | :--- |
| **IR Ô A1** | GPIO 32 | Ô Cư dân (Active LOW khi có xe) |
| **IR Ô A2** | GPIO 33 | Ô Cư dân (Active LOW khi có xe) |
| **IR Ô A3** | GPIO 25 | Ô Khách vãng lai (Active LOW khi có xe) |
| **IR Ô A4** | GPIO 26 | Ô Khách vãng lai (Active LOW khi có xe) |
| **UART2 TX** | GPIO 17 | Gửi chuỗi `$dddd*HH\n` sang Gate In (9600 baud) |

---

## 📁 Cấu Trúc Thư Mục Dự Án

```
Smart_Parking/
├── Smart_Parking_ESP32_Web/     # Mã nguồn Firmware PlatformIO cho ESP32
│   ├── include/
│   │   ├── secrets.example.h    # File mẫu cấu hình WiFi & Firebase
│   │   └── secrets.h            # File chứa thông tin mật (đã gitignore)
│   ├── lib/
│   │   └── ParkingCore/         # Thư viện dùng chung (NTP, Cache, Queue, Heartbeat)
│   ├── src/
│   │   ├── gate_in/             # Firmware Cổng Vào & module lot_link
│   │   ├── gate_out/            # Firmware Cổng Ra & module thanh toán
│   │   ├── lot/                 # Firmware Bãi đỗ xe
│   │   └── reset/               # Tiện ích format bộ nhớ NVS Flash
│   └── platformio.ini           # Cấu hình build đa môi trường
├── Web/                         # Giao diện Web điều hành (Dashboard)
│   ├── index.html               # Ứng dụng Web Single-Page (TailwindCSS)
│   └── du_lieu_mau.json         # Dữ liệu mẫu khởi tạo hệ thống
├── Schematic/                   # Bản vẽ thiết kế mạch điện nguyên lý
│   └── Schematic_System/        # Project KiCad hoàn chỉnh
├── bang_uid_6_the.txt           # Danh mục UID các loại thẻ thử nghiệm
├── du_lieu_sach.json            # Dữ liệu khởi tạo Firebase mẫu
└── README.md                    # Tài liệu hướng dẫn dự án
```

---

## 🚀 Hướng Dẫn Cài Đặt & Chạy Thử

### 1. Chuẩn bị Phần cứng
* 3 x Board ESP32 (NodeMCU-32S / ESP32 DevKit V1)
* 2 x Module RFID RC522 (kèm thẻ Mifare Classic 1K / NTAG213)
* 2 x Động cơ Servo (SG90 / MG996R) làm barrier
* 6 x Cảm biến vật cản hồng ngoại (IR Sensor)
* 1 x Màn hình LCD 1602 kèm module I2C
* 1 x Màn hình OLED SSD1306 0.96 inch I2C (128x64)
* 2 x Còi Buzzer 5V, 2 x Đèn LED báo hiệu

### 2. Cài đặt Firmware ESP32
1. Mở thư mục `Smart_Parking_ESP32_Web` trong **VS Code** có cài sẵn extension **PlatformIO IDE**.
2. Sao chép file cấu hình:
   ```bash
   cp include/secrets.example.h include/secrets.h
   ```
3. Mở file `include/secrets.h` và cập nhật thông tin mạng WiFi cùng URL Firebase của bạn.
4. Nạp code cho từng board ESP32 tương ứng qua PlatformIO:
   * **Cổng Vào:** Chọn env `gate_in` -> `Upload`
   * **Cổng Ra:** Chọn env `gate_out` -> `Upload`
   * **Bãi Đỗ Xe:** Chọn env `lot` -> `Upload`

### 3. Chạy Web Dashboard
* Mở trực tiếp file `Web/index.html` trên trình duyệt (hoặc deploy lên GitHub Pages / Firebase Hosting).
* Đăng nhập với tài khoản quản trị và quan sát trạng thái ô đỗ, lịch sử ra vào và hóa đơn theo thời gian thực.

---

## 👨‍💻 Tác Giả & Liên Hệ

* **Tác giả:** Phạm Đức Trí
* **Email:** ductripham912@gmail.com
* **Mục tiêu:** Dự án đồ án tốt nghiệp / Portfolio xin việc ngành Kỹ thuật Nhúng & IoT (Embedded Systems & IoT Engineer).
