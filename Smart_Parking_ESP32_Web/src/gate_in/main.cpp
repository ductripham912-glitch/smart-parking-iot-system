#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>
#include "ParkingCore.h"
#include "secrets.h"
#include "lot_link.h"

// ================= Cấu hình Chân =================
static const uint8_t PIN_RC522_SS  = 5;    // SCK 18, MISO 19, MOSI 23 cố định theo SPI phần cứng ESP32
static const uint8_t PIN_RC522_RST = 4;
static const uint8_t PIN_SERVO     = 13;   // Tín hiệu PWM điều khiển Barrier
static const uint8_t PIN_IR_ARRIVE = 32;   // Cảm biến hồng ngoại 1: Xe đến trước barrier
static const uint8_t PIN_IR_PASS   = 33;   // Cảm biến hồng ngoại 2: Xe đã vượt qua barrier
static const uint8_t PIN_LED       = 2;    // LED báo trạng thái trên board
static const uint8_t PIN_BUZZER    = 14;   // Còi Buzzer báo âm thanh (Active buzzer)

// ================= Cấu hình Hoạt động =================
static const bool     IR_ACTIVE_LOW       = true;    // true: module báo LOW khi có vật cản
static const int      SERVO_CLOSED_DEG    = 90;       // Góc đóng barrier (nằm ngang)
static const int      SERVO_OPEN_DEG      = 180;      // Góc mở barrier (dựng đứng)
static const uint32_t IR_DEBOUNCE_MS      = 80;      // Chống rung tín hiệu cảm biến hồng ngoại
static const uint32_t READ_MS             = 50;      // Chu kỳ quét thẻ RFID RC522
static const uint32_t REPEAT_MS           = 2000;    // Bỏ qua đọc lặp lại cùng một thẻ trong 2s
static const uint32_t OPEN_TIMEOUT_MS     = 20000;   // Mở barrier 20s không có xe qua thì tự đóng
static const uint32_t CLOSE_DELAY_MS      = 1000;    // Xe qua hẳn sau 1s thì đóng barrier
static const bool     REQUIRE_CAR_AT_GATE = false;   // true: bắt buộc IR1 thấy xe mới cho quẹt thẻ

// ================= Bộ điều khiển Còi Buzzer (Non-blocking) =================
class BuzzerController {
private:
  uint8_t pin;
  uint8_t beepsRemaining;
  uint16_t onDurationMs;
  uint16_t offDurationMs;
  uint32_t nextToggleTime;
  bool isSounding;

public:
  void init(uint8_t buzzerPin) {
    pin = buzzerPin;
    beepsRemaining = 0;
    isSounding = false;
    nextToggleTime = 0;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }

  // Phát n tiếng bíp với thời gian bật và tắt
  void play(uint8_t count, uint16_t onMs, uint16_t offMs = 100) {
    if (count == 0) return;
    beepsRemaining = count;
    onDurationMs = onMs;
    offDurationMs = offMs;
    isSounding = true;
    digitalWrite(pin, HIGH);
    nextToggleTime = millis() + onDurationMs;
  }

  // 1 bíp ngắn: Thẻ hợp lệ / Mở cổng thành công
  void beepSuccess() {
    play(1, 150);
  }

  // 3 bíp dồn dập: Từ chối / Lỗi / Hết chỗ / Anti-passback
  void beepReject() {
    play(3, 100, 80);
  }

  // 1 bíp dài: Hết thời gian chờ xe (Timeout) hoặc cảnh báo
  void beepTimeout() {
    play(1, 600);
  }

  void update(uint32_t now) {
    if (beepsRemaining == 0) return;

    if ((int32_t)(now - nextToggleTime) >= 0) {
      if (isSounding) {
        digitalWrite(pin, LOW);
        isSounding = false;
        beepsRemaining--;
        if (beepsRemaining > 0) {
          nextToggleTime = now + offDurationMs;
        }
      } else {
        digitalWrite(pin, HIGH);
        isSounding = true;
        nextToggleTime = now + onDurationMs;
      }
    }
  }

