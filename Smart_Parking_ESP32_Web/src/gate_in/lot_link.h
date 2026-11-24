#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "ParkingCore.h"

// Nhận trạng thái 4 ô từ board bãi qua UART2, đẩy lên Firebase, hiển thị LCD, báo heartbeat /devices/lot.

namespace LotLink {

struct Bay {
  const char* name;
  bool resident;   // true: ô cư dân (giu_cho/co_xe), false: ô khách (trong/co_xe)
};

// THỨ TỰ và SỐ LƯỢNG phải khớp mảng bays[] trong src/lot/main.cpp
static const Bay BAYS[] = {
  {"A1", true},
  {"A2", true},
  {"A3", false},
  {"A4", false},
};
static const size_t NB = sizeof(BAYS) / sizeof(BAYS[0]);

static const uint8_t  UART_RX_PIN     = 16;
static const uint8_t  UART_TX_PIN     = 17;
static const uint32_t UART_BAUD       = 9600;
static const uint32_t LINK_TIMEOUT_MS = 5000;     // quá thời gian này không nhận được khung nào = mất tín hiệu bãi
static const uint32_t REASSERT_MS     = 300000;   // ghi lại trạng thái các ô mỗi 5 phút
static const uint32_t BEAT_MS         = 15000;    // heartbeat của bãi

static const uint8_t  LCD_SDA        = 21;
static const uint8_t  LCD_SCL        = 22;
static const uint32_t I2C_CLOCK_HZ   = 50000;
static const uint32_t LCD_REFRESH_MS = 1000;
static const uint32_t LCD_REINIT_MS  = 120000;
static const uint32_t LCD_RETRY_MS   = 5000;

// ---------- Trạng thái ----------
static bool occ[NB];                // trạng thái mới nhất nhận từ bãi
static bool pub[NB];                // trạng thái đã gửi lên Firebase
static bool pubValid[NB];
static bool haveFrame = false;
static bool linkOk = false;
static uint32_t lastFrameAt = 0;

static char rxBuf[24];
static uint8_t rxLen = 0;

static uint32_t lastReassert = 0;
static bool reassertDone = false;
static uint32_t lastBeat = 0;

static LiquidCrystal_I2C* lcd = nullptr;
static uint8_t lcdAddr = 0;
static bool lcdReady = false;
static uint32_t lastLcdRefresh = 0;
static uint32_t lastLcdInit = 0;
static uint32_t lastLcdTry = 0;

// ---------- Firebase ----------
static const char* stateText(size_t i, bool occupied) {
  if (occupied) return "co_xe";
  return BAYS[i].resident ? "giu_cho" : "trong";
}

static void publishBay(size_t i, bool logIt) {
  String path = "/slots/";
  path += BAYS[i].name;
  String json = "\"";
  json += stateText(i, occ[i]);
  json += "\"";
  Parking::write(Parking::Op::Set, path, json);
  pub[i] = occ[i];
  pubValid[i] = true;
  if (logIt) Serial.printf("[lot-link] %s -> %s\n", BAYS[i].name, stateText(i, occ[i]));
}

// ---------- UART ----------
static void handleFrame(const char* line, uint32_t now) {
  if (strlen(line) != NB + 4 || line[0] != '$' || line[NB + 1] != '*') return;
  uint8_t cs = 0;
  for (size_t i = 0; i < NB; i++) {
    char c = line[1 + i];
    if (c != '0' && c != '1') return;
    cs ^= (uint8_t)c;
  }
  char hex[3] = {line[NB + 2], line[NB + 3], 0};
  if ((uint8_t)strtoul(hex, nullptr, 16) != cs) return;   // sai checksum: bỏ khung

  bool changed = !haveFrame;
  for (size_t i = 0; i < NB; i++) {
    bool o = (line[1 + i] == '1');
    if (o != occ[i]) changed = true;
    occ[i] = o;
  }
  haveFrame = true;
  lastFrameAt = now;
  if (changed) lastLcdRefresh = 0;   // vẽ lại LCD ngay
}

static void pollUart(uint32_t now) {
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    if (c == '\r') continue;
    if (c == '\n') {
      rxBuf[rxLen] = 0;
      if (rxLen > 0) handleFrame(rxBuf, now);
      rxLen = 0;
    } else if (rxLen < sizeof(rxBuf) - 1) {
      rxBuf[rxLen++] = c;
    } else {
      rxLen = 0;   // dòng quá dài: bỏ
    }
  }
}

