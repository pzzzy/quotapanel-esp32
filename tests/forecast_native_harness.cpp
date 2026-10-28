#include "../firmware/src/native_forecast.h"
#include <iostream>
#include <iomanip>
int main(){long long now,reset;double remaining;int count;
 while(std::cin>>now>>reset>>remaining>>count){std::vector<nativeforecast::Sample>s;for(int i=0;i<count;i++){double t,u;std::cin>>t>>u;s.push_back({t,u});}auto r=nativeforecast::estimate(s,remaining,reset,now);
 std::cout<<std::setprecision(15)<<r.state<<' '<<r.confidence<<' '<<r.seconds<<' '<<r.lowSeconds<<' '<<r.highSeconds<<' '<<r.rate<<' '<<r.coverageSeconds<<' '<<r.resetFirst<<' '<<r.resetPossible<<' '<<r.historical<<' '<<r.historyAgeSeconds<<'\n';}}
