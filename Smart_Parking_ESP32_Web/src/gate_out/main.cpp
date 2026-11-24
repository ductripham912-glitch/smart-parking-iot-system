#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <math.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "ParkingCore.h"
#include "secrets.h"

// ================= Cấu hình Chân =================
static const uint8_t PIN_RC522_SS  = 5;     // SCK 18, MISO 19, MOSI 23 dùng chung bus SPI
static const uint8_t PIN_RC522_RST = 4;
static const uint8_t PIN_OLED_SDA  = 21;    // OLED I2C
static const uint8_t PIN_OLED_SCL  = 22;
static const uint8_t OLED_ADDR    = 0x3C;   // một số module dùng 0x3D
static const uint8_t PIN_SERVO     = 13;    // Tín hiệu PWM điều khiển Barrier
static const uint8_t PIN_IR_ARRIVE = 32;    // Cảm biến hồng ngoại 1: Xe đang ở chỗ barrier
static const uint8_t PIN_IR_PASS   = 33;    // Cảm biến hồng ngoại 2: Xe đã qua barrier
static const uint8_t PIN_LED       = 2;     // LED báo trạng thái trên board
static const uint8_t PIN_BUZZER    = 14;    // Còi Buzzer báo âm thanh (Active buzzer)

// ================= Cấu hình Hoạt động =================
static const bool     IR_ACTIVE_LOW       = true;    // true: module báo LOW khi có xe
static const int      SERVO_CLOSED_DEG    = 90;
static const int      SERVO_OPEN_DEG      = 180;
static const uint32_t IR_DEBOUNCE_MS      = 80;
static const uint32_t READ_MS             = 50;      // Chu kỳ quét thẻ RFID RC522
static const uint32_t REPEAT_MS           = 2000;    // Cùng một thẻ trong 2s chỉ tính 1 lần
static const uint32_t PAYMENT_SHOW_MS     = 2500;    // Thời gian hiện màn hình miễn phí trước khi mở
static const uint32_t PAY_TIMEOUT_MS      = 60000;   // Chờ thanh toán QR tối đa 60s
static const uint32_t PAY_POLL_MS         = 800;     // Chu kỳ hỏi Firebase xem Web đã thu tiền chưa
static const uint32_t PAID_GRACE_MS       = 120000;  // Khách đã trả tiền mà chưa kịp qua: cho mở lại barrier trong 2 phút
static const uint32_t OPEN_TIMEOUT_MS     = 20000;   // Mở barrier 20s không có xe qua thì tự đóng
static const uint32_t CLOSE_DELAY_MS      = 1000;    // Hết xe 1s thì đóng barrier
static const uint32_t MSG_SHOW_MS         = 2500;    // Thời gian hiện thông báo trước khi về màn hình chờ
static const uint32_t IDLE_REDRAW_MS      = 3000;    // Vẽ lại màn hình chờ định kỳ
static const uint32_t SESS_SYNC_MS        = 15000;   // Chu kỳ lưu bản sao /sessions vào RAM
static const bool     REQUIRE_CAR_AT_GATE = false;   // true: chỉ nhận thẻ khi IR 1 thấy xe

// ================= Màn hình OLED SSD1306 I2C =================
static Adafruit_SSD1306 display(128, 64, &Wire, -1);

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

  void play(uint8_t count, uint16_t onMs, uint16_t offMs = 100) {
    if (count == 0) return;
    beepsRemaining = count;
    onDurationMs = onMs;
    offDurationMs = offMs;
    isSounding = true;
    digitalWrite(pin, HIGH);
    nextToggleTime = millis() + onDurationMs;
  }

  void beepSuccess() {
    play(1, 150);
  }

  void beepPaying() {
    play(2, 90, 80);
  }

  void beepReject() {
    play(3, 100, 80);
  }

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
enum class State : uint8_t { Idle, Paying, Open };
enum class Who : uint8_t { None, Resident, Guest };

struct ExitInfo {
  uint64_t gioVao;
  long phut;
  long phi;
};

static MFRC522 rfid(PIN_RC522_SS, PIN_RC522_RST);
static Servo barrier;

