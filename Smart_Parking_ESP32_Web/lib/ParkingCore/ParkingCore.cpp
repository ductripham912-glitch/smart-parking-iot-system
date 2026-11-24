#include "ParkingCore.h"

#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Firebase_ESP_Client.h>
#include <sys/time.h>
#include <esp_system.h>
#include <vector>
#include "addons/TokenHelper.h"
#include <WiFiMulti.h>

namespace Parking {

// ---------- Biến nội bộ ----------
static Settings cfg;
static FirebaseData fbdo;
static FirebaseAuth fbAuth;
static FirebaseConfig fbConfig;
static Preferences prefs;
static WiFiMulti wifiMulti;

static bool fbStarted = false;
static bool ntpStarted = false;
static bool busyFlag = false;
static uint32_t lastWifiTry = 0;
static uint32_t nextSync = 0;
static bool syncedOnce = false;
static uint32_t droppedTotal = 0;

// Báo trạng thái thiết bị lên /devices/<deviceName> để web xem được
static const uint32_t BEAT_MS = 15000;
static uint32_t lastBeat = 0;
static bool beatDone = false;
static uint32_t bootCount = 0;

static JsonDocument docCfg, docRes, docCards;

struct Pending {
  Op op;
  String path;
  String json;
  uint8_t tries;
};
static std::vector<Pending> pendingQueue;
static const uint8_t MAX_QUEUE = 30;
static uint32_t nextTry = 0;

// Hàng đợi lưu trong flash dạng vòng: khóa q0..q29, qh = đầu, qn = số phần tử
static uint8_t qHead = 0;
static uint8_t qCount = 0;

// Đếm số lần ghi, dùng để phát hiện cache đã cũ khi đồng bộ chia bước
static uint32_t writeCounter = 0;

#define LOG(fmt, ...) Serial.printf("[%s] " fmt "\n", cfg.deviceName, ##__VA_ARGS__)

// ---------- Tiện ích ----------
String u64(uint64_t v) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
  return String(buf);
}

bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }

bool online() { return wifiConnected() && fbStarted && Firebase.ready(); }

bool timeValid() { return time(nullptr) > 1700000000; }

uint64_t nowMs() {
  if (!timeValid()) return 0;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
}

size_t queueSize() { return pendingQueue.size(); }

uint32_t droppedCount() { return droppedTotal; }

void setBusy(bool busy) { busyFlag = busy; }

// ---------- Lưu/đọc chuỗi dài trong flash (dạng blob, không bị giới hạn ~4000 byte của putString) ----------
static void putBlob(const char* key, const String& s) {
  prefs.putBytes(key, s.c_str(), s.length());
}

static String getBlob(const char* key, const char* defaultValue) {
  size_t n = prefs.getBytesLength(key);
  if (n == 0) return String(defaultValue);
  std::vector<char> buf(n + 1);
  prefs.getBytes(key, buf.data(), n);
  buf[n] = 0;
  return String(buf.data());
}

// ---------- Bộ nhớ đệm ----------
static String lastSaved[3];   // bản đã lưu trong flash, để chỉ ghi khi dữ liệu thật sự đổi

static void loadCache() {
  deserializeJson(docCfg, getBlob("cfgb", "{}"));
  deserializeJson(docRes, getBlob("resb", "{}"));
  deserializeJson(docCards, getBlob("cardsb", "{}"));
  serializeJson(docCfg, lastSaved[0]);
  serializeJson(docRes, lastSaved[1]);
  serializeJson(docCards, lastSaved[2]);
}

static void saveIfChanged(const char* key, JsonDocument& doc, String& last) {
  String s;
  serializeJson(doc, s);
  if (s == last) return;
  putBlob(key, s);
  last = s;
}

static void saveCache() {
  saveIfChanged("cfgb", docCfg, lastSaved[0]);
  saveIfChanged("resb", docRes, lastSaved[1]);
  saveIfChanged("cardsb", docCards, lastSaved[2]);
}

// ---------- Hàng đợi lưu trong flash ----------
static String qKey(uint8_t i) {
  char buf[8];
  snprintf(buf, sizeof(buf), "q%u", (unsigned)i);
  return String(buf);
}

