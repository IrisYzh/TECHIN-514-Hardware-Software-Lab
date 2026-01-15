#include <Arduino.h>

// XIAO ESP32-C3 pin mapping (common):
// D0 = GPIO2
// D1 = GPIO3
static const int PIN_VOUT1 = 2;  // D0 -> GPIO2
static const int PIN_VOUT2 = 3;  // D1 -> GPIO3

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Reading VOUT1 (GPIO2) and VOUT2 (GPIO3)...");

  // ESP32 ADC is 12-bit by default, but set explicitly:
  analogReadResolution(12);
}

void loop() {
  int adc1 = analogRead(PIN_VOUT1);  // 0~4095
  int adc2 = analogRead(PIN_VOUT2);

  float v1 = (adc1 / 4095.0) * 3.3;
  float v2 = (adc2 / 4095.0) * 3.3;

  Serial.print("J2(VOUT1) GPIO2: ADC=");
  Serial.print(adc1);
  Serial.print("  V=");
  Serial.print(v1, 3);
  Serial.print(" V   |   ");

  Serial.print("J3(VOUT2) GPIO3: ADC=");
  Serial.print(adc2);
  Serial.print("  V=");
  Serial.print(v2, 3);
  Serial.println(" V");

  delay(500);
}