static State state = State::Idle;
static Who openWho = Who::None;
static String openUid;
static ExitInfo exitInfo = {0, 0, 0};
static uint32_t payStartedAt = 0;
static uint32_t lastPayCheck = 0;
static bool webClosed = false;          // true: Web đã thu tiền và tự đóng phiên + ghi lịch sử
static String paidUid;                  // thẻ đã trả tiền nhưng chưa qua barrier
static uint32_t paidUntil = 0;
static bool paidConfirmed = false;      // Web đã xác nhận thanh toán trong lần mở barrier này
static bool paidWebClosed = false;      // lúc đó phiên đã bị Web xóa chưa
static ExitInfo paidInfo = {0, 0, 0};   // phí/phút của lần đã thanh toán
static uint32_t openedAt = 0;
static bool passed = false;
static uint32_t clearSince = 0;

static uint32_t rejectUntil = 0;
static uint32_t msgUntil = 0;
static uint32_t lastIdleDraw = 0;

static JsonDocument sessCache;      // Bản sao /sessions trong RAM khi offline
static uint32_t lastSessSync = 0;
static bool sessSyncedOnce = false;

static uint32_t lastRead = 0;
static uint32_t lastReport = 0;
static String lastUid;
static uint32_t lastUidAt = 0;

// ================= Khai báo trước =================
static void commitExit();
static void finishClose(bool commit);

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

// Định dạng tiền: 10000 -> "10.000"
static void fmtMoney(long v, char* out, size_t n) {
  char raw[16];
  snprintf(raw, sizeof(raw), "%ld", v);
  size_t len = strlen(raw), j = 0;
  for (size_t i = 0; i < len && j + 2 < n; i++) {
    out[j++] = raw[i];
    size_t rest = len - 1 - i;
    if (rest > 0 && rest % 3 == 0) out[j++] = '.';
  }
  out[j] = 0;
}

// Phí tối thiểu 1 block: xe vừa vào đã tạm tính 1 block (ví dụ 5.000 đ), không có thời gian miễn phí.
static long calcFee(double minutes) {
  long block = Parking::cfgLong("blockPhut", 60);
  long price = Parking::cfgLong("donGiaMoiGio", 5000);
  if (block < 1) block = 1;
  if (minutes < 0) minutes = 0;
  long blocks = (long)ceil(minutes / (double)block);
  if (blocks < 1) blocks = 1;
  return blocks * price;
}

static void oled(const char* title, const char* l2, const char* l3, const char* l4) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print(title);
  display.setTextSize(1);
  display.setCursor(0, 26);
  display.print(l2);
  display.setCursor(0, 38);
  display.print(l3);
  display.setCursor(0, 52);
  display.print(l4);
  display.display();
}

static void showMsg(const char* title, const char* l2, const char* l3, const char* l4) {
  oled(title, l2, l3, l4);
  msgUntil = millis() + MSG_SHOW_MS;
  lastIdleDraw = 0;
}

static void drawIdle() {
  char b[24];
  unsigned guests = sessCache.is<JsonObject>() ? (unsigned)sessCache.as<JsonObject>().size() : 0;
  snprintf(b, sizeof(b), "%s  Khach:%u", Parking::online() ? "Online" : "Offline", guests);
  oled("CONG RA", "Quet the de ra", "", b);
}

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

static void logGuestExit(const String& uid, const ExitInfo& e) {
  if (!Parking::timeValid()) return;
  String j;
  j.reserve(220);
  j += "{\"loai\":\"ra\",\"doiTuong\":\"khach\",\"ai\":\"";
  j += uid;
  j += "\",\"gio\":";
  j += Parking::u64(Parking::nowMs());
  j += ",\"gioVao\":";
  j += Parking::u64(e.gioVao);
  j += ",\"phut\":";
  j += String(e.phut);
  j += ",\"phi\":";
  j += String(e.phi);
  j += ",\"phat\":0,\"ghiChu\":\"Cổng ra tự động (VietQR)\"}";
  Parking::write(Parking::Op::Push, "/history", j);
}

static void triggerReject() {
  rejectUntil = millis() + 1200;
  buzzer.beepReject();
}

