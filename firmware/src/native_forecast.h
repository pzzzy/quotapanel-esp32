#pragma once
#include <vector>
#include <stdint.h>
namespace nativeforecast {
struct Sample { double at,used; };
struct Estimate {
    const char* state="learning";const char* confidence="LOW";
    double seconds=-1,lowSeconds=-1,highSeconds=-1,rate=-1,pointsConsumed=0;
    int64_t asOf=0;int coverageSeconds=0;bool resetFirst=false,resetPossible=false;
};
Estimate estimate(std::vector<Sample> samples,double remaining,int64_t reset,int64_t now);
}
