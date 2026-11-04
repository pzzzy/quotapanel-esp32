// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "battery_model.h"

namespace battery {
class PowerSession {
    float minimum=0;
public:
    bool confirmed=false;
    void end(){confirmed=false;}
    bool start(bool valid,bool usbHost,float mv){
        if(!valid||usbHost||percent(mv)<0)return false;
        confirmed=true;minimum=mv;return true;
    }
    bool observe(uint32_t now,float mv,const Reading& before,bool usbHost){
        if(!confirmed)return false;
        if(percent(mv)<0||usbHost||(before.valid&&(now-before.at>15000||std::fabs(mv-before.millivolts)>100))||mv>minimum+40){end();return false;}
        // Use the filtered baseline so a single Wi-Fi/speaker load dip does
        // not turn its ordinary rebound into a presumed supply change.
        if(before.valid)minimum=std::min(minimum,before.millivolts);return true;
    }
};
inline float continuousPercent(float mv){
    if(percent(mv)<0)return -1;
    const float volts[]={3300,3500,3600,3700,3750,3800,3850,3900,4000,4100,4200};
    const float levels[]={0,5,10,20,30,40,50,60,75,90,100};
    if(mv<=3300)return 0;
    for(int i=1;i<11;i++)if(mv<volts[i])return levels[i-1]+(mv-volts[i-1])*(levels[i]-levels[i-1])/(volts[i]-volts[i-1]);
    return 100;
}
struct RuntimeEstimate {
    bool available=false;
    float seconds=0,lowSeconds=0,highSeconds=0,pointsPerHour=0;
    uint32_t coverageMs=0;
};
class Runtime {
    Sample samples[181]{};unsigned count=0,cursor=0;
    bool fit(uint32_t now,uint32_t horizon,float& rate)const{
        double sx=0,sy=0,sxx=0,sxy=0,syy=0;unsigned n=0;Sample first,last;
        for(unsigned i=0;i<count;i++){
            auto s=samples[(cursor+181-count+i)%181];if(now-s.at>horizon)continue;
            if(!n)first=s;last=s;n++;
            double x=(uint32_t)(s.at-first.at)/3600000.0,y=continuousPercent(s.mv);
            sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;syy+=y*y;
        }
        if(n<16||last.at-first.at<900000||first.mv-last.mv<30||continuousPercent(first.mv)-continuousPercent(last.mv)<3)return false;
        double xx=n*sxx-sx*sx,xy=n*sxy-sx*sy,yy=n*syy-sy*sy;
        if(xx<=0||yy<=0||xy>=0||xy*xy/(xx*yy)<0.6)return false;
        rate=-xy/xx;return std::isfinite(rate)&&rate>0;
    }
public:
    void reset(){count=cursor=0;}
    void add(uint32_t now,float mv){
        if(percent(mv)<0){reset();return;}
        if(count){auto last=samples[(cursor+180)%181];if(now-last.at>90000)reset();else if(now-last.at<60000)return;}
        samples[cursor].at=now;samples[cursor].mv=mv;cursor=(cursor+1)%181;if(count<181)count++;
    }
    RuntimeEstimate estimate(uint32_t now,float mv)const{
        RuntimeEstimate e;if(count<2||percent(mv)<0)return e;
        auto first=samples[(cursor+181-count)%181],last=samples[(cursor+180)%181];
        e.coverageMs=last.at-first.at;
        if(now-last.at>90000)return e;
        // Require a recent fall too: a historic drain rate cannot describe a
        // charger, resting cell or a newly idle workload.
        Sample recent=last;for(unsigned i=0;i<count;i++){auto s=samples[(cursor+181-count+i)%181];if(now-s.at<=360000){recent=s;break;}}
        if(now-recent.at<240000||recent.mv-mv<5)return e;
        float a=0,b=0;bool haveA=fit(now,1800000,a),haveB=fit(now,10800000,b);
        if(!haveA&&!haveB)return e;
        float rate=haveA&&haveB?0.7f*a+0.3f*b:haveA?a:b;
        float fastest=haveA&&haveB?std::max(a,b):rate,slowest=haveA&&haveB?std::min(a,b):rate;
        float remaining=std::max(0.0f,continuousPercent(mv)-5); // Reserve at 3.50 V, not predicted hardware shutdown.
        e.seconds=remaining/rate*3600;
        if(e.seconds>86400)return e;
        e.available=true;e.pointsPerHour=rate;
        // Sensitivity range, not a statistical confidence interval. Generic
        // voltage-to-charge curves and load changes dominate uncertainty.
        e.lowSeconds=remaining/(fastest*1.5f)*3600;e.highSeconds=std::min(86400.0f,remaining/(slowest*0.5f)*3600);
        return e;
    }
};
class WarningLatch {
    uint32_t lowAt=0,criticalAt=0,rearmAt=0;
    bool lowTiming=false,criticalTiming=false,rearmTiming=false;
public:
    uint8_t warned=0;
    void interrupt(){lowTiming=criticalTiming=rearmTiming=false;}
    // Return 1 (low) or 2 (critical). Caller commits only after queue acceptance.
    int update(uint32_t now,bool confirmed,bool valid,float mv){
        if(!confirmed||!valid){interrupt();return 0;}
        float p=continuousPercent(mv);
        if(p>=25){if(!rearmTiming){rearmTiming=true;rearmAt=now;}if(now-rearmAt>=120000)warned=0;}
        else rearmTiming=false;
        if(p<=15){if(!lowTiming){lowTiming=true;lowAt=now;}}else lowTiming=false;
        if(p<=5){if(!criticalTiming){criticalTiming=true;criticalAt=now;}}else criticalTiming=false;
        if(criticalTiming&&now-criticalAt>=30000&&!(warned&2))return 2;
        if(lowTiming&&now-lowAt>=60000&&!(warned&1))return 1;
        return 0;
    }
    void commit(int severity){warned|=severity==2?3:1;}
};
}