static void ledUpdate(uint32_t now) {
  bool on = false;
  if ((int32_t)(rejectUntil - now) > 0) on = ((now / 100) % 2) == 0;
  else if (state != State::Idle) on = true;
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}

static void reject(const String& uid, const char* l2, const char* l3, const char* who, const char* historyNote) {
  Serial.printf("[gate-out] %s: TỪ CHỐI (%s %s)\n", uid.c_str(), l2, l3);
  showMsg("TU CHOI", l2, l3, uid.c_str());
  triggerReject();
  if (who != nullptr && historyNote != nullptr) logHistory("tu_choi", who, uid, historyNote);
}

// ================= Phiên khách =================
static void syncSessions(uint32_t now) {
  if (!Parking::online() || Parking::queueSize() != 0) return;
  if (lastSessSync != 0 && now - lastSessSync < SESS_SYNC_MS) return;
  lastSessSync = now;
  String out;
  Parking::ReadResult r = Parking::fbRead("/sessions", out);
  if (r == Parking::ReadResult::NotFound) {
    sessCache.to<JsonObject>();   // Firebase xác nhận không có phiên nào
    sessSyncedOnce = true;
  } else if (r == Parking::ReadResult::Ok) {
    JsonDocument d;
    if (!deserializeJson(d, out)) {
      sessCache = d;
      sessSyncedOnce = true;
    }
  }
}

static bool loadSession(const String& uid, uint64_t& gioVao) {
  if (Parking::online() && Parking::queueSize() == 0) {
    String path = "/sessions/" + uid;
    String out;
    Parking::ReadResult r = Parking::fbRead(path, out);
    if (r == Parking::ReadResult::Ok) {
      JsonDocument d;
      if (!deserializeJson(d, out)) {
        double g = d["gioVao"] | 0.0;
        if (g > 0) {
          gioVao = (uint64_t)g;
          sessCache[uid]["gioVao"] = g;
          return true;
        }
      }
      sessCache.remove(uid);
      return false;
    }
    if (r == Parking::ReadResult::NotFound) {
      sessCache.remove(uid);   // Firebase xác nhận không có phiên, không dùng bản sao RAM đã cũ
      return false;
    }
    // Lỗi mạng hoặc lỗi khác: dùng bản sao trong RAM bên dưới
  }
  double g = sessCache[uid]["gioVao"] | 0.0;
  if (g > 0) {
    gioVao = (uint64_t)g;
    return true;
  }
  return false;
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
  Parking::setBusy(true);

  if (who == Who::Resident) oled("CU DAN", "Tam biet", "Xin moi ra", "");
  else oled("MOI RA", "Da thanh toan", "Cam on quy khach", "");
  Serial.printf("[gate-out] MO BARRIER cho %s (%s)\n", uid.c_str(), who == Who::Resident ? "Cư dân" : "Khách");
}

static void startPaying(const String& uid) {
  state = State::Paying;
  openWho = Who::Guest;
  openUid = uid;
  payStartedAt = millis();
  lastPayCheck = 0;
  webClosed = false;
  buzzer.beepPaying();
  Parking::setBusy(true);

  char money[16], lineFee[24];
  fmtMoney(exitInfo.phi, money, sizeof(money));
  snprintf(lineFee, sizeof(lineFee), "Phi: %s d", money);

  if (exitInfo.phi == 0) {
    // Chỉ xảy ra khi donGiaMoiGio = 0 trong config
    oled("MIEN PHI", "Khong thu phi", "Dang mo cong...", "");
    Serial.printf("[gate-out] Khách %s: Miễn phí gửi xe (%ld phút)\n", uid.c_str(), exitInfo.phut);
  } else {
    oled("THANH TOAN", lineFee, "Quet ma QR tren Web", "Cho xac nhan...");
    Serial.printf("[gate-out] Khách %s: Đỗ %ld phút, phí %ld đồng -> Chờ xác nhận QR trên Web...\n",
                  uid.c_str(), exitInfo.phut, exitInfo.phi);

    String payJson;
    payJson.reserve(128);
    payJson += "{\"trangThai\":\"cho_thanh_toan\",\"phi\":";
    payJson += String(exitInfo.phi);
    payJson += ",\"phut\":";
    payJson += String(exitInfo.phut);
    payJson += ",\"gio\":";
    payJson += Parking::u64(Parking::nowMs());
    payJson += "}";
    Parking::write(Parking::Op::Set, "/sessions/" + uid + "/thanhToan", payJson);
  }
}

