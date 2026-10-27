#include "../firmware/src/recovery_policy.h"
#include <cassert>
#include <cstring>
#include <climits>
int main(){
    using namespace recovery;
    Calendar first{2026,10,27,8,0,0},last{2027,1,25,8,0,0};
    assert(certificateTime(first,last,first)==0);assert(certificateTime(first,last,last)==0);
    assert(certificateTime(first,last,{2026,10,27,7,59,59})==-1);
    assert(certificateTime(first,last,{2027,1,25,8,0,1})==1);
    assert(certificateTime(first,last,{2026,12,31,23,59,59})==0);
    assert(compare({2024,2,29,0,0,0},{2024,3,1,0,0,0})<0);
    assert(compare({2027,1,1,0,0,0},{2026,12,31,23,59,59})>0);
    for(int code=-20;code<=599;code++)assert(refreshDefinitelyUnsent(code)==(code==-1));
    assert(retryMs(1,0)==5000);assert(retryMs(2,0)==15000);
    assert(retryMs(3,0)==30000);assert(retryMs(4,0)==60000);
    assert(retryMs(5,0)==120000);assert(retryMs(6,0)==300000);
    assert(retryMs(UINT_MAX,0)==300000);assert(retryMs(1,120)==120000);
    assert(retryMs(1,UINT_MAX)==3600000);
    assert(!strcmp(requestError(-1,-0x2700),"TLS CERTIFICATE ERROR"));
    assert(!strcmp(requestError(-1,-0x3000),"TLS CERTIFICATE ERROR"));
    assert(!strcmp(requestError(-1,0),"CONNECTION FAILED"));
    assert(!strcmp(requestError(401,0),"SIGN IN AGAIN"));
    assert(!strcmp(requestError(429,0),"SERVER RATE LIMIT"));
    assert(!strcmp(requestError(503,0),"SERVER UNAVAILABLE"));
}
