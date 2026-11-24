#include <Arduino.h>
#include <Preferences.h>

void setup() {
  Serial.begin(115200);
  delay(1000);

  Preferences prefs;
  prefs.begin("parking", false);
  prefs.clear();   // xóa cache cư dân/thẻ/cấu hình và hàng đợi ghi lưu trong flash
  prefs.end();

  Serial.println("[reset] Da xoa cache va hang doi trong flash.");
  Serial.println("[reset] Bay gio nap lai firmware that cho board nay.");
}

void loop() {
  delay(1000);
}