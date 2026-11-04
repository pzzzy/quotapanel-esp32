// SPDX-License-Identifier: GPL-3.0-only
#include "battery_monitor.h"
#include "board_pins.h"
#include <Arduino.h>
#include <Preferences.h>
#include "alerts_audio.h"
#include <atomic>

namespace {
battery::Monitor monitor;
portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
uint32_t sampledAt=0;
bool sampled=false;
battery::Runtime runtime;
battery::RuntimeEstimate estimate;
battery::WarningLatch warnings;
battery::PowerSession power;
std::atomic<uint8_t> storedWarnings{0};
}
void batteryBegin(){
    Preferences p;if(p.begin("qp-battery",true)){warnings.warned=p.getUChar("warned",0)&3;p.end();}
    storedWarnings=warnings.warned;
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
    bool usbHost=Serial.isConnected();
    portENTER_CRITICAL(&lock);
    if(power.confirmed&&!power.observe(now,mv,monitor.reading,usbHost)){
        runtime.reset();estimate={};warnings.interrupt();
    }
    monitor.add(now,mv);
    if(power.confirmed){
        runtime.add(now,monitor.reading.millivolts);estimate=runtime.estimate(now,monitor.reading.millivolts);
    }
    int warning=warnings.update(now,power.confirmed,monitor.reading.valid,monitor.reading.millivolts);
    portEXIT_CRITICAL(&lock);
    if(warning&&audioQueueBatteryWarning(warning==2))warnings.commit(warning);
    if(storedWarnings!=warnings.warned){
        Preferences p;if(p.begin("qp-battery",false)&&p.putUChar("warned",warnings.warned)==1)storedWarnings=warnings.warned;p.end();
    }
}
battery::Reading batteryReading(){
    portENTER_CRITICAL(&lock);auto r=monitor.reading;r.confirmed=power.confirmed;portEXIT_CRITICAL(&lock);
    if(r.valid&&millis()-r.at>15000){r.valid=false;r.confirmed=false;r.percentage=-1;r.trendReady=false;}
    if(!r.confirmed)r.percentage=-1;
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
    out["batteryOnlyConfirmedByUser"]=r.confirmed;out["presenceDetected"]=nullptr;
    out["voltage"]=nullptr;out["percentage"]=nullptr;out["voltageRateMvPerMin"]=nullptr;
    out["charging"]=nullptr;out["externalPower"]=nullptr;out["currentMa"]=nullptr;out["inputVoltage"]=nullptr;out["powerWatts"]=nullptr;
    out["trend"]="unavailable";out["coverageSeconds"]=r.coverageMs/1000;
    if(r.valid){
        out["voltage"]=r.millivolts/1000;if(r.confirmed)out["percentage"]=r.percentage;out["sampleAgeSeconds"]=(millis()-r.at)/1000;
        out["trend"]=r.trend==battery::Trend::Rising?"rising":r.trend==battery::Trend::Falling?"falling":r.trend==battery::Trend::Steady?"steady":"learning";
        if(r.trendReady)out["voltageRateMvPerMin"]=r.rate;
    }
    auto e=batteryRuntime();out["runtimeSeconds"]=nullptr;out["runtimeLowSeconds"]=nullptr;out["runtimeHighSeconds"]=nullptr;
    out["runtimeCoverageSeconds"]=e.coverageMs/1000;out["runtimeReserveVolts"]=3.5;
    out["runtimeConfidence"]="low";
    out["runtimeState"]=!r.confirmed?"confirm_battery_only":r.millivolts<=3500?"reserve_reached":e.available?"estimate":"learning";
    if(r.confirmed&&e.available){out["runtimeSeconds"]=e.seconds;out["runtimeLowSeconds"]=e.lowSeconds;out["runtimeHighSeconds"]=e.highSeconds;}
    out["warningLatch"]=storedWarnings.load();
}
bool batterySetMode(bool enabled){
    if(enabled&&(Serial.isConnected()||!batteryReading().valid))return false;
    portENTER_CRITICAL(&lock);if(enabled)power.start(monitor.reading.valid,false,monitor.reading.millivolts);else power.end();
    runtime.reset();estimate={};warnings.interrupt();portEXIT_CRITICAL(&lock);return true;
}
battery::RuntimeEstimate batteryRuntime(){
    portENTER_CRITICAL(&lock);auto e=estimate;bool enabled=power.confirmed;portEXIT_CRITICAL(&lock);
    if(!enabled||!batteryReading().valid)return {};return e;
}
bool batteryWarningEligible(bool critical){
    auto r=batteryReading();return r.valid&&r.confirmed&&battery::continuousPercent(r.millivolts)<=(critical?5:15);
}
