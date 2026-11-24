#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>

// Test đọc UID thẻ RC522 - dùng đúng chân và định dạng UID của dự án.
// UID in ra dạng chữ HOA, không dấu cách (ví dụ 04A1B2C3D4E580) -> copy dán thẳng vào web khi thêm thẻ.

static const uint8_t PIN_RC522_SS  = 5;    // SCK 18, MISO 19, MOSI 23 cố định theo SPI phần cứng ESP32
static const uint8_t PIN_RC522_RST = 4;

static const uint32_t READ_MS   = 50;      // Chu kỳ quét thẻ
static const uint32_t REPEAT_MS = 1500;    // Cùng một thẻ giữ trên đầu đọc chỉ in 1 lần mỗi 1,5s

static MFRC522 rfid(PIN_RC522_SS, PIN_RC522_RST);

static uint32_t lastRead = 0;
static uint32_t lastHealth = 0;
static String lastUid;
static uint32_t lastUidAt = 0;
static uint32_t cardCount = 0;

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

static void checkRfidHealth() {
  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  if (ver == 0x00 || ver == 0xFF) {
    Serial.println("[uid-test] CANH BAO: mat ket noi RC522, dang khoi dong lai...");
    rfid.PCD_Init();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_RC522_SS, OUTPUT);
  digitalWrite(PIN_RC522_SS, HIGH);

  SPI.begin();
  rfid.PCD_Init();

  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("[uid-test] RC522 Version=0x%02X %s\n", ver,
                (ver == 0x91 || ver == 0x92 || ver == 0x82) ? "(OK)" : "(LOI KET NOI - kiem tra day, nguon 3.3V)");
  Serial.println("[uid-test] San sang. Dat the len dau doc de xem UID...");
}

void loop() {
  uint32_t now = millis();

  if (now - lastRead >= READ_MS) {
    lastRead = now;
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      String uid = uidToString(rfid.uid);
      MFRC522::PICC_Type type = rfid.PICC_GetType(rfid.uid.sak);
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();

      if (uid != lastUid || now - lastUidAt >= REPEAT_MS) {
        lastUid = uid;
        lastUidAt = now;
        cardCount++;
        Serial.printf("[uid-test] #%lu  UID=%s  (%u byte)  Loai=%s\n",
                      (unsigned long)cardCount, uid.c_str(), (unsigned)rfid.uid.size,
                      rfid.PICC_GetTypeName(type));
      }
    }
  }

  if (now - lastHealth > 10000) {
    lastHealth = now;
    checkRfidHealth();
  }
}