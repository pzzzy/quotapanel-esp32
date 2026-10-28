#include "native_data.h"
#include "native_forecast.h"
#include "history_journal.h"
#include <Preferences.h>
#include <esp_partition.h>
#include <cmath>
#include <algorithm>
namespace {
using historyjournal::Record;
using historyjournal::MAGIC;
using historyjournal::hashBytes;
const uint32_t CAPACITY=32768;
Record* history=nullptr;uint32_t count=0,cursor=0;
const esp_partition_t* partition=nullptr;
bool journalReady=false;
struct FlashStorage {
    uint32_t size()const{return partition?partition->size:0;}
    bool read(uint32_t at,void* out,size_t size){return partition&&esp_partition_read(partition,at,out,size)==ESP_OK;}
    bool write(uint32_t at,const void* data,size_t size){return partition&&esp_partition_write(partition,at,data,size)==ESP_OK;}
    bool erase(uint32_t at,size_t size){return partition&&esp_partition_erase_range(partition,at,size)==ESP_OK;}
} flash;
historyjournal::Journal<FlashStorage> journal(flash);
JsonDocument previous[2],events;uint64_t eventSequence=0;
uint32_t hash(const String& s){return hashBytes(s.c_str(),s.length());}
String key(JsonObject w){return w["bucket"].as<String>()+":"+w["slot"].as<String>()+":"+String(w["minutes"].as<int>());}
void remember(const Record& r){history[cursor]=r;cursor=(cursor+1)%CAPACITY;if(count<CAPACITY)count++;}
bool add(uint32_t identity,uint32_t window,uint32_t reset,uint32_t at,float used,bool force=false) {
    if(!history||!at||!std::isfinite(used)||used<0||used>100)return false;
    const Record* latest=nullptr;const Record* saved=nullptr;
    for(uint32_t n=0;n<count;n++){const auto &r=history[n];
        if(r.identity==identity&&r.window==window&&r.reset==reset&&r.at==at&&r.used==used)return true;
        if(r.identity==identity&&r.window==window){
            if(!latest||r.at>latest->at)latest=&r;
            if(r.sequence&&(!saved||r.at>saved->at))saved=&r;
        }
    }
    if(latest&&latest->at==at&&latest->used==used&&latest->reset==reset)return true;
    Record r{MAGIC,0,identity,window,reset,at,used,0};
    bool durable=true;
    if(force||historyjournal::checkpoint(r,latest,saved))durable=journalReady&&journal.append(r);
    // Keep useful live observations even if flash is unavailable; retry saving
    // on subsequent polls, and report the failure instead of hiding it.
    remember(r);return journalReady&&durable;
}
int64_t utcDate(const char* value) {
    if(!value)return 0;int y,m,d,hh,mm,ss;
    if(sscanf(value,"%d-%d-%dT%d:%d:%d",&y,&m,&d,&hh,&mm,&ss)!=6||!strchr(value,'Z')||m<1||m>12||d<1||d>31||hh>23||mm>59||ss>60)return 0;
    y-=m<=2;int era=(y>=0?y:y-399)/400;unsigned yoe=y-era*400;
    unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1,doe=yoe*365+yoe/4-yoe/100+doy;
    return ((int64_t)era*146097+doe-719468)*86400+hh*3600+mm*60+ss;
}
void forecast(JsonObject output,const nativeforecast::Estimate& f){
    output["historical"]=f.historical;output["historyAgeSeconds"]=f.historyAgeSeconds;output["state"]=f.state;output["confidence"]=f.confidence;output["asOf"]=f.asOf;output["coverageSeconds"]=f.coverageSeconds;output["pointsConsumed"]=f.pointsConsumed;
    output["seconds"]=nullptr;output["lowSeconds"]=nullptr;output["highSeconds"]=nullptr;output["rate"]=nullptr;
    if(f.seconds>=0)output["seconds"]=f.seconds;if(f.lowSeconds>=0)output["lowSeconds"]=f.lowSeconds;if(f.highSeconds>=0)output["highSeconds"]=f.highSeconds;if(f.rate>=0)output["rate"]=f.rate;
    output["resetFirst"]=f.resetFirst;output["resetPossible"]=f.resetPossible;
}
void windows(JsonArray out,JsonObject limit,const String& bucket,const String& name,uint32_t identity,int64_t now){
    const char* slots[]={"primary","secondary"};
    for(auto slot:slots){JsonObject source=limit[String(slot)+"_window"].as<JsonObject>();if(source.isNull())continue;
        JsonObject w=out.add<JsonObject>();w["bucket"]=bucket;w["slot"]=slot;w["name"]=name;
        int seconds=source["limit_window_seconds"]|0;w["minutes"]=seconds?seconds/60:0;
        w["label"]=seconds==604800?"WEEK":seconds==18000?"5 HOUR":seconds?String(seconds/60)+" MIN":"WINDOW";
        double used=source["used_percent"].is<double>()?source["used_percent"].as<double>():NAN;
        w["used"]=nullptr;w["remaining"]=nullptr;if(std::isfinite(used)&&used>=0&&used<=100){w["used"]=used;w["remaining"]=100-used;}
        uint32_t reset=source["reset_at"]|0u;w["reset"]=reset?JsonVariant(source["reset_at"]):JsonVariant();
        uint32_t window=hash(key(w));w["historyHealthy"]=std::isfinite(used)&&add(identity,window,reset,now,used);
        std::vector<nativeforecast::Sample> samples;
        for(uint32_t n=0;n<count;n++){auto &r=history[n];if(r.identity==identity&&r.window==window&&r.reset==reset&&r.at>=now-86400&&r.at<=now)samples.push_back({(double)r.at,r.used});}
        forecast(w["forecast"].to<JsonObject>(),nativeforecast::estimate(samples,100-used,reset,now));
    }
}
void emit(const char* type,int slot,int64_t now){
    eventSequence=std::max(eventSequence+1,(uint64_t)now*1000);
    auto e=events.as<JsonArray>().add<JsonObject>();e["id"]=eventSequence;e["account"]=slot;e["type"]=type;e["at"]=now;
    while(events.size()>16)events.as<JsonArray>().remove(0);
}
bool storeBaseline(int slot,JsonDocument& doc){
    String value;serializeJson(doc,value);Preferences p;if(!p.begin("oa-native",false,"accounts"))return false;
    bool ok=p.putString(("prior"+String(slot)).c_str(),value)==value.length();p.end();return ok;
}
void alerts(int slot,const String& identity,JsonObject account,int64_t now,bool persistState=true){
    JsonDocument &prior=previous[slot];bool known=prior["identity"].as<String>()==identity;
    JsonObject old=prior["account"].as<JsonObject>();JsonDocument next;next["identity"]=identity;
    JsonObject snapshot=next["account"].to<JsonObject>();snapshot["banked"]=account["banked"];snapshot["bankExpires"]=account["bankExpires"];
    JsonArray saved=snapshot["windows"].to<JsonArray>();JsonObject latch=next["warnings"].to<JsonObject>();
    if(known)latch.set(prior["warnings"].as<JsonObjectConst>());
    bool exhausted=false,reset=false,usageFell=false;int warning=0;
    bool banksKnown=known&&!old["banked"].isNull()&&!account["banked"].isNull();
    bool expired=!old["bankExpires"].isNull()&&old["bankExpires"].as<int64_t>()<=now;
    bool bankDrop=banksKnown&&account["banked"].as<int>()<old["banked"].as<int>()&&!expired;
    for(JsonObject w:account["windows"].as<JsonArray>()) {
        String k=w["bucket"].as<String>()+":"+w["slot"].as<String>();JsonObject before;
        if(known)for(JsonObject candidate:old["windows"].as<JsonArray>())if(candidate["bucket"].as<String>()==w["bucket"].as<String>()&&candidate["slot"].as<String>()==w["slot"].as<String>()){before=candidate;break;}
        bool haveBefore=!before.isNull(),usedKnown=haveBefore&&!before["used"].isNull()&&!w["used"].isNull();
        bool fell=usedKnown&&w["used"].as<double>()<before["used"].as<double>();
        int64_t oldReset=before["reset"]|0LL,newReset=w["reset"]|0LL;
        bool changed=oldReset&&newReset&&oldReset!=newReset,elapsed=oldReset&&now>=oldReset;
        bool latch10=false,latch5=false;
        if(known)for(int threshold:prior["warnings"][k].as<JsonArray>()){latch10|=threshold==10;latch5|=threshold==5;}
        if((fell&&(changed||bankDrop||elapsed))||(changed&&elapsed)){latch10=false;latch5=false;}
        if(!w["remaining"].isNull()) {
            double r=w["remaining"];
            if(r>0 && r<=10 && !latch10)warning=warning?std::min(warning,10):10;
            if(r>0 && r<=5 && !latch5)warning=5;
            latch10|=r<=10;latch5|=r<=5;
            if(r==0&&(!known||(haveBefore&&!before["remaining"].isNull()&&before["remaining"].as<double>()>0)))exhausted=true;
        }
        auto remembered=latch[k].to<JsonArray>();if(latch5)remembered.add(5);if(latch10)remembered.add(10);
        auto s=saved.add<JsonObject>();for(const char* field:{"bucket","slot","used","remaining","reset"})s[field]=w[field];
        usageFell|=fell;reset|=fell&&(changed||elapsed);
    }
    if(banksKnown&&account["banked"].as<int>()>old["banked"].as<int>())emit("banked_gained",slot,now);
    if(bankDrop&&usageFell)emit("banked_used",slot,now);else if(reset)emit("reset",slot,now);
    if(exhausted)emit("exhausted",slot,now);else if(warning)emit(warning==5?"warning_5":"warning_10",slot,now);
    String a,b;serializeJson(prior,a);serializeJson(next,b);
    if(a!=b){if(!persistState||storeBaseline(slot,next))prior.set(next);}
}
}
bool nativeDataBegin(){
    if(history)return journalReady;
    partition=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,(esp_partition_subtype_t)0x40,"history");
    if(!partition)return false;
    history=(Record*)ps_calloc(CAPACITY,sizeof(Record));if(!history)return false;
    journalReady=journal.restore([](const Record& r){remember(r);});
    events.to<JsonArray>();Preferences p;if(p.begin("oa-native",true,"accounts")){for(int i=0;i<2;i++)deserializeJson(previous[i],p.getString(("prior"+String(i)).c_str(),"{}"));p.end();}
    return journalReady;
}
uint32_t nativeHistoryCount(){return count;}
bool nativeDataSeed(const String& identity,JsonObject input){
    if(!nativeDataBegin())return false;
    String name=input["window"]|"";uint32_t reset=input["reset"]|0u;if(!name.length()||!input["samples"].is<JsonArray>())return false;
    for(JsonArray sample:input["samples"].as<JsonArray>()){
        if(sample.size()!=2||!sample[0].is<double>()||!sample[1].is<double>())return false;
        if(!add(hash(identity),hash(name),reset,sample[0].as<uint32_t>(),sample[1].as<float>(),true))return false;
    }return true;
}
bool nativeDataBaseline(int slot,const String& identity,JsonObject prior){
    if(slot<0||slot>1||!nativeDataBegin())return false;
    JsonDocument doc;doc.set(prior);doc["identity"]=identity;
    if(!storeBaseline(slot,doc))return false;previous[slot].set(doc);return true;
}
void nativeDataUpdate(int slot,const String& identity,JsonObject usage,JsonObject credits,JsonObject out,int64_t now){
    out["label"]=slot==0?"PRIMARY":"SECONDARY";out["plan"]=usage["plan_type"]|"unknown";out["updated"]=now;out["error"]=nullptr;
    out["credits"]=usage["credits"]["balance"];out["unlimited"]=usage["credits"]["unlimited"]|false;
    out["allowed"]=usage["rate_limit"]["allowed"];out["spendControl"]=usage["spend_control"]["reached"];
    out["banked"]=credits["available_count"];out["bankExpires"]=nullptr;int available=0;int64_t expiry=0;bool datesComplete=true;
    for(JsonObject c:credits["credits"].as<JsonArray>())if(c["status"].as<String>()=="available"){
        available++;int64_t at=utcDate(c["expires_at"].as<const char*>());if(at>0&&(!expiry||at<expiry))expiry=at;
        if(!c["expires_at"].isNull()&&!at)datesComplete=false;
    }
    if(expiry)out["bankExpires"]=expiry;
    out["bankExpiryComplete"]=credits["credits"].is<JsonArray>()&&!credits["available_count"].isNull()&&available>=credits["available_count"].as<int>()&&datesComplete;
    JsonArray ws=out["windows"].to<JsonArray>();windows(ws,usage["rate_limit"].as<JsonObject>(),"codex","codex",hash(identity),now);
    windows(ws,usage["code_review_rate_limit"].as<JsonObject>(),"code_review","Code review",hash(identity),now);
    for(JsonObject more:usage["additional_rate_limits"].as<JsonArray>())windows(ws,more["rate_limit"].as<JsonObject>(),more["metered_feature"]|"additional",more["limit_name"]|"Additional",hash(identity),now);
    JsonObject choice;double minimum=1e30;bool partial=false,hasNormal=false;
    for(JsonObject w:ws)if(w["bucket"].as<String>()=="codex")hasNormal=true;
    for(JsonObject w:ws){if(hasNormal&&w["bucket"].as<String>()!="codex")continue;JsonObject f=w["forecast"];
        String state=f["state"]|"";partial|=state!="estimate"&&state!="exhausted";
        if(choice.isNull())choice=w;
        if(!f["seconds"].isNull()&&f["seconds"].as<double>()<minimum){minimum=f["seconds"];choice=w;}
    }
    if(!choice.isNull()){out["forecast"]=choice["forecast"];out["forecast"]["window"]=choice["label"];out["forecast"]["partial"]=partial;}
    else{out["forecast"]["state"]="unavailable";out["forecast"]["seconds"]=nullptr;}
    alerts(slot,identity,out,now);
}
void nativeDataEvents(JsonArray output,int64_t now){for(JsonObject e:events.as<JsonArray>())if(now-e["at"].as<int64_t>()<600)output.add(e);}
void nativeDataAlertTest(JsonArray cases,JsonArray results){
    JsonDocument savedPrior,savedEvents;savedPrior.set(previous[0]);savedEvents.set(events);uint64_t savedSequence=eventSequence;
    for(JsonObject c:cases){
        previous[0].set(c["prior"]);events.clear();events.to<JsonArray>();
        alerts(0,"test",c["account"].as<JsonObject>(),c["now"].as<int64_t>(),false);
        auto result=results.add<JsonObject>();auto types=result["events"].to<JsonArray>();
        for(JsonObject event:events.as<JsonArray>())types.add(event["type"]);
        result["warnings"]=previous[0]["warnings"];
    }
    previous[0].set(savedPrior);events.set(savedEvents);eventSequence=savedSequence;
}
