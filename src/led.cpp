#include "led.h"

#include "board_pins.h"

#include <Arduino.h>

void ledBegin() {
  pinMode(BOARD_LED_PIN, OUTPUT);
  digitalWrite(BOARD_LED_PIN, LOW);
}

void ledSetWifiActive(bool on) { digitalWrite(BOARD_LED_PIN, on ? HIGH : LOW); }
