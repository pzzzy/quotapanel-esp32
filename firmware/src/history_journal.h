// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <cstring>
#include <cmath>

namespace historyjournal {
// Keep the alpha.1/alpha.2 layout: upgrades retain existing history.
struct Record {uint32_t magic,sequence,identity,window,reset,at;float used;uint32_t crc;};
static_assert(sizeof(Record)==32,"Journal record layout");
constexpr uint32_t MAGIC=0x3148414f,SECTOR=4096;
inline uint32_t hashBytes(const void* bytes,size_t size){
    uint32_t h=2166136261u;auto p=(const uint8_t*)bytes;
    for(size_t i=0;i<size;i++)h=(h^p[i])*16777619u;
    return h;
}
inline bool valid(const Record& r){
    return r.magic==MAGIC && r.sequence && r.at &&
        r.crc==hashBytes(&r,sizeof(r)-4) && std::isfinite(r.used) && r.used>=0 && r.used<=100;
}
inline bool newer(uint32_t a,uint32_t b){return (int32_t)(a-b)>0;}
inline bool checkpoint(const Record& r,const Record* latest,const Record* saved){
    if(!latest||!saved)return true;
    if(r.reset!=latest->reset||r.used<latest->used)return true;
    if(r.reset!=saved->reset||r.used<saved->used)return true;
    // Five-minute heartbeat; save changes of >=1 point within one minute.
    return r.at>=saved->at && (r.at-saved->at>=300 ||
        (r.at-saved->at>=60 && r.used-saved->used>=1));
}
// Storage supplies size(), read(), write(), erase(). Tests use NOR semantics.
template<class Storage> class Journal {
    Storage& storage;
    uint32_t sequence=0,index=0;
    bool found=false;
public:
    explicit Journal(Storage& s):storage(s){}
    template<class Visitor> bool restore(Visitor visit){
        sequence=0;index=0;found=false;
        if(!storage.size()||storage.size()%SECTOR)return false;
        for(uint32_t i=0;i<storage.size()/sizeof(Record);i++){
            Record r;if(!storage.read(i*sizeof(r),&r,sizeof(r)))return false;
            if(!valid(r))continue;
            visit(r);
            if(!found||newer(r.sequence,sequence)){sequence=r.sequence;index=i+1;found=true;}
        }
        return true;
    }
    bool append(Record& r){
        if(!storage.size()||storage.size()%SECTOR)return false;
        const uint32_t slots=storage.size()/sizeof(Record),perSector=SECTOR/sizeof(Record);
        index%=slots;Record existing;
        if(!storage.read(index*sizeof(Record),&existing,sizeof(existing)))return false;
        bool erased=true;
        for(size_t i=0;i<sizeof(existing);i++)if(((uint8_t*)&existing)[i]!=0xff)erased=false;
        // A torn record cannot be overwritten on NOR; skip to the next sector.
        if(index%perSector && !erased)index=((index/perSector+1)*perSector)%slots;
        if(index%perSector==0&&!storage.erase(index*sizeof(Record),SECTOR))return false;
        Record next=r;next.magic=MAGIC;next.sequence=sequence+1;
        if(!next.sequence)next.sequence=1; // zero marks a RAM-only sample
        next.crc=hashBytes(&next,sizeof(next)-4);
        if(!storage.write(index*sizeof(Record),&next,sizeof(next)))return false;
        Record verify;
        if(!storage.read(index*sizeof(Record),&verify,sizeof(verify))||memcmp(&next,&verify,sizeof(next)))return false;
        sequence=next.sequence;index=(index+1)%slots;found=true;r=next;return true;
    }
};
}