static void updatePaying(uint32_t now) {
  // 1. Trường hợp miễn phí
  if (exitInfo.phi == 0) {
    if (now - payStartedAt >= PAYMENT_SHOW_MS) {
      startOpen(Who::Guest, openUid);
    }
    return;
  }

  // 2. Có phí: hỏi Firebase định kỳ xem Web đã thu tiền chưa
  if (now - lastPayCheck >= PAY_POLL_MS) {
    lastPayCheck = now;

    if (Parking::online()) {
      String path = "/sessions/" + openUid;
      String out;
      Parking::ReadResult r = Parking::fbRead(path, out);

      if (r == Parking::ReadResult::NotFound) {
        // Web đã xóa phiên và ghi lịch sử -> coi là đã thanh toán, không ghi lại lịch sử
        webClosed = true;
        paidConfirmed = true;
        sessCache.remove(openUid);
        Serial.println("[gate-out] Web đã thu tiền và đóng phiên -> mở barrier");
        startOpen(Who::Guest, openUid);
        return;
      }

      if (r == Parking::ReadResult::Ok) {
        JsonDocument d;
        if (!deserializeJson(d, out)) {
          const char* st = d["thanhToan"]["trangThai"] | "";
          if (strcmp(st, "da_thanh_toan") == 0) {
            // Web chỉ đánh dấu đã thanh toán, phiên còn nguyên: cổng ra tự đóng phiên và ghi lịch sử
            paidConfirmed = true;
            Serial.println("[gate-out] Web đã xác nhận thanh toán -> mở barrier");
            startOpen(Who::Guest, openUid);
            return;
          }
        }
      }
    }
  }

  // 3. Quá 60 giây không ai thanh toán -> hủy yêu cầu thanh toán
  if (now - payStartedAt >= PAY_TIMEOUT_MS) {
    Serial.println("[gate-out] Quá thời gian chờ thanh toán (Timeout 60s) -> Hủy bỏ");
    String reqPath = "/sessions/" + openUid + "/thanhToan";
    Parking::write(Parking::Op::Delete, reqPath);
    finishClose(false);
  }
}

static void commitExit() {
  if (openWho == Who::Resident) {
    Parking::setResidentInside(openUid, false);
    logHistory("ra", "cu_dan", openUid, "");
    Serial.printf("[gate-out] Cư dân %s đã rời khỏi bãi\n", openUid.c_str());
  } else if (openWho == Who::Guest) {
    if (webClosed) {
      // Web đã thu tiền, xóa phiên và ghi lịch sử: không ghi lại để tránh cộng doanh thu hai lần
      sessCache.remove(openUid);
      Serial.printf("[gate-out] Khách %s đã ra (Web đã thu %ld đồng)\n", openUid.c_str(), exitInfo.phi);
    } else {
      String path = "/sessions/" + openUid;
      Parking::write(Parking::Op::Delete, path);
      logGuestExit(openUid, exitInfo);
      sessCache.remove(openUid);
      Serial.printf("[gate-out] Khách %s đã ra, thu %ld đồng\n", openUid.c_str(), exitInfo.phi);
    }
    paidUid = "";
  }
}

static void finishClose(bool commit) {
  barrier.write(SERVO_CLOSED_DEG);
  if (commit) {
    commitExit();
    showMsg("TAM BIET", "Cam on quy khach", "", "");
  } else {
    buzzer.beepTimeout();
    if (openWho == Who::Guest && paidConfirmed) {
      // Đã trả tiền nhưng xe chưa qua: nhớ lại để quét lại là mở, không phải trả lần nữa
      paidUid = openUid;
      paidUntil = millis() + PAID_GRACE_MS;
      paidInfo = exitInfo;
      paidWebClosed = webClosed;
      showMsg("HET GIO", "Da thanh toan", "Quet lai de ra", "");
    } else {
      showMsg("HET GIO", "Chua thanh toan", "Quet lai de tra", "");
    }
  }
  state = State::Idle;
  openWho = Who::None;
  openUid = "";
  webClosed = false;
  paidConfirmed = false;
  Parking::setBusy(false);
}

