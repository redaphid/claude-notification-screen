// Minimal WiFi bring-up probe. Prints the reset reason FIRST -- if the board is
// browning out when the radio starts, the reason on the next boot says so.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>

static const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
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

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.printf("\n[min] boot, reset reason = %s\n", resetReasonName(esp_reset_reason()));
  Serial.flush();

  // Give the reader a window to attach before anything risky happens.
  for (int i = 3; i > 0; i--) {
    Serial.printf("[min] radio start in %d...\n", i);
    Serial.flush();
    delay(1000);
  }

  // Drop the CPU to 80MHz first: less baseline current means more headroom for
  // the RF inrush, if the 3V3 rail is what is collapsing.
  Serial.println("[min] cpu -> 80MHz");
  Serial.flush();
  setCpuFrequencyMhz(80);

  Serial.println("[min] WiFi.mode(WIFI_STA)");
  Serial.flush();
  WiFi.mode(WIFI_STA);
  Serial.println("[min] WiFi.mode returned");
  Serial.flush();

  WiFi.setTxPower(WIFI_POWER_2dBm);
  Serial.printf("[min] tx power %d, mac %s\n", (int)WiFi.getTxPower(), WiFi.macAddress().c_str());
  Serial.flush();
}

void loop() {
  Serial.printf("[min] alive %lus heap %u\n", millis() / 1000, (unsigned)ESP.getFreeHeap());
  delay(1000);
}