// Thêm phần tử p vào cuối hàng đợi flash. Gọi TRƯỚC khi tăng qCount.
static void persistTail(const Pending& p) {
  JsonDocument d;
  d["o"] = (uint8_t)p.op;
  d["p"] = p.path;
  d["j"] = p.json;
  String s;
  serializeJson(d, s);
  uint8_t idx = (qHead + qCount) % MAX_QUEUE;
  prefs.putString(qKey(idx).c_str(), s);
  qCount++;
  prefs.putUChar("qn", qCount);
}

static void enqueue(const Pending& p) {
  if (pendingQueue.size() >= MAX_QUEUE) {
    LOG("Hang doi day, bo lenh cu nhat: %s", pendingQueue.front().path.c_str());
    prefs.remove(qKey(qHead).c_str());
    qHead = (qHead + 1) % MAX_QUEUE;
    qCount--;
    prefs.putUChar("qh", qHead);
    prefs.putUChar("qn", qCount);
    pendingQueue.erase(pendingQueue.begin());
    droppedTotal++;
  }
  persistTail(p);
  pendingQueue.push_back(p);
}

static void dequeueHead() {
  if (pendingQueue.empty()) return;
  prefs.remove(qKey(qHead).c_str());
  qHead = (qHead + 1) % MAX_QUEUE;
  qCount--;
  prefs.putUChar("qh", qHead);
  prefs.putUChar("qn", qCount);
  pendingQueue.erase(pendingQueue.begin());
}

static void rewriteQueueInFlash() {
  for (uint8_t i = 0; i < MAX_QUEUE; i++) prefs.remove(qKey(i).c_str());
  qHead = 0;
  qCount = 0;
  prefs.putUChar("qh", 0);
  prefs.putUChar("qn", 0);
  for (const Pending& p : pendingQueue) persistTail(p);
}

static void loadQueue() {
  qHead = prefs.getUChar("qh", 0);
  qCount = prefs.getUChar("qn", 0);
  if (qHead >= MAX_QUEUE || qCount > MAX_QUEUE) {
    qHead = 0;
    qCount = 0;
  }

  uint8_t stored = qCount;
  pendingQueue.clear();
  for (uint8_t i = 0; i < stored; i++) {
    uint8_t idx = (qHead + i) % MAX_QUEUE;
    String s = prefs.getString(qKey(idx).c_str(), "");
    JsonDocument d;
    if (s.length() == 0 || deserializeJson(d, s)) continue;
    uint8_t o = d["o"] | 255;
    String path = d["p"] | "";
    String json = d["j"] | "";
    if (o > 2 || path.length() == 0 || path[0] != '/') continue;
    pendingQueue.push_back(Pending{(Op)o, path, json, 0});
  }

  // Có phần tử hỏng thì ghi lại cho gọn
  if (pendingQueue.size() != stored) rewriteQueueInFlash();
  if (!pendingQueue.empty()) {
    LOG("Khoi phuc %u lenh ghi chua gui tu bo nho", (unsigned)pendingQueue.size());
  }
}

static bool hasPending(const String& path) {
  for (const Pending& p : pendingQueue) {
    if (p.path == path) return true;
  }
  return false;
}

// ---------- Gửi lệnh lên Firebase ----------
static bool sendOp(const Pending& p) {
  const char* path = p.path.c_str();
  const String& j = p.json;

  if (p.op == Op::Delete) {
    return Firebase.RTDB.deleteNode(&fbdo, path);
  }

  if (j.length() > 0 && j[0] == '{') {
    FirebaseJson js;
    js.setJsonData(j);
    if (p.op == Op::Set) return Firebase.RTDB.setJSON(&fbdo, path, &js);
    return Firebase.RTDB.pushJSON(&fbdo, path, &js);
  }

  if (p.op == Op::Push) return false;  // Push chỉ hỗ trợ object

  if (j.length() >= 2 && j[0] == '"') {
    return Firebase.RTDB.setString(&fbdo, path, j.substring(1, j.length() - 1));
  }
  if (j == "true" || j == "false") {
    return Firebase.RTDB.setBool(&fbdo, path, j == "true");
  }
  return Firebase.RTDB.setInt(&fbdo, path, (int)j.toInt());
}

