// SPDX-License-Identifier: GPL-3.0-only
#include "battery_monitor.h"
#include "board_pins.h"
#include <Arduino.h>

namespace {
battery::Monitor monitor;
portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
uint32_t sampledAt=0;
bool sampled=false;
}
void batteryBegin(){
    pinMode(XZ_BAT_ADC,INPUT);
    analogReadResolution(12);
    // BAT/2 reaches 2.12 V: the S3's 6 dB range (1.75 V) is insufficient.
    analogSetPinAttenuation(XZ_BAT_ADC,ADC_11db);
    batteryTick();
}
void batteryTick(){
    uint32_t now=millis();if(sampled&&now-sampledAt<5000)return;
    sampled=true;sampledAt=now;
    analogReadMilliVolts(XZ_BAT_ADC); // Discard the first conversion after idle.
    uint32_t values[32];
    for(auto &v:values){v=analogReadMilliVolts(XZ_BAT_ADC);delay(1);}
    std::sort(values,values+32);
    uint32_t sum=0;for(unsigned i=8;i<24;i++)sum+=values[i];
    float mv=sum/16.0f*2.0f; // R14 and R15 are both 200 kOhm.
    // Reject unstable acquisition rather than displaying a plausible average.
    if(values[27]-values[4]>80)mv=0;
    portENTER_CRITICAL(&lock);monitor.add(now,mv);portEXIT_CRITICAL(&lock);
}
battery::Reading batteryReading(){
    portENTER_CRITICAL(&lock);auto r=monitor.reading;portEXIT_CRITICAL(&lock);
    if(r.valid&&millis()-r.at>15000){r.valid=false;r.percentage=-1;r.trendReady=false;}
    return r;
}
// Called by the main display loop, the same task that samples the ADC.
unsigned batteryHistory(float* values,unsigned capacity){
    unsigned count=std::min(capacity,monitor.count);
    for(unsigned i=0;i<count;i++)values[i]=monitor.samples[(monitor.cursor+battery::Monitor::Capacity-count+i)%battery::Monitor::Capacity].mv;
    return count;
}
void batteryStatus(JsonObject out){
    auto r=batteryReading();out["valid"]=r.valid;out["percentageEstimated"]=true;
    out["voltage"]=nullptr;out["percentage"]=nullptr;out["voltageRateMvPerMin"]=nullptr;
    out["charging"]=nullptr;out["externalPower"]=nullptr;out["currentMa"]=nullptr;out["inputVoltage"]=nullptr;out["powerWatts"]=nullptr;
    out["trend"]="unavailable";out["coverageSeconds"]=r.coverageMs/1000;
    if(r.valid){
        out["voltage"]=r.millivolts/1000;out["percentage"]=r.percentage;out["sampleAgeSeconds"]=(millis()-r.at)/1000;
        out["trend"]=r.trend==battery::Trend::Rising?"rising":r.trend==battery::Trend::Falling?"falling":r.trend==battery::Trend::Steady?"steady":"learning";
        if(r.trendReady)out["voltageRateMvPerMin"]=r.rate;
    }
}
