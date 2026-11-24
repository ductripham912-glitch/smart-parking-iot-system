#pragma once
#include "ParkingCore.h"

// ================= CẤU HÌNH THIẾT BỊ VÀ KẾT NỐI =================
// Đổi tên file này thành secrets.h và điền thông tin của bạn

// Danh sách WiFi đặt sẵn: ESP32 tự động quét và kết nối vào mạng có sóng mạnh nhất
static const Parking::WifiCred WIFI_LIST[] = {
  {"YOUR_WIFI_SSID_1", "YOUR_WIFI_PASSWORD_1"},
  {"YOUR_WIFI_SSID_2", "YOUR_WIFI_PASSWORD_2"},
  {""}
};
static const size_t WIFI_COUNT = sizeof(WIFI_LIST) / sizeof(WIFI_LIST[0]);

// Thông tin kết nối Firebase Realtime Database
#define API_KEY        "YOUR_FIREBASE_WEB_API_KEY"
#define DATABASE_URL   "https://your-project-id-default-rtdb.firebaseio.com"

// Tài khoản Firebase Authentication (Email/Password) cấp quyền cho ESP32
#define USER_EMAIL     "esp32gate@yourdomain.com"
#define USER_PASSWORD  "your_password"