// Lỗi vĩnh viễn: thử lại bao nhiêu lần cũng vô ích (sai dữ liệu, bị rules chặn, sai đường dẫn).
// Còn lại (mất mạng, timeout, SSL...) coi là tạm thời.
static bool isPermanentError() {
  int code = fbdo.httpCode();
  if (code == 400 || code == 403 || code == 404) return true;
  String r = fbdo.errorReason();
  r.toLowerCase();
  return r.indexOf("denied") >= 0;
}

void write(Op op, const String& path, const String& json) {
  Pending p{op, path, json, 0};
  writeCounter++;

  // Đang online và hàng đợi trống: gửi ngay
  if (pendingQueue.empty() && online()) {
    if (sendOp(p)) return;
    if (isPermanentError()) {
      LOG("LOI VINH VIEN khi ghi %s: %s (bo lenh)", path.c_str(), fbdo.errorReason().c_str());
      droppedTotal++;
      return;
    }
    LOG("Gui that bai (%s), xep vao hang doi", fbdo.errorReason().c_str());
  }

  enqueue(p);
}

static const char* resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

// Gửi thẳng, không xếp hàng đợi: tín hiệu cũ gửi muộn không còn ý nghĩa
static void sendHeartbeat() {
  String j;
  j += "{\"lastSeen\":";
  j += u64(nowMs());
  j += ",\"ip\":\"";
  j += WiFi.localIP().toString();
  j += "\",\"rssi\":";
  j += String(WiFi.RSSI());
  j += ",\"uptimeS\":";
  j += String((unsigned long)(millis() / 1000));
  j += ",\"queue\":";
  j += String((unsigned)pendingQueue.size());
  j += ",\"dropped\":";
  j += String((unsigned long)droppedTotal);
  j += ",\"reset\":\"";
  j += resetReasonName();
  j += "\",\"boots\":";
  j += String((unsigned long)bootCount);
  j += ",\"heap\":";
  j += String((unsigned long)ESP.getFreeHeap());
  j += "}";

  String path = "/devices/";
  path += cfg.deviceName;
  Pending p{Op::Set, path, j, 0};
  if (!sendOp(p)) {
    LOG("Heartbeat that bai: %s", fbdo.errorReason().c_str());
  }
}

static void flushQueue() {
  if (pendingQueue.empty()) return;
  if ((int32_t)(millis() - nextTry) < 0) return;

  Pending& p = pendingQueue.front();
  if (sendOp(p)) {
    dequeueHead();
    return;
  }

  if (isPermanentError()) {
    LOG("LOI VINH VIEN khi gui lai %s: %s (bo lenh)", p.path.c_str(), fbdo.errorReason().c_str());
    droppedTotal++;
    dequeueHead();
    return;
  }

  // Lỗi tạm thời: giữ nguyên lệnh, chờ lâu dần (2s, 4s, 8s, 16s, tối đa 30s)
  if (p.tries < 255) p.tries++;
  uint8_t shift = p.tries > 4 ? 4 : p.tries;
  uint32_t waitMs = 2000UL << shift;
  if (waitMs > 30000UL) waitMs = 30000UL;
  nextTry = millis() + waitMs;
  LOG("Gui lai that bai lan %u (%s), thu lai sau %lu ms", p.tries, fbdo.errorReason().c_str(),
      (unsigned long)waitMs);
}

// ---------- Đọc Firebase ----------
bool fbGetJson(const String& path, String& out) {
  if (!online()) return false;
  if (!Firebase.RTDB.getJSON(&fbdo, path.c_str())) {
    LOG("Doc %s that bai: %s", path.c_str(), fbdo.errorReason().c_str());
    return false;
  }
  if (fbdo.dataType() == "json") {
    out = fbdo.jsonString();
  } else {
    out = "{}";
  }
  return true;
}

ReadResult fbRead(const String& path, String& out) {
  if (!online()) return ReadResult::Error;
  if (Firebase.RTDB.getJSON(&fbdo, path.c_str())) {
    if (fbdo.dataType() == "json") {
      out = fbdo.jsonString();
    } else {
      out = "{}";
    }
    return ReadResult::Ok;
  }
  String reason = fbdo.errorReason();
  String lower = reason;
  lower.toLowerCase();
  if (lower.indexOf("not exist") >= 0) {
    out = "";
    return ReadResult::NotFound;
  }
  LOG("Doc %s that bai: %s", path.c_str(), reason.c_str());
  return ReadResult::Error;
}

