// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <cstring>
namespace otaformat {
constexpr size_t METADATA_SIZE=112,HEADER_SIZE=176,MAX_IMAGE=0x400000;
constexpr char BOARD[]="quotapanel-esp32s3-v1";
struct Metadata {uint32_t sequence,size;char version[32];uint8_t digest[32];};
inline uint32_t little32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
inline bool equal(const void* a,const void* b,size_t n){
    auto x=(const uint8_t*)a,y=(const uint8_t*)b;uint8_t difference=0;
    for(size_t i=0;i<n;i++)difference|=x[i]^y[i];return difference==0;
}
inline bool parse(const uint8_t* header,size_t size,uint32_t floor,Metadata& out){
    if(size!=HEADER_SIZE||memcmp(header,"QPOTA001",8))return false;
    char board[32]={};memcpy(board,BOARD,sizeof(BOARD));
    if(memcmp(header+8,board,32))return false;
    out.sequence=little32(header+40);out.size=little32(header+44);
    if(out.sequence<=floor||out.size<256||out.size>MAX_IMAGE)return false;
    memcpy(out.digest,header+48,32);memcpy(out.version,header+80,32);
    bool ended=false;
    for(char c:out.version){
        if(c==0){ended=true;continue;}
        if(ended||!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'))return false;
    }
    return ended&&out.version[0];
}
}
