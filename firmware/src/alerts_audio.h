#pragma once
#include <Arduino.h>
void audioBegin();
bool audioQueueAlert(int type,int account);
bool audioQueueBatteryWarning(bool critical=false,bool preview=false);
void audioToggleMute();
void audioSetVolume(int percent);
bool audioMuted();
bool audioReady();
int audioVolume();
uint32_t audioPlayed();
uint32_t audioFailed();