// ---------- Đồng bộ cache chia bước ----------
// Mỗi lần gọi chỉ tải 1 trong 3 nhánh để mỗi lần chặn vòng lặp ngắn hơn.
static String sCfg, sRes, sCards;
static uint8_t syncPhase = 0;
static uint32_t syncStartWrites = 0;

// Trả về: 0 = đang dở (gọi tiếp), 1 = xong, -1 = lỗi/hoãn
static int syncStep() {
  if (!pendingQueue.empty() || busyFlag) {
    syncPhase = 0;
    return -1;
  }

  if (syncPhase == 0) syncStartWrites = writeCounter;

  bool ok;
  if (syncPhase == 0) ok = fbGetJson("/config", sCfg);
  else if (syncPhase == 1) ok = fbGetJson("/residents", sRes);
  else ok = fbGetJson("/cards", sCards);

  if (!ok) {
    syncPhase = 0;
    return -1;
  }
  if (syncPhase < 2) {
    syncPhase++;
    return 0;
  }
  syncPhase = 0;

  // Giữa các bước có lệnh ghi mới: dữ liệu vừa tải có thể đã cũ, bỏ và thử lại sau
  if (writeCounter != syncStartWrites) {
    LOG("Co lenh ghi moi trong luc dong bo, bo dot dong bo nay");
    return -1;
  }

  JsonDocument a, b, c;
  if (deserializeJson(a, sCfg) || deserializeJson(b, sRes) || deserializeJson(c, sCards)) {
    LOG("Du lieu dong bo bi loi dinh dang, giu cache cu");
    return -1;
  }
  docCfg = a;
  docRes = b;
  docCards = c;
  saveCache();
  syncedOnce = true;
  LOG("Da dong bo cache tu Firebase (%u cu dan)", (unsigned)docRes.size());
  return 1;
}

bool syncCache() {
  int r;
  do {
    r = syncStep();
  } while (r == 0);
  return r == 1;
}

bool cacheReady() { return docRes.size() > 0; }

// ---------- Truy vấn cache ----------
bool residentExists(const String& id) { return docRes[id].is<JsonObject>(); }

bool residentInside(const String& id, bool fresh) {
  if (fresh && online()) {
    String path;
    path += "/residents/";
    path += id;
    path += "/trongBai";
    // Có lệnh ghi cùng đường dẫn đang chờ thì cache mới hơn Firebase, không đọc
    if (!hasPending(path)) {
      if (Firebase.RTDB.getBool(&fbdo, path.c_str())) {
        bool v = fbdo.boolData();
        if (residentExists(id)) docRes[id]["trongBai"] = v;  // chỉ cập nhật RAM, đỡ mòn flash
        return v;
      }
      LOG("Doc truc tiep %s that bai (%s), dung cache", path.c_str(), fbdo.errorReason().c_str());
    }
  }
  return docRes[id]["trongBai"] | false;
}

void setResidentInside(const String& id, bool inside) {
  if (!residentExists(id)) return;
  docRes[id]["trongBai"] = inside;
  saveCache();

  String path;
  path += "/residents/";
  path += id;
  path += "/trongBai";
  write(Op::Set, path, inside ? "true" : "false");
}

String residentSlot(const String& id) {
  for (JsonPair kv : docCfg["o"].as<JsonObject>()) {
    if (String(kv.value()["chuXe"] | "") == id) return String(kv.key().c_str());
  }
  return "";
}

bool cardRegistered(const String& uid) { return docCards[uid].is<JsonObject>(); }

bool cardLocked(const String& uid) { return docCards[uid]["khoa"] | false; }

long cfgLong(const char* key, long defaultValue) { return docCfg[key] | defaultValue; }

