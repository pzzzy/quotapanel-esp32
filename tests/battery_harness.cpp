// SPDX-License-Identifier: GPL-3.0-only
#include "../firmware/src/battery_model.h"
#include "../firmware/src/battery_runtime.h"
#include "../firmware/src/voice_mulaw.h"
#include <cassert>
#include <limits>
int main(){
    using namespace battery;
    assert(percent(0)==-1&&percent(4500)==-1&&percent(NAN)==-1);
    assert(percent(3300)==0&&percent(4200)==100&&percent(3800)==40);
    int previous=0;for(int mv=2500;mv<=4350;mv++){int p=percent(mv);assert(p>=previous&&p<=100&&p%5==0);previous=p;}
    Monitor m;for(unsigned i=0;i<120;i++)m.add(i*5000,3900+(int(i%3)-1)*2);
    assert(m.reading.valid&&m.reading.trendReady&&m.reading.trend==Trend::Steady);
    assert(std::fabs(m.reading.rate)<0.1);
    m.reset();for(unsigned i=0;i<80;i++)m.add(i*5000,3800+i*0.5f);
    assert(m.reading.trend==Trend::Rising&&m.reading.rate>5&&m.reading.rate<7);
    m.reset();for(unsigned i=0;i<80;i++)m.add(i*5000,4000-i*0.5f);
    assert(m.reading.trend==Trend::Falling&&m.reading.rate<-5&&m.reading.rate>-7);
    m.add(400000,4300);assert(!m.reading.trendReady&&m.count==1);
    m.add(405000,0);assert(!m.reading.valid&&m.count==0);
    m.add(410000,3900);assert(m.reading.percentage==60&&!m.reading.trendReady);
    m.add(500000,3901);assert(m.count==1); // A sampling gap restarts the trend.
    m.reset();for(unsigned i=0;i<1000;i++)m.add(uint32_t(0xffff0000u+i*5000u),3800);
    assert(m.count==Monitor::Capacity&&m.reading.trend==Trend::Steady&&m.reading.coverageMs==595000);
    m.reset();m.add(1,4200);assert(m.reading.trend==Trend::Learning); // High voltage is not proof of charging/full.
    PowerSession power;
    m.reset();m.add(0,4130);
    assert(!power.observe(5000,4130,m.reading,false)); // Empty charger-driven socket is never auto-confirmed.
    assert(!power.start(true,true,4130)); // A USB host invalidates battery-only operation.
    assert(power.start(true,false,3800));m.reset();m.add(0,3800);
    assert(power.observe(5000,3790,m.reading,false));
    assert(!power.observe(10000,4130,m.reading,false)); // Charger/battery-removal step revokes the run.
    assert(power.start(true,false,3800));
    assert(!power.observe(20000,3790,m.reading,false)); // Stale observations fail closed.
    assert(power.start(true,false,3800));
    assert(!power.observe(5000,0,m.reading,false));
    assert(power.start(true,false,3800));
    assert(!power.observe(5000,3841,m.reading,false)); // Sustained rise, even below jump threshold.

    Runtime runtime;
    for(unsigned i=0;i<16;i++)runtime.add(i*60000,3900-i*2);
    auto e=runtime.estimate(900000,3870);assert(e.available&&e.seconds>6000&&e.seconds<9000);
    assert(e.lowSeconds<e.seconds&&e.highSeconds>e.seconds);
    assert(!runtime.estimate(1100000,3870).available); // Stale history.
    for(unsigned i=16;i<35;i++)runtime.add(i*60000,3870);
    assert(!runtime.estimate(2040000,3870).available); // A recent plateau invalidates earlier drain pace.
    runtime.reset();for(unsigned i=0;i<80;i++)runtime.add(i*60000,3700+i);
    assert(!runtime.estimate(4740000,3779).available); // Charging is not discharge.
    runtime.reset();for(unsigned i=0;i<15;i++)runtime.add(i*60000,3900-i*2);
    assert(!runtime.estimate(840000,3872).available); // Too short, even if slope looks good.
    runtime.reset();for(unsigned i=0;i<40;i++)runtime.add(i*60000,3900-i*0.1f);
    assert(!runtime.estimate(2340000,3896.1).available); // Insufficient signal.
    runtime.reset();for(unsigned i=0;i<200;i++)runtime.add(uint32_t(0xfff00000u+i*60000u),4100-i*2);
    assert(runtime.estimate(uint32_t(0xfff00000u+199*60000u),3702).available); // Ring and clock wrap.
    runtime.reset();assert(!runtime.estimate(12000000,3702).available);

    WarningLatch alerts;
    assert(!alerts.update(0,false,true,3500));assert(!alerts.update(120000,false,true,3500));
    assert(!alerts.update(0,true,true,3640));assert(alerts.update(60000,true,true,3640)==1);
    alerts.commit(1);assert(!alerts.update(120000,true,true,3640));
    assert(!alerts.update(130000,true,true,3500));assert(alerts.update(160000,true,true,3500)==2);
    alerts.commit(2);assert(alerts.warned==3);
    WarningLatch reboot;reboot.warned=alerts.warned;
    assert(!reboot.update(0,true,true,3500));assert(!reboot.update(120000,true,true,3500));
    reboot.update(130000,true,true,3800);reboot.update(250000,true,true,3800);assert(reboot.warned==0);
    reboot.update(260000,true,true,3500);assert(reboot.update(290000,true,true,3500)==2);
    reboot.commit(2);assert(!reboot.update(320000,true,true,3500)); // No duplicate low after critical.
    WarningLatch interrupted;interrupted.update(0,true,true,3640);interrupted.update(50000,false,true,3640);
    assert(!interrupted.update(60000,true,true,3640));assert(interrupted.update(120000,true,true,3640)==1);
    assert(decodeMulaw(255)==0&&decodeMulaw(127)==0&&decodeMulaw(128)==32124&&decodeMulaw(0)==-32124);
}
