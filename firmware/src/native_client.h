#pragma once
#include <Arduino.h>
void nativeClientBegin();
String nativeClientCommand(const String &command);
bool nativeEnabled();
void nativeConnectionState(const String& state);
void nativePublishPacket(const String& packet);

String nativeTimezone();