  void stop() {
    beepsRemaining = 0;
    isSounding = false;
    digitalWrite(pin, LOW);
  }
};

static BuzzerController buzzer;

// ================= Cảm biến Hồng ngoại =================
struct Sensor {
  uint8_t pin;
  bool raw;
  bool stable;
  uint32_t changedAt;
};
static Sensor irArrive = {PIN_IR_ARRIVE, false, false, 0};
static Sensor irPass   = {PIN_IR_PASS, false, false, 0};

static bool readActive(uint8_t pin) {
  int v = digitalRead(pin);
  return IR_ACTIVE_LOW ? (v == LOW) : (v == HIGH);
}

static void sensorInit(Sensor& s) {
  pinMode(s.pin, INPUT_PULLUP);
  s.raw = readActive(s.pin);
  s.stable = s.raw;
  s.changedAt = millis();
}

static void sensorUpdate(Sensor& s, uint32_t now) {
  bool a = readActive(s.pin);
  if (a != s.raw) {
    s.raw = a;
    s.changedAt = now;
  } else if (s.stable != s.raw && now - s.changedAt >= IR_DEBOUNCE_MS) {
    s.stable = s.raw;
  }
}

// ================= Trạng thái Cổng =================
enum class State : uint8_t { Idle, Open };
enum class Who : uint8_t { None, Resident, Guest };

static MFRC522 rfid(PIN_RC522_SS, PIN_RC522_RST);
static Servo barrier;

static State state = State::Idle;
static Who openWho = Who::None;
static String openUid;
static uint32_t openedAt = 0;
static bool passed = false;
static uint32_t clearSince = 0;

static uint32_t rejectUntil = 0;
static int knownSessions = 0;   // Số phiên khách đang mở

static uint32_t lastRead = 0;
static uint32_t lastReport = 0;
static String lastUid;
static uint32_t lastUidAt = 0;

