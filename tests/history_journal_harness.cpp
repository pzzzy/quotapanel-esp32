#include "../firmware/src/history_journal.h"
#include "../firmware/src/native_forecast.h"
#include <vector>
#include <cassert>
#include <algorithm>
#include <limits>
using namespace historyjournal;
struct Flash {
    std::vector<uint8_t> bytes=std::vector<uint8_t>(16384,0xff);
    int torn=-1;bool failErase=false,failRead=false;unsigned erases=0;
    uint32_t size()const{return bytes.size();}
    bool read(uint32_t at,void* out,size_t n){
        assert(at+n<=bytes.size());if(failRead)return false;
        memcpy(out,bytes.data()+at,n);return true;
    }
    bool erase(uint32_t at,size_t n){
        assert(at%4096==0&&n==4096);if(failErase)return false;
        std::fill(bytes.begin()+at,bytes.begin()+at+n,0xff);erases++;return true;
    }
    bool write(uint32_t at,const void* data,size_t n){
        assert(at+n<=bytes.size());auto p=(const uint8_t*)data;
        size_t written=torn<0?n:std::min(n,(size_t)torn);
        for(size_t i=0;i<written;i++){assert((bytes[at+i]&p[i])==p[i]);bytes[at+i]&=p[i];}
        return torn<0;
    }
};
Record sample(uint32_t at,float used=10,uint32_t identity=1,uint32_t window=2,uint32_t reset=2100000){
    return Record{MAGIC,0,identity,window,reset,at,used,0};
}
int main(){
    Flash flash;Journal<Flash> journal(flash);std::vector<Record> loaded;
    assert(journal.restore([&](const Record& r){loaded.push_back(r);}));assert(loaded.empty());
    auto first=sample(2000000);assert(checkpoint(first,nullptr,nullptr));assert(journal.append(first));assert(valid(first));
    auto later=sample(2000030,11);assert(!checkpoint(later,&first,&first));
    later.at+=30;assert(checkpoint(later,&first,&first));
    later=sample(2000300);assert(checkpoint(later,&first,&first));
    later=sample(2000001,9);assert(checkpoint(later,&first,&first));
    later=sample(2000001,10,1,2,2200000);assert(checkpoint(later,&first,&first));
    // Each possible torn write is ignored on reboot; a following append works.
    for(int split=0;split<32;split++){
        Flash f=flash;Journal<Flash> before(f);assert(before.restore([](const Record&){}));
        auto next=sample(2000300,11);f.torn=split;assert(!before.append(next));assert(next.sequence==0);
        f.torn=-1;Journal<Flash> after(f);loaded.clear();
        assert(after.restore([&](const Record&r){loaded.push_back(r);}));assert(loaded.size()==1);
        assert(after.append(next));assert(valid(next));
        Journal<Flash> reboot(f);loaded.clear();assert(reboot.restore([&](const Record&r){loaded.push_back(r);}));assert(loaded.size()==2);
    }
    // Ring wrap erases only the oldest sector and resumes at the newest record.
    for(unsigned i=1;i<900;i++){auto r=sample(2000000+i*30,i%100);assert(journal.append(r));}
    Journal<Flash> reboot(flash);loaded.clear();assert(reboot.restore([&](const Record&r){loaded.push_back(r);}));
    assert(loaded.size()>=385&&loaded.size()<=512);
    assert(std::max_element(loaded.begin(),loaded.end(),[](const Record&a,const Record&b){return a.at<b.at;})->at==2026970);
    auto next=sample(2027000);assert(reboot.append(next));assert(next.sequence==901);
    // Checksum corruption and read/erase failures are never accepted as saved.
    flash.bytes[0]^=1;Journal<Flash> damaged(flash);loaded.clear();assert(damaged.restore([&](const Record&r){assert(valid(r));loaded.push_back(r);}));
    Flash failures;Journal<Flash> bad(failures);assert(bad.restore([](const Record&){}));
    failures.failErase=true;next=sample(2000000);assert(!bad.append(next));assert(next.sequence==0);
    failures.failErase=false;failures.failRead=true;assert(!bad.restore([](const Record&){}));assert(!bad.append(next));
    // Sequence wrap remains chronological and zero remains reserved for RAM.
    Flash wrap;auto tail=sample(2000000);tail.sequence=UINT32_MAX;tail.crc=hashBytes(&tail,sizeof(tail)-4);
    memcpy(wrap.bytes.data(),&tail,sizeof(tail));Journal<Flash> w(wrap);assert(w.restore([](const Record&){}));
    next=sample(2000030);assert(w.append(next));assert(next.sequence==1);
    Journal<Flash> w2(wrap);assert(w2.restore([](const Record&){}));next=sample(2000060);assert(w2.append(next));assert(next.sequence==2);
    // Use the real durable record format and estimator across a reboot/gap.
    Flash history;Journal<Flash> h(history);assert(h.restore([](const Record&){}));
    std::vector<nativeforecast::Sample> live;
    for(unsigned i=0;i<=36;i++){
        auto r=sample(2000000-10800+i*300,10+i*300*10.0/3600);
        assert(h.append(r));live.push_back({(double)r.at,r.used});
        auto other=sample(r.at,95,99);assert(h.append(other));
        auto oldCycle=sample(r.at,90,1,2,1900000);assert(h.append(oldCycle));
    }
    auto expected=nativeforecast::estimate(live,60,2100000,2000000);
    Journal<Flash> restored(history);std::vector<nativeforecast::Sample> saved;
    assert(restored.restore([&](const Record&r){if(r.identity==1&&r.window==2&&r.reset==2100000)saved.push_back({(double)r.at,r.used});}));
    auto actual=nativeforecast::estimate(saved,60,2100000,2000000);
    assert(actual.seconds==expected.seconds&&actual.rate==expected.rate);
    saved.push_back({2003600,50});actual=nativeforecast::estimate(saved,50,2100000,2003600);
    assert(actual.historical&&actual.historyAgeSeconds==3600&&actual.seconds==18000&&actual.rate==10);
    saved.push_back({2003630,0});actual=nativeforecast::estimate(saved,100,2100000,2003630);
    assert(!actual.historical&&actual.seconds<0);
}
