#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
bool nativeDataBegin();
bool nativeDataSeed(const String& identity,JsonObject input);
bool nativeDataBaseline(int slot,const String& identity,JsonObject prior);
void nativeDataUpdate(int slot,const String& identity,JsonObject usage,JsonObject credits,JsonObject output,int64_t now);
void nativeDataEvents(JsonArray output,int64_t now);
uint32_t nativeHistoryCount();
void nativeDataAlertTest(JsonArray cases,JsonArray results);
