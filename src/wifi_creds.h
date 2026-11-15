#pragma once

#include <Arduino.h>

void wifiCredsBegin();
bool wifiCredsConfigured();
String wifiCredsSsid();
String wifiCredsPass();

// Empty ssid rejected. Pass may be empty (open network).
bool wifiCredsApply(const String &ssid, const String &pass);