static void updateOpen(uint32_t now) {
  if (irPass.stable) {
    if (!passed) Serial.println("[gate-out] Xe đang vượt qua barrier");
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
    Serial.println("[gate-out] Quá thời gian chờ (Timeout) -> Đóng barrier");
    finishClose(false);
  }
}

// ================= Xử lý Thẻ =================
static void handleCard(const String& uid) {
  if (state != State::Idle) return;

  if (!Parking::cacheReady()) {
    reject(uid, "Chua dong bo", "du lieu", nullptr, nullptr);
    return;
  }
  if (REQUIRE_CAR_AT_GATE && !irArrive.stable) {
    Serial.println("[gate-out] Chưa thấy xe tại cổng, bỏ qua thẻ");
    return;
  }

  // 1. Cư dân
  if (Parking::residentExists(uid)) {
    if (!Parking::residentInside(uid)) {
      reject(uid, "Chua o trong bai", "(anti-passback)", "cu_dan", "Chưa ở trong bãi (anti-passback)");
      return;
    }
    startOpen(Who::Resident, uid);
    return;
  }

  // 2. Thẻ khách
  if (!Parking::cardRegistered(uid)) {
    reject(uid, "The chua dang ky", "", "khach", "Thẻ chưa đăng ký");
    return;
  }
  if (Parking::cardLocked(uid)) {
    reject(uid, "The da bi khoa", "Lien he quan ly", "khach", "Thẻ bị khóa");
    return;
  }
  if (paidUid == uid && (int32_t)(paidUntil - millis()) > 0) {
    Serial.printf("[gate-out] %s: đã thanh toán trước đó, mở lại barrier\n", uid.c_str());
    exitInfo = paidInfo;          // giữ đúng số tiền đã trả cho lịch sử
    webClosed = paidWebClosed;    // phiên đã bị Web xóa hay chưa
    paidConfirmed = true;
    startOpen(Who::Guest, uid);
    return;
  }
  if (!Parking::timeValid()) {
    reject(uid, "Chua co gio", "Thu lai sau", nullptr, nullptr);
    return;
  }

  uint64_t gioVao = 0;
  if (!loadSession(uid, gioVao)) {
    reject(uid, "Khong co phien", "trong bai", "khach", "Thẻ không có phiên trong bãi");
    return;
  }

  double minutes = ((double)Parking::nowMs() - (double)gioVao) / 60000.0;
  if (minutes < 0) minutes = 0;
  exitInfo.gioVao = gioVao;
  exitInfo.phut = (long)floor(minutes);
  exitInfo.phi = calcFee(minutes);
  startPaying(uid);
}

// ================= Lệnh mở cổng từ Web (khách mất thẻ) =================
static const uint32_t CMD_POLL_MS    = 1500;     // Chu kỳ hỏi Firebase xem Web có lệnh mở cổng không
static const uint32_t CMD_MAX_AGE_MS = 120000;   // Lệnh cũ hơn 2 phút thì bỏ qua
static uint32_t lastCmdCheck = 0;

static void openByAdmin(const String& uid) {
  barrier.write(SERVO_OPEN_DEG);
  state = State::Open;
  openWho = Who::Guest;
  openUid = uid;
  webClosed = true;          // Web đã xóa phiên và ghi lịch sử, cổng ra không ghi lại
  paidConfirmed = false;
  openedAt = millis();
  passed = false;
  clearSince = 0;
  buzzer.beepSuccess();
  Parking::setBusy(true);
  oled("MOI RA", "Da xu ly mat the", "Cam on quy khach", "");
  Serial.printf("[gate-out] MO BARRIER theo lenh Web (mat the %s)\n", uid.c_str());
}

