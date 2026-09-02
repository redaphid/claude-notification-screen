#include <Arduino.h>

int candidates[] = {40, 2, 41};
const int numCandidates = sizeof(candidates) / sizeof(candidates[0]);

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < numCandidates; i++) {
    pinMode(candidates[i], OUTPUT);
    digitalWrite(candidates[i], LOW);
  }
}

void loop() {
  for (int i = 0; i < numCandidates; i++) {
    int pin = candidates[i];
    Serial.printf("Testing GPIO%d (%d blinks)\n", pin, i + 1);
    for (int b = 0; b < i + 1; b++) {
      digitalWrite(pin, HIGH);
      delay(500);
      digitalWrite(pin, LOW);
      delay(500);
    }
    delay(1500);
  }
}
