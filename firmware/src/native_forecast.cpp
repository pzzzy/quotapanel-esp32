#include "native_forecast.h"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace nativeforecast {
namespace {
// Input is one continuous, monotonic observation segment.
Estimate current(const std::vector<Sample>& segment,double remaining,int64_t reset,int64_t now) {
    Estimate r;r.asOf=now;
    if(segment.empty()||now-segment.back().at>90){r.state="stale";return r;}
    if(segment.size()<2)return r;
    auto end=segment.back();double span=end.at-segment.front().at,consumed=end.used-segment.front().used;
    r.coverageSeconds=(int)span;r.pointsConsumed=consumed;
    if(span<600)return r;
    auto baseline=[&segment](double cutoff){
        if(cutoff<=segment.front().at)return segment.front();
        for(size_t i=1;i<segment.size();i++) {
            auto a=segment[i-1],b=segment[i];
            if(a.at<=cutoff && cutoff<=b.at)return Sample{cutoff,a.used+(b.used-a.used)*(cutoff-a.at)/(b.at-a.at)};
        }return segment.back();
    };
    auto recent=baseline(end.at-3600);
    if(end.at-recent.at>=3600 && end.used-recent.used<1){r.state="no_burn";return r;}
    if(consumed<2)return r;
    const double horizons[]={900,3600,21600,86400},weights[]={.55,.30,.10,.05};
    double weighted=0,total=0,slow=1e30,fast=0;std::vector<double> seen;
    for(int i=0;i<4;i++) {
        auto start=baseline(end.at-horizons[i]);double elapsed=end.at-start.at;
        double rounded=std::round(start.at*1000)/1000;
        if(elapsed<600||std::find(seen.begin(),seen.end(),rounded)!=seen.end())continue;
        seen.push_back(rounded);double delta=std::max(0.0,end.used-start.used);
        double rate=delta*3600/elapsed,error=3600/elapsed;
        double quality=std::min(1.0,elapsed/horizons[i]);
        weighted+=rate*weights[i]*quality;total+=weights[i]*quality;
        slow=std::min(slow,std::max(0.0,rate-error));fast=std::max(fast,rate+error);
    }
    if(total<=0)return r;
    double rate=weighted/total;
    if(rate<=0){r.state="no_burn";return r;}
    double seconds=remaining/rate*3600,low=std::max(0.0,remaining-1)/fast*3600;
    double high=slow>0?(remaining+1)/slow*3600:-1,spread=fast/std::max(slow,.001);
    if(span>=3600 && consumed>=4 && spread<=3 && seconds<=span*8)r.confidence="MED";
    if(span>=7200 && consumed>=8 && spread<1.6 && seconds<=span*4)r.confidence="HIGH";
    r.state="estimate";r.seconds=std::round(seconds);r.lowSeconds=std::round(low);
    r.highSeconds=high>=0?std::round(high):-1;r.rate=std::round(rate*1000)/1000;
    r.resetFirst=reset>0 && low>=reset-now;r.resetPossible=reset>0 && seconds>=reset-now;
    return r;
}
} // namespace
Estimate estimate(std::vector<Sample> samples,double remaining,int64_t reset,int64_t now) {
    Estimate r;r.asOf=now;
    if(!std::isfinite(remaining)||remaining>100){r.state="unavailable";return r;}
    if(remaining<=0){r.state="exhausted";r.seconds=0;return r;}
    if(reset>0 && reset<=now){r.state="reset_pending";return r;}
    samples.erase(std::remove_if(samples.begin(),samples.end(),[now](const Sample& s){return !std::isfinite(s.at)||!std::isfinite(s.used)||s.used<0||s.used>100||s.at<now-86400||s.at>now;}),samples.end());
    std::stable_sort(samples.begin(),samples.end(),[](const Sample& a,const Sample& b){return a.at<b.at;});
    std::vector<Sample> unique;
    for(auto s:samples) {
        if(!unique.empty()&&s.at==unique.back().at)unique.pop_back();
        unique.push_back(s);
    }
    if(unique.empty()||now-unique.back().at>90){r.state="stale";return r;}
    std::vector<std::vector<Sample>> segments(1);
    for(auto s:unique) {
        // A correction/reset invalidates all earlier slopes, even across a gap.
        if(!segments.back().empty()) {
            auto previous=segments.back().back();
            if(s.used<previous.used){segments.clear();segments.emplace_back();}
            else if(s.at-previous.at>600)segments.emplace_back();
        }
        segments.back().push_back(s);
    }
    r=current(segments.back(),remaining,reset,now);
    if(r.seconds>=0 || strcmp(r.state,"learning")!=0)return r;
    // A fresh reading can use a recent saved trend while a new segment learns.
    // Never divide an unobserved usage jump by a polling interval.
    for(size_t i=segments.size()-1;i>0;--i) {
        const auto& prior=segments[i-1];double age=now-prior.back().at;
        if(age>21600)break;
        auto old=current(prior,remaining,0,(int64_t)prior.back().at);
        if(strcmp(old.state,"no_burn")==0)break;
        if(old.seconds<0)continue;
        old.asOf=now;old.historical=true;old.historyAgeSeconds=(int)age;old.confidence="LOW";
        // Unobserved workload changes widen sensitivity bounds with age.
        double uncertainty=1+age/3600.0;
        old.lowSeconds=std::floor(old.lowSeconds/uncertainty);
        if(old.highSeconds>=0)old.highSeconds=std::ceil(old.highSeconds*uncertainty);
        old.resetFirst=reset>0&&old.lowSeconds>=reset-now;
        old.resetPossible=reset>0&&old.seconds>=reset-now;
        return old;
    }
    return r;
}
}
