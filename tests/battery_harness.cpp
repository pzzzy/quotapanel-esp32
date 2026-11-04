// SPDX-License-Identifier: GPL-3.0-only
#include "../firmware/src/battery_model.h"
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
}
