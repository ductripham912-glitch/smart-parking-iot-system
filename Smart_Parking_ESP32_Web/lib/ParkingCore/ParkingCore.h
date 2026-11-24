#pragma once
#include <Arduino.h>

namespace Parking {

struct WifiCred {
  const char* ssid;
  const char* password;
};

struct Settings {
  const WifiCred* wifiList;   // danh sách WiFi (xem secrets.h)
  size_t wifiCount;
  const char* apiKey;
  const char* databaseUrl;
  const char* userEmail;
  const char* userPassword;
  const char* deviceName;   // tên thiết bị, dùng để in log
};

// Loại lệnh ghi lên Firebase
enum class Op : uint8_t { Set, Push, Delete };

// Kết quả đọc một nhánh: Ok (có dữ liệu), NotFound (nhánh không tồn tại), Error (mất mạng hoặc lỗi khác)
enum class ReadResult : uint8_t { Ok, NotFound, Error };

// ----- Vòng đời -----
void begin(const Settings& s);   // gọi 1 lần trong setup()
void loop();                     // gọi liên tục trong loop()

// Báo "cổng đang bận" (barrier đang chạy, đang chờ xe qua, đang đọc thẻ...).
// Khi bận, loop() KHÔNG gửi hàng đợi và KHÔNG đồng bộ cache để khỏi chặn vòng lặp.
// Nhớ setBusy(false) khi xong việc.
void setBusy(bool busy);

// ----- Trạng thái -----
bool wifiConnected();
bool online();                   // WiFi + Firebase sẵn sàng
bool timeValid();                // đã lấy được giờ NTP
uint64_t nowMs();                // mili giây từ 1970 (UTC), 0 nếu chưa có giờ
String u64(uint64_t v);          // chuyển số 64 bit sang chuỗi
size_t queueSize();              // số lệnh ghi đang chờ gửi
uint32_t droppedCount();         // số lệnh ghi đã bị bỏ (lỗi quyền/sai dữ liệu hoặc hàng đợi đầy)

// ----- Ghi / đọc Firebase -----
// json là giá trị JSON dạng chuỗi:
//   object : {"gioVao":1791856800000}   (nên dùng cho mọi số lớn như mốc thời gian)
//   chuỗi  : "co_xe"                    (có dấu nháy kép)
//   bool   : true / false
//   số nhỏ : 5000                       (chỉ số nguyên 32 bit)
// Push chỉ dùng với object. Mất mạng thì lệnh được xếp hàng đợi (lưu cả vào flash),
// có mạng tự gửi, kể cả sau khi ESP32 khởi động lại.
void write(Op op, const String& path, const String& json = "");

// Đọc một nhánh, trả về chuỗi JSON ("{}" nếu nhánh trống). Chỉ chạy khi online.
bool fbGetJson(const String& path, String& out);

// Như fbGetJson nhưng phân biệt "nhánh không tồn tại" với "lỗi mạng". Không in log khi NotFound.
ReadResult fbRead(const String& path, String& out);

// ----- Bộ nhớ đệm (config, residents, cards) -----
bool syncCache();                              // tải đầy đủ từ Firebase (chặn cho đến khi xong)
bool cacheReady();                             // đã có dữ liệu cư dân trong bộ nhớ

bool   residentExists(const String& id);
// fresh = true: khi online sẽ đọc trực tiếp trongBai từ Firebase (đúng giữa các cổng).
// Offline hoặc đọc lỗi thì dùng cache.
bool   residentInside(const String& id, bool fresh = true);
void   setResidentInside(const String& id, bool inside);  // cập nhật cache + Firebase
String residentSlot(const String& id);                    // ô riêng của cư dân, ví dụ "A1"

bool cardRegistered(const String& uid);
bool cardLocked(const String& uid);

long cfgLong(const char* key, long defaultValue);         // ví dụ "donGiaMoiGio"

}  // namespace Parking