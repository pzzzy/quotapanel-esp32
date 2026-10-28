#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
void wifiUpdateBegin();
void wifiUpdateTick();
void wifiUpdateAccountHealth(bool healthy);
bool wifiUpdateConfigure(const String& encoded);
void wifiUpdateStatus(JsonObject status);
bool wifiUpdateBusy();