// ---------- LCD ----------
static bool pingAddr(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static uint8_t findLcdAddr() {
  if (pingAddr(0x27)) return 0x27;
  if (pingAddr(0x3F)) return 0x3F;
  return 0;
}

static bool initLcd() {
  uint8_t addr = findLcdAddr();
  if (addr == 0) {
    lcdReady = false;
    return false;
  }
  if (!lcd || addr != lcdAddr) {
    lcd = new LiquidCrystal_I2C(addr, 16, 2);
    lcdAddr = addr;
  }
  lcd->init();
  lcd->backlight();
  Wire.setClock(I2C_CLOCK_HZ);
  lcdReady = true;
  lastLcdInit = millis();
  Serial.printf("[lot-link] LCD san sang tai dia chi 0x%02X\n", addr);
  return true;
}

static void padLine(char* dst, const char* src) {
  memset(dst, ' ', 16);
  dst[16] = 0;
  size_t n = strlen(src);
  if (n > 16) n = 16;
  memcpy(dst, src, n);
}

static void drawLcd() {
  if (!lcdReady) return;
  if (!pingAddr(lcdAddr)) {
    lcdReady = false;
    Serial.println("[lot-link] Mat ket noi LCD, se thu lai");
    return;
  }

  uint8_t freeGuest = 0, totalGuest = 0;
  const char* firstFree = nullptr;
  for (size_t i = 0; i < NB; i++) {
    if (BAYS[i].resident) continue;
    totalGuest++;
    if (!occ[i]) {
      freeGuest++;
      if (!firstFree) firstFree = BAYS[i].name;
    }
  }

  char t[24], l1[17], l2[17];
  if (linkOk) {
    snprintf(t, sizeof(t), "Cho khach: %u/%u", (unsigned)freeGuest, (unsigned)totalGuest);
    padLine(l1, t);
    if (firstFree) snprintf(t, sizeof(t), "Moi vao o %s", firstFree);
    else snprintf(t, sizeof(t), "HET CHO KHACH");
    padLine(l2, t);
  } else {
    padLine(l1, "Cho khach: ?");
    padLine(l2, "MAT KET NOI BAI");
  }
  l1[15] = Parking::online() ? '*' : '!';

  lcd->setCursor(0, 0);
  lcd->print(l1);
  lcd->setCursor(0, 1);
  lcd->print(l2);
}

// ---------- Vòng đời ----------
inline void begin() {
  Serial2.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  Wire.begin(LCD_SDA, LCD_SCL);
  if (initLcd()) {
    lcd->setCursor(0, 0);
    lcd->print("Smart Parking   ");
    lcd->setCursor(0, 1);
    lcd->print("Dang khoi dong  ");
  } else {
    Serial.println("[lot-link] Khong thay LCD (0x27/0x3F), se thu lai sau");
  }
}

// gateIdle = true khi barrier đang đóng và không có việc gì. Việc ghi Firebase chỉ làm lúc rảnh để khỏi chặn barrier.
inline void loop(bool gateIdle) {
  uint32_t now = millis();

  pollUart(now);

  bool ok = haveFrame && (now - lastFrameAt < LINK_TIMEOUT_MS);
  if (ok != linkOk) {
    linkOk = ok;
    Serial.println(ok ? "[lot-link] Da noi voi bai do" : "[lot-link] MAT tin hieu tu bai do");
    lastLcdRefresh = 0;
  }

  // LCD: vẽ lại định kỳ, khởi tạo lại định kỳ, tự nối lại khi mất
  if (!lcdReady) {
    if (now - lastLcdTry >= LCD_RETRY_MS) {
      lastLcdTry = now;
      if (initLcd()) drawLcd();
    }
  } else if (now - lastLcdInit >= LCD_REINIT_MS) {
    initLcd();
    drawLcd();
    lastLcdRefresh = now;
  } else if (now - lastLcdRefresh >= LCD_REFRESH_MS) {
    lastLcdRefresh = now;
    drawLcd();
  }

  if (!haveFrame || !gateIdle) return;

  // Ghi lên Firebase khi ô đổi trạng thái
  for (size_t i = 0; i < NB; i++) {
    if (!pubValid[i] || pub[i] != occ[i]) publishBay(i, true);
  }

  bool canSend = Parking::online() && Parking::timeValid() && Parking::queueSize() == 0;

  // Ghi lại trạng thái các ô định kỳ
  if (canSend && (!reassertDone || now - lastReassert >= REASSERT_MS)) {
    lastReassert = now;
    reassertDone = true;
    for (size_t i = 0; i < NB; i++) publishBay(i, false);
    Serial.println("[lot-link] Da ghi lai trang thai cac o");
  }

  // Heartbeat thay cho board bãi (chỉ khi còn nhận được tín hiệu từ bãi)
  if (canSend && linkOk && now - lastBeat >= BEAT_MS) {
    lastBeat = now;
    String j = "{\"lastSeen\":";
    j += Parking::u64(Parking::nowMs());
    j += ",\"ip\":\"UART qua gate-in\"}";
    Parking::write(Parking::Op::Set, "/devices/lot", j);
  }
}

}  // namespace LotLink