// ---------- Vòng đời ----------
void begin(const Settings& s) {
  cfg = s;

  prefs.begin("parking", false);
  bootCount = prefs.getUInt("boots", 0) + 1;   // đếm số lần khởi động (phát hiện reset liên tục do sụt nguồn)
  prefs.putUInt("boots", bootCount);
  LOG("Khoi dong lan %lu, ly do: %s", (unsigned long)bootCount, resetReasonName());
  // Khóa cũ (dạng chuỗi) của bản trước, bỏ đi; cache sẽ được tải lại khi có mạng
  if (prefs.isKey("cfg")) prefs.remove("cfg");
  if (prefs.isKey("res")) prefs.remove("res");
  if (prefs.isKey("cards")) prefs.remove("cards");

  loadCache();
  LOG("Da tai cache tu bo nho: %u cu dan", (unsigned)docRes.size());
  loadQueue();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);   // để WiFiMulti tự chọn lại mạng, tránh tranh nhau nối lại cùng một SSID
  for (size_t i = 0; i < cfg.wifiCount; i++) {
    wifiMulti.addAP(cfg.wifiList[i].ssid, cfg.wifiList[i].password);
  }

  Serial.printf("[%s] Dang ket noi WiFi (%u mang da luu)", cfg.deviceName, (unsigned)cfg.wifiCount);
  uint32_t t0 = millis();
  while (!wifiConnected() && millis() - t0 < 20000) {
    wifiMulti.run(5000);
    Serial.print(".");
  }
  Serial.println();
  if (wifiConnected()) {
    LOG("WiFi OK: %s, IP: %s", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  } else {
    LOG("Chua co WiFi, chay che do offline va tu noi lai");
  }

  fbConfig.api_key = cfg.apiKey;
  fbConfig.database_url = cfg.databaseUrl;
  fbAuth.user.email = cfg.userEmail;
  fbAuth.user.password = cfg.userPassword;
  fbConfig.token_status_callback = tokenStatusCallback;
  fbConfig.max_token_generation_retry = 3;   // hạn chế thử đăng nhập sai nhiều lần

  fbdo.setBSSLBufferSize(4096, 1024);
  fbdo.setResponseSize(4096);
  Firebase.reconnectNetwork(true);
  // Firebase.begin() được gọi trong loop() khi đã có WiFi
}

void loop() {
  uint32_t now = millis();

    // 1. WiFi: mất mạng thì mỗi 10 giây quét và nối lại vào mạng tốt nhất trong danh sách.
  //    Đang bận (barrier mở, chờ thanh toán) thì không quét vì scan chặn vòng lặp vài giây.
  if (!wifiConnected()) {
    if (!busyFlag && now - lastWifiTry > 10000) {
      lastWifiTry = now;
      LOG("Mat WiFi, dang quet va noi lai...");
      if (wifiMulti.run(4000) == WL_CONNECTED) {
        LOG("Da noi lai WiFi: %s, IP: %s", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
      }
    }
    return;
  }
  
  // 2. Giờ NTP: bắt đầu một lần khi đã có WiFi
  if (!ntpStarted) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    ntpStarted = true;
  }

  // 3. Firebase: đăng nhập một lần khi đã có WiFi
  if (!fbStarted) {
    Firebase.begin(&fbConfig, &fbAuth);
    fbStarted = true;
  }
  if (!Firebase.ready()) return;

  // 4. Báo trạng thái thiết bị cho web (kể cả khi đang bận, để web không báo nhầm là mất kết nối)
  if (timeValid() && (!beatDone || now - lastBeat >= BEAT_MS)) {
    lastBeat = now;
    beatDone = true;
    sendHeartbeat();
  }

  // 5. Cổng đang bận thì không làm việc nặng
  if (busyFlag) return;

  // 6. Gửi các lệnh ghi đang chờ
  flushQueue();

  // 7. Đồng bộ cache: mỗi lần loop chỉ làm 1 bước.
  //    Thành công: lần sau sau 60 giây. Lỗi/hoãn: thử lại sau 10 giây (kể cả lần đầu).
  if (pendingQueue.empty() && (syncPhase > 0 || (int32_t)(now - nextSync) >= 0)) {
    int r = syncStep();
    if (r == 1) nextSync = millis() + 60000;
    else if (r == -1) nextSync = millis() + 10000;
  }
}
}  // namespace Parking
