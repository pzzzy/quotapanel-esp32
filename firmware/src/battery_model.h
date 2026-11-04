// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace battery {
// A generic 1S LiPo voltage curve, not a fuel gauge or a calibrated cell model.
// Deliberately display in 5% steps. Load, temperature and charging affect it.
inline int percent(float mv) {
    if(!std::isfinite(mv)||mv<2500||mv>4350)return -1;
    const float volts[]={3300,3500,3600,3700,3750,3800,3850,3900,4000,4100,4200};
    const int levels[]={0,5,10,20,30,40,50,60,75,90,100};
    if(mv<=volts[0])return 0;
    for(int i=1;i<11;i++)if(mv<volts[i]){
        float p=levels[i-1]+(mv-volts[i-1])*(levels[i]-levels[i-1])/(volts[i]-volts[i-1]);
        return (int)std::round(p/5)*5;
    }
    return 100;
}
enum class Trend { Learning, Steady, Rising, Falling };
struct Sample { uint32_t at=0;float mv=0; };
struct Reading {
    bool valid=false,trendReady=false;
    float millivolts=0,rate=0;
    int percentage=-1;
    uint32_t at=0,coverageMs=0;
    Trend trend=Trend::Learning;
};
class Monitor {
public:
    static constexpr unsigned Capacity=120;
    Sample samples[Capacity]{};
    unsigned count=0,cursor=0;
    Reading reading;
    void reset(){count=cursor=0;reading=Reading{};}
    void add(uint32_t now,float mv) {
        if(percent(mv)<0){reset();return;}
        // Break history across gaps and abrupt supply/load changes, including
        // battery reconnection. Never extrapolate a step as a charging rate.
        if(reading.valid&&(now-reading.at>15000||std::fabs(mv-reading.millivolts)>100))reset();
        float filtered=reading.valid?reading.millivolts+(mv-reading.millivolts)*0.25f:mv;
        samples[cursor].at=now;samples[cursor].mv=filtered;
        cursor=(cursor+1)%Capacity;if(count<Capacity)count++;
        reading=Reading{};reading.valid=true;reading.at=now;reading.millivolts=filtered;reading.percentage=percent(filtered);
        const auto oldest=samples[(cursor+Capacity-count)%Capacity];
        reading.coverageMs=now-oldest.at;
        if(count<20||reading.coverageMs<180000)return;
        double sx=0,sy=0,sxx=0,sxy=0;
        for(unsigned i=0;i<count;i++){
            const auto s=samples[(cursor+Capacity-count+i)%Capacity];
            double x=(uint32_t)(s.at-oldest.at)/60000.0;
            sx+=x;sy+=s.mv;sxx+=x*x;sxy+=x*s.mv;
        }
        double denominator=count*sxx-sx*sx;
        if(denominator<=0)return;
        reading.rate=(count*sxy-sx*sy)/denominator;
        reading.trendReady=true;reading.trend=Trend::Steady;
        float change=filtered-oldest.mv;
        // Rate is terminal-voltage slope in mV/min, never battery current.
        if(reading.rate>=2&&change>=10)reading.trend=Trend::Rising;
        else if(reading.rate<=-2&&change<=-10)reading.trend=Trend::Falling;
    }
};
}