// ================= Tiện ích =================
static String uidToString(const MFRC522::Uid& u) {
  String s;
  s.reserve(u.size * 2);
  for (byte i = 0; i < u.size; i++) {
    if (u.uidByte[i] < 0x10) s += "0";
    s += String(u.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

// Ghi lịch sử lên Firebase
static void logHistory(const char* loai, const char* doiTuong, const String& uid, const char* note) {
  if (!Parking::timeValid()) return;
  String j;
  j.reserve(160);
  j += "{\"loai\":\"";
  j += loai;
  j += "\",\"doiTuong\":\"";
  j += doiTuong;
  j += "\",\"ai\":\"";
  j += uid;
  j += "\",\"gio\":";
  j += Parking::u64(Parking::nowMs());
  if (note != nullptr && note[0] != 0) {
    j += ",\"ghiChu\":\"";
    j += note;
    j += "\"";
  }
  j += "}";
  Parking::write(Parking::Op::Push, "/history", j);
}

static void triggerReject() { 
  rejectUntil = millis() + 1200; 
  buzzer.beepReject();
}

// LED: Sáng liên tục khi barrier mở, nhấp nháy nhanh khi bị từ chối
static void ledUpdate(uint32_t now) {
  bool on = false;
  if ((int32_t)(rejectUntil - now) > 0) on = ((now / 100) % 2) == 0;
  else if (state == State::Open) on = true;
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}

// Kiểm tra số khách đang trong bãi
static void guestState(const String& uid, int& count, bool& hasUid) {
  count = knownSessions;
  hasUid = false;
  if (Parking::online() && Parking::queueSize() == 0) {
    String out;
    Parking::ReadResult r = Parking::fbRead("/sessions", out);
    if (r == Parking::ReadResult::NotFound) {
      count = 0;              // không có nhánh sessions = bãi không có khách
      knownSessions = 0;
    } else if (r == Parking::ReadResult::Ok) {
      JsonDocument d;
      if (!deserializeJson(d, out)) {
        count = (int)d.as<JsonObject>().size();
        hasUid = uid.length() > 0 && d[uid].is<JsonObject>();
        knownSessions = count;
      }
    }
  }
}

// ================= Điều khiển Barrier =================
static void startOpen(Who who, const String& uid) {
  barrier.write(SERVO_OPEN_DEG);
  state = State::Open;
  openWho = who;
  openUid = uid;
  openedAt = millis();
  passed = false;
  clearSince = 0;
  buzzer.beepSuccess();
  Parking::setBusy(true);   // Tạm dừng đồng bộ nền để ưu tiên cảm biến xe
  Serial.printf("[gate-in] MO BARRIER cho %s (%s)\n", uid.c_str(), who == Who::Resident ? "Cư dân" : "Khách");
}

static void commitEntry() {
  if (openWho == Who::Resident) {
    Parking::setResidentInside(openUid, true);
    logHistory("vao", "cu_dan", openUid, "");
    Serial.printf("[gate-in] Cư dân %s đã vào bãi an toàn\n", openUid.c_str());
  } else if (openWho == Who::Guest) {
    String j;
    j.reserve(48);
    j += "{\"gioVao\":";
    j += Parking::u64(Parking::nowMs());
    j += "}";
    String path = "/sessions/" + openUid;
    Parking::write(Parking::Op::Set, path, j);
    knownSessions++;
    logHistory("vao", "khach", openUid, "");
    Serial.printf("[gate-in] Khách %s đã vào bãi (Số khách hiện tại: %d)\n", openUid.c_str(), knownSessions);
  }
}

static void finishClose(bool commit) {
  barrier.write(SERVO_CLOSED_DEG);
  if (commit) {
    commitEntry();
  } else {
    buzzer.beepTimeout();
  }
  state = State::Idle;
  openWho = Who::None;
  openUid = "";
  Parking::setBusy(false);
}

static void updateOpen(uint32_t now) {
  if (irPass.stable) {
    if (!passed) Serial.println("[gate-in] Xe đang đi qua barrier");
    passed = true;
  }
  bool carPresent = irArrive.stable || irPass.stable;

  if (passed) {
    if (carPresent) {
      clearSince = 0;
    } else {
      if (clearSince == 0) clearSince = now;
      if (now - clearSince >= CLOSE_DELAY_MS) finishClose(true);
    }
  } else if (now - openedAt >= OPEN_TIMEOUT_MS && !carPresent) {
    Serial.println("[gate-in] Quá thời gian chờ (Timeout), không có xe -> Đóng cổng");
    finishClose(false);
  }
}

// ================= Xử lý Thẻ =================
static void handleCard(const String& uid) {
  if (state != State::Idle) return;

  if (!Parking::cacheReady()) {
    Serial.println("[gate-in] Cache chưa sẵn sàng (đang tải dữ liệu từ Firebase)");
    triggerReject();
    return;
  }
  if (REQUIRE_CAR_AT_GATE && !irArrive.stable) {
    Serial.println("[gate-in] Chưa phát hiện xe đỗ trước cổng, bỏ qua thẻ");
    return;
  }

  // 1. Cư dân (thẻ gắn dưới gầm hoặc quẹt thẻ)
  if (Parking::residentExists(uid)) {
    if (Parking::residentInside(uid)) {
      Serial.printf("[gate-in] Cư dân %s: TỪ CHỐI (Anti-passback: Đã ở trong bãi)\n", uid.c_str());
      logHistory("tu_choi", "cu_dan", uid, "Đã ở trong bãi (anti-passback)");
      triggerReject();
      return;
    }
    startOpen(Who::Resident, uid);
    return;
  }

  // 2. Thẻ khách
  if (!Parking::cardRegistered(uid)) {
    Serial.printf("[gate-in] Thẻ %s: TỪ CHỐI (Chưa đăng ký trên hệ thống)\n", uid.c_str());
    logHistory("tu_choi", "khach", uid, "Thẻ chưa đăng ký");
    triggerReject();
    return;
  }
  if (Parking::cardLocked(uid)) {
    Serial.printf("[gate-in] Thẻ %s: TỪ CHỐI (Thẻ đang bị khóa)\n", uid.c_str());
    logHistory("tu_choi", "khach", uid, "Thẻ bị khóa");
    triggerReject();
    return;
  }
  if (!Parking::timeValid()) {
    Serial.println("[gate-in] Chưa đồng bộ giờ NTP chuẩn từ mạng");
    triggerReject();
    return;
  }

  int count;
  bool hasUid;
  guestState(uid, count, hasUid);
  long maxGuest = Parking::cfgLong("khachToiDa", 2);

  if (hasUid) {
    Serial.printf("[gate-in] Thẻ %s: TỪ CHỐI (Thẻ đang có phiên đỗ trong bãi)\n", uid.c_str());
    logHistory("tu_choi", "khach", uid, "Thẻ đang có phiên trong bãi");
    triggerReject();
    return;
  }
  if (count >= maxGuest) {
    Serial.printf("[gate-in] Thẻ %s: TỪ CHỐI (Đã hết chỗ đỗ cho khách: %d/%ld)\n", uid.c_str(), count, maxGuest);
    logHistory("tu_choi", "khach", uid, "Hết chỗ khách");
    triggerReject();
    return;
  }

  startOpen(Who::Guest, uid);
}

// Kiểm tra sức khỏe đầu đọc RC522 để tự động khởi tạo lại nếu lỗi tiếp xúc
static void checkRfidHealth() {
  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  if (ver == 0x00 || ver == 0xFF) {
    Serial.println("[gate-in] CẢNH BÁO: Mất kết nối RC522, đang khởi động lại...");
    rfid.PCD_Init();
  }
}

// ================= Setup & Loop =================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  buzzer.init(PIN_BUZZER);

  sensorInit(irArrive);
  sensorInit(irPass);

  ESP32PWM::allocateTimer(0);
  barrier.setPeriodHertz(50);
  barrier.attach(PIN_SERVO, 500, 2400);
  barrier.write(SERVO_CLOSED_DEG);

  SPI.begin();
  rfid.PCD_Init();
  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("[gate-in] RC522 Version=0x%02X %s\n", ver,
                (ver == 0x91 || ver == 0x92) ? "(OK)" : "(LỖI KẾT NỐI)");

  // Bíp khởi động hệ thống
  buzzer.play(2, 80, 80);

  Parking::Settings s;
  s.wifiList     = WIFI_LIST;
  s.wifiCount    = WIFI_COUNT;
  s.apiKey       = API_KEY;
  s.databaseUrl  = DATABASE_URL;
  s.userEmail    = USER_EMAIL;
  s.userPassword = USER_PASSWORD;
  s.deviceName   = "gate-in";
  LotLink::begin();
  Parking::begin(s);
}

void loop() {
  Parking::loop();

  uint32_t now = millis();
  buzzer.update(now);

  sensorUpdate(irArrive, now);
  sensorUpdate(irPass, now);

  if (state == State::Open) updateOpen(now);

  if (now - lastRead >= READ_MS) {
    lastRead = now;
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      String uid = uidToString(rfid.uid);
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
      if (uid != lastUid || now - lastUidAt >= REPEAT_MS) {
        lastUid = uid;
        lastUidAt = now;
        handleCard(uid);
      }
    }
  }

  ledUpdate(now);
  LotLink::loop(state == State::Idle);
  if (now - lastReport > 10000) {
    lastReport = now;
    checkRfidHealth();
    Serial.printf("[gate-in] WiFi=%d Online=%d Cache=%d HàngĐợi=%u | %s Khách=%d IR1=%d IR2=%d\n",
                  Parking::wifiConnected(), Parking::online(), Parking::cacheReady(),
                  (unsigned)Parking::queueSize(),
                  state == State::Open ? "MỞ" : "ĐÓNG", knownSessions, irArrive.stable, irPass.stable);
  }
}
