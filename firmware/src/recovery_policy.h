// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
namespace recovery {
struct Calendar {int year,month,day,hour,minute,second;};
inline int compare(Calendar a,Calendar b){
    const int left[]={a.year,a.month,a.day,a.hour,a.minute,a.second};
    const int right[]={b.year,b.month,b.day,b.hour,b.minute,b.second};
    for(unsigned i=0;i<6;i++)if(left[i]!=right[i])return left[i]<right[i]?-1:1;
    return 0;
}
inline int certificateTime(Calendar first,Calendar last,Calendar now){
    return compare(now,first)<0?-1:compare(now,last)>0?1:0;
}
// Arduino HTTPClient returns -1 only when connect() failed, before sending
// headers or a body. All other failed refresh outcomes remain ambiguous.
inline bool refreshDefinitelyUnsent(int http) { return http == -1; }
inline uint32_t retryMs(unsigned failures, uint32_t retryAfterSeconds) {
    const uint32_t delays[] = {5000,15000,30000,60000,120000,300000};
    unsigned index = failures ? failures-1 : 0;
    if(index>5)index=5;
    uint32_t server = (retryAfterSeconds>3600?3600:retryAfterSeconds)*1000;
    return delays[index]>server?delays[index]:server;
}
inline const char* requestError(int http,int tls) {
    if(http<0) return tls==-0x2700||tls==-0x3000 ? "TLS CERTIFICATE ERROR" : "CONNECTION FAILED";
    if(http==401) return "SIGN IN AGAIN";
    if(http==403) return "ACCESS BLOCKED";
    if(http==429) return "SERVER RATE LIMIT";
    if(http>=500) return "SERVER UNAVAILABLE";
    return "INVALID SERVER DATA";
}
}