static void checkAdminCommand(uint32_t now) {
  if (state != State::Idle) return;
  if (now - lastCmdCheck < CMD_POLL_MS) return;
  lastCmdCheck = now;
  if (!Parking::online()) return;

  String out;
  if (Parking::fbRead("/commands/gate_out", out) != Parking::ReadResult::Ok) return;

  JsonDocument d;
  if (deserializeJson(d, out)) return;
  const char* lenh = d["lenh"] | "";
  String uid = d["ai"] | "";
  double gio = d["gio"] | 0.0;

  // Xóa lệnh ngay để không mở lặp lại
  Parking::write(Parking::Op::Delete, "/commands/gate_out");

  if (strcmp(lenh, "open") != 0) return;
  if (Parking::timeValid() && gio > 0) {
    double age = (double)Parking::nowMs() - gio;
    if (age > (double)CMD_MAX_AGE_MS || age < -(double)CMD_MAX_AGE_MS) {
      Serial.println("[gate-out] Lệnh mở cổng đã cũ, bỏ qua");
      return;
    }
  }
  sessCache.remove(uid);
  openByAdmin(uid);
}

// Kiểm tra tiếp xúc phần cứng RC522
static void checkRfidHealth() {
  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  if (ver == 0x00 || ver == 0xFF) {
    Serial.println("[gate-out] CẢNH BÁO: Mất kết nối RC522, đang khởi động lại...");
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

  // Kéo cao chân CS của RC522 trước khi khởi động bus SPI
  pinMode(PIN_RC522_SS, OUTPUT);
  digitalWrite(PIN_RC522_SS, HIGH);

  SPI.begin();

  // OLED I2C (bus riêng, không dùng chung với RC522)
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  Wire.setClock(400000);

  Wire.beginTransmission(OLED_ADDR);
  bool oledAck = (Wire.endTransmission() == 0);

  // Luôn gọi begin() để cấp phát bộ đệm, tránh lỗi khi vẽ về sau
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[gate-out] LỖI: không cấp phát được bộ đệm OLED");
  }
  if (!oledAck) {
    Serial.println("[gate-out] KHÔNG thấy OLED trên I2C (kiểm tra SDA=21, SCL=22, VCC, GND, địa chỉ 0x3C/0x3D)");
  }
  oled("SMART", "Parking - Cong ra", "Dang khoi dong...", "");

  rfid.PCD_Init();
  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("[gate-out] RC522 Version=0x%02X %s\n", ver,
                (ver == 0x91 || ver == 0x92 || ver == 0x82) ? "(OK)" : "(LỖI KẾT NỐI)");

  sessCache.to<JsonObject>();

  // Bíp khởi động hệ thống
  buzzer.play(2, 80, 80);

  Parking::Settings s;
  s.wifiList     = WIFI_LIST;
  s.wifiCount    = WIFI_COUNT;
  s.apiKey       = API_KEY;
  s.databaseUrl  = DATABASE_URL;
  s.userEmail    = USER_EMAIL;
  s.userPassword = USER_PASSWORD;
  s.deviceName   = "gate-out";
  Parking::begin(s);

  showMsg("CONG RA", "San sang", "", "");
}

void loop() {
  Parking::loop();

  uint32_t now = millis();
  buzzer.update(now);

  sensorUpdate(irArrive, now);
  sensorUpdate(irPass, now);

  if (state == State::Paying) updatePaying(now);
  else if (state == State::Open) updateOpen(now);

  if (state == State::Idle) {
    checkAdminCommand(now);
    syncSessions(now);
    if ((int32_t)(now - msgUntil) >= 0 && now - lastIdleDraw >= IDLE_REDRAW_MS) {
      lastIdleDraw = now;
      drawIdle();
    }
  }

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

  if (now - lastReport > 10000) {
    lastReport = now;
    checkRfidHealth();
    Serial.printf("[gate-out] WiFi=%d Online=%d Cache=%d HàngĐợi=%u | TrạngThái=%d IR1=%d IR2=%d\n",
                  Parking::wifiConnected(), Parking::online(), Parking::cacheReady(),
                  (unsigned)Parking::queueSize(),
                  (int)state, irArrive.stable, irPass.stable);
  }
}