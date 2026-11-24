#include <Arduino.h>

// Board bãi đỗ: chỉ đọc IR 4 ô rồi gửi qua UART2 sang gate_in. Không dùng WiFi/Firebase.
// Khung gửi: $dddd*HH\n   (d = '1' có xe, '0' trống, theo thứ tự bays[]; HH = XOR các ký tự d, dạng hex)

struct Bay {
  const char* name;
  uint8_t pin;
};

// THỨ TỰ và SỐ LƯỢNG phải khớp mảng BAYS trong src/gate_in/lot_link.h
static const Bay bays[] = {
  {"A1", 32},
  {"A2", 33},
  {"A3", 25},
  {"A4", 26},
};
static const size_t N = sizeof(bays) / sizeof(bays[0]);

static const bool     IR_ACTIVE_LOW = true;     // true: module báo LOW khi có xe
static const uint32_t SAMPLE_MS     = 20;       // chu kỳ đọc cảm biến
static const uint32_t DEBOUNCE_MS   = 600;      // trạng thái phải ổn định chừng này mới được tính
static const uint32_t SEND_MS       = 1000;     // gửi định kỳ mỗi giây (cũng là tín hiệu "còn sống")

static const uint8_t  UART_RX_PIN = 16;         // chưa dùng, giữ để cấu hình đủ cặp chân
static const uint8_t  UART_TX_PIN = 17;
static const uint32_t UART_BAUD   = 9600;

struct BayState {
  bool raw;            // lần đọc gần nhất
  bool stable;         // trạng thái đã xác nhận (true = có xe)
  uint32_t changedAt;  // lúc raw đổi lần cuối
};
static BayState st[N];

static uint32_t lastSample = 0;
static uint32_t lastSend = 0;
static uint32_t lastReport = 0;

static bool readOccupied(uint8_t pin) {
  int v = digitalRead(pin);
  return IR_ACTIVE_LOW ? (v == LOW) : (v == HIGH);
}

static void sendFrame() {
  char payload[N + 1];
  uint8_t cs = 0;
  for (size_t i = 0; i < N; i++) {
    payload[i] = st[i].stable ? '1' : '0';
    cs ^= (uint8_t)payload[i];
  }
  payload[N] = 0;
  Serial2.printf("$%s*%02X\n", payload, cs);
  lastSend = millis();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial2.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  for (size_t i = 0; i < N; i++) {
    pinMode(bays[i].pin, INPUT_PULLUP);
    bool occ = readOccupied(bays[i].pin);
    st[i] = {occ, occ, millis()};
  }
  Serial.println("[lot] San sang, gui trang thai qua UART2 (TX=GPIO17)");
}

void loop() {
  uint32_t now = millis();

  // 1. Đọc IR, chống nhiễu; đổi trạng thái thì gửi ngay
  if (now - lastSample >= SAMPLE_MS) {
    lastSample = now;
    for (size_t i = 0; i < N; i++) {
      bool occ = readOccupied(bays[i].pin);
      if (occ != st[i].raw) {
        st[i].raw = occ;
        st[i].changedAt = now;
      } else if (st[i].raw != st[i].stable && now - st[i].changedAt >= DEBOUNCE_MS) {
        st[i].stable = st[i].raw;
        Serial.printf("[lot] %s -> %s\n", bays[i].name, st[i].stable ? "co_xe" : "khong_xe");
        sendFrame();
      }
    }
  }

  // 2. Gửi định kỳ
  if (now - lastSend >= SEND_MS) sendFrame();

  // 3. Log định kỳ qua Serial
  if (now - lastReport > 5000) {
    lastReport = now;
    Serial.print("[lot]");
    for (size_t i = 0; i < N; i++) {
      Serial.printf(" %s=%d", bays[i].name, st[i].stable);
    }
    Serial.println();
  }
}