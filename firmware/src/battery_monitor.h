// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <ArduinoJson.h>
#include "battery_model.h"
void batteryBegin();
void batteryTick();
battery::Reading batteryReading();
unsigned batteryHistory(float* values,unsigned capacity);
void batteryStatus(JsonObject output);
