// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <string.h>
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
inline bool refreshDue(int64_t now,int64_t expires,int64_t earliest){
    return now>=earliest&&expires<=now+300;
}
// A fresh access token rejected by the usage API may indicate an upstream
// policy or endpoint change. Avoid rotating every time that API returns 401.
inline bool retryUnauthorized(int64_t now,int64_t issued,int64_t earliest){
    return now>=earliest&&now-issued>=300;
}
inline const char* refreshFailure(int http,const char* code){
    if(http==400||http==401){
        if(!strcmp(code,"refresh_token_expired"))return "refresh_token_expired";
        if(!strcmp(code,"refresh_token_invalidated"))return "refresh_token_revoked";
        if(!strcmp(code,"refresh_token_reused"))return "refresh_token_reused";
        if(!strcmp(code,"invalid_grant"))return "refresh_grant_rejected";
    }
    if(http==200)return "refresh_response_invalid";
    return "refresh_outcome_unknown";
}
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
