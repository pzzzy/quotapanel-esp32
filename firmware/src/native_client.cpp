// SPDX-License-Identifier: GPL-3.0-only
// Standalone HTTPS polling, authentication and bounded recovery.
#include "native_client.h"
#include "wifi_update.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include "verified_tls.h"
#include <HTTPClient.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <mbedtls/platform.h>
#include <esp_heap_caps.h>
#include <time.h>
#include <esp_sntp.h>
#include "recovery_policy.h"
#include <algorithm>
#include <atomic>
#include "native_roots.h"
#include "native_data.h"

namespace {
QueueHandle_t jobs;
SemaphoreHandle_t lock,updateLock;
struct ActivityGuard {
    ActivityGuard(){xSemaphoreTake(updateLock,portMAX_DELAY);}
    ~ActivityGuard(){xSemaphoreGive(updateLock);}
};
String result = "{\"state\":\"idle\"}";
String commandResult=result;bool inCommand=false;
String wifiResult,wifiRequestId;
bool wifiJobPending=false, wifiJobActive=false;
std::atomic<bool> cancelled{false},updatePause{false},updateLockHeld{false};
const char* CLIENT_ID="app_EMoamEEZ73f0CkXaXp7hrann";
struct BrowserFlow {
    String verifier,state,expected;
    int slot=-1;
    uint32_t started=0;
};
BrowserFlow browserFlows[2]; // USB and Wi-Fi each own their pending browser session.
std::atomic<bool> enabled{false};
bool storageReady=false;
JsonDocument cached[2];uint32_t nextPoll=0,pollCount=0,pollFailures=0,consecutiveFailures=0;
void publish(JsonDocument &doc) {
    if(wifiJobActive){
        JsonDocument safe;
        for(const char* key:{"state","error","authorizationUrl","stateToken","persisted","polls"})
            if(!doc[key].isNull())safe[key]=doc[key];
        String encoded;serializeJson(safe,encoded);
        xSemaphoreTake(lock,portMAX_DELAY);
        wifiResult=encoded;wifiJobPending=doc["state"]=="running";
        if(!wifiJobPending)wifiJobActive=false;
        xSemaphoreGive(lock);
        return;
    }
    String value;serializeJson(doc,value);
    xSemaphoreTake(lock,portMAX_DELAY);
    if(inCommand)commandResult=value;else result=value;
    xSemaphoreGive(lock);
}
class BoundedBody: public Stream {
public:
    String body;bool overflow=false;
    size_t write(uint8_t b) override {return write(&b,1);}
    size_t write(const uint8_t* b,size_t n) override {
        if(body.length()+n>48000){overflow=true;return 0;}
        body.concat((const char*)b,n);return n;
    }
    int available() override {return 0;} int read() override {return -1;}
    int peek() override {return -1;} void flush() override {}
};
void request(const char* name,const String &url,const String &payload,
             const String &access,const String &account,JsonObject out,
             String* returned=nullptr,bool form=false,bool shortRetry=false) {
    VerifiedTLSClient tls;tls.setCACert(NATIVE_ROOTS);tls.setHandshakeTimeout(shortRetry?10:20);
    HTTPClient http;http.setConnectTimeout(shortRetry?7000:15000);http.setTimeout(shortRetry?8000:15000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setReuse(false);http.setUserAgent("codex-cli");
    const char* headers[]={"Content-Type","cf-mitigated","Retry-After"};
    http.begin(tls,url);http.collectHeaders(headers,3);
    http.addHeader("Accept","application/json");
    if(access.length()){http.addHeader("Authorization","Bearer "+access);http.addHeader("ChatGPT-Account-Id",account);}
    uint32_t start=millis();int status;
    if(payload.length()){http.addHeader("Content-Type",form?"application/x-www-form-urlencoded":"application/json");status=http.POST(payload);}
    else status=http.GET();
    out["endpoint"]=name;out["http"]=status;out["elapsedMs"]=millis()-start;
    char tlsMessage[160]={};int tlsError=status<0?tls.lastError(tlsMessage,sizeof(tlsMessage)):0;
    out["tlsError"]=tlsError;out["verificationFlags"]=tls.verificationFlags;
    if(status<0){out["httpError"]=HTTPClient::errorToString(status);if(tlsError)out["tlsMessage"]=tlsMessage;}
    out["tlsVerified"]=status>0;out["contentType"]=http.header("Content-Type");
    out["challenge"]=http.header("cf-mitigated")=="challenge";
    if(http.header("Retry-After").length())out["retryAfter"]=http.header("Retry-After").toInt();
    BoundedBody sink;
    int read = status>0 && http.getSize()<=48000 ? http.writeToStream(&sink) : -1;
    bool complete = read>=0 && !sink.overflow && (size_t)read==sink.body.length();
    out["bytes"]=sink.body.length();out["overflow"]=sink.overflow;
    if(!complete && status>0)out["bodyReadError"]=read;
    if(returned)*returned=complete?sink.body:String();
    JsonDocument parsed;
    if(complete && !deserializeJson(parsed,sink.body)) {
        JsonArray keys=out["keys"].to<JsonArray>();
        for(JsonPair item:parsed.as<JsonObject>())keys.add(item.key().c_str());
        // Diagnostic quota payloads contain no OAuth credentials. Never expose auth bodies.
        if(access.length() && status==200)out["data"]=parsed;
    }
    http.end();
}
String encode(const String &s) {
    String out;char hex[4];for(size_t i=0;i<s.length();i++) {
        unsigned char c=s[i];if(isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')out+=(char)c;
        else {snprintf(hex,sizeof(hex),"%%%02X",c);out+=hex;}
    }return out;
}
bool jwt(const String &token,JsonDocument &out) {
    int a=token.indexOf('.'),b=token.indexOf('.',a+1);if(a<0||b<=a)return false;
    String segment=token.substring(a+1,b);segment.replace('-','+');segment.replace('_','/');
    while(segment.length()%4)segment+='=';
    unsigned char buffer[8192];size_t size=0;
    if(mbedtls_base64_decode(buffer,sizeof(buffer),&size,(const unsigned char*)segment.c_str(),segment.length()))return false;
    return !deserializeJson(out,buffer,size);
}
bool saveSession(int slot,JsonDocument &session) {
    String data;serializeJson(session,data);String key="account"+String(slot);
    Preferences store;if(!storageReady||!store.begin("oa-native",false,"accounts"))return false;
    bool ok=store.putString(key.c_str(),data)==data.length() && store.getString(key.c_str(),"")==data;
    store.end();return ok;
}
bool loadSession(int slot,JsonDocument &session) {
    Preferences store;if(!storageReady||!store.begin("oa-native",true,"accounts"))return false;
    String data=store.getString(("account"+String(slot)).c_str(),"");store.end();
    return !deserializeJson(session,data) && session["refresh"].is<const char*>();
}
bool acceptTokens(const String &body,const String &expected,JsonDocument &session) {
    JsonDocument response,claims;
    if(deserializeJson(response,body) || !response["access_token"].is<const char*>())return false;
    String access=response["access_token"];
    if(!jwt(access,claims))return false;
    String account=claims["https://api.openai.com/auth"]["chatgpt_account_id"]|"";
    if(account.length()!=36 || (expected.length()&&account!=expected) || claims["exp"].as<int64_t>()<=time(nullptr))return false;
    session["access"]=access;session["account"]=account;session["expires"]=claims["exp"].as<int64_t>();
    session["issued"]=time(nullptr);
    session.remove("earliestRefresh");
    if(response["earliest_refresh_at"].is<int64_t>())session["earliestRefresh"]=response["earliest_refresh_at"];
    if(response["refresh_token"].is<const char*>())session["refresh"]=response["refresh_token"];
    return String(session["refresh"]|"").length()>0;
}
void accountReads(JsonDocument &session,JsonArray requests) {
    String token=session["access"]|"",account=session["account"]|"";
    bool retried=false;
    auto read=[&](const char* name,const char* url){
        auto response=requests.add<JsonObject>();request(name,url,"",token,account,response);
        int http=response["http"]|0;
        int failure=http==200?(response["bodyReadError"]|0):http;
        if(retried||WiFi.status()!=WL_CONNECTED||!recovery::retryQuotaRead(failure,response["tlsError"]|0))return;
        retried=true;
        response.clear();request(name,url,"",token,account,response,nullptr,false,true);
        response["firstError"]=failure;
    };
    read("usage","https://chatgpt.com/backend-api/wham/usage");
    read("reset-credits","https://chatgpt.com/backend-api/wham/rate-limit-reset-credits");
}
String base64url(const uint8_t* bytes,size_t length) {
    unsigned char encoded[128];size_t n=0;
    if(mbedtls_base64_encode(encoded,sizeof(encoded),&n,bytes,length))return "";
    String result((char*)encoded,n);result.replace('+','-');result.replace('/','_');result.replace("=","");return result;
}
void browserStart(const String &input,JsonDocument &report) {
    auto& flow=browserFlows[wifiJobActive];
    JsonDocument args;if(deserializeJson(args,input)||!args["slot"].is<int>()||args["slot"].as<int>()<0||args["slot"].as<int>()>1||(String(args["expected"]|"").length()!=0&&String(args["expected"]|"").length()!=36)){report["error"]="invalid_login_input";return;}
    flow=BrowserFlow{};
    JsonDocument prior;
    flow.expected=args["expected"]|"";
    int slot=args["slot"];
    if(loadSession(slot,prior)) {
        String paired=prior["account"]|"";
        if((wifiJobActive&&paired.length()!=36)||(flow.expected.length()&&paired!=flow.expected)){
            report["error"]="existing_account_mismatch";return;
        }
        flow.expected=paired;
    }else if(wifiJobActive){
        report["error"]="initial_pairing_requires_usb";return;
    }
    uint8_t random[32],digest[32];esp_fill_random(random,sizeof(random));flow.verifier=base64url(random,sizeof(random));
    esp_fill_random(random,sizeof(random));flow.state=base64url(random,sizeof(random));
    mbedtls_sha256_ret((const unsigned char*)flow.verifier.c_str(),flow.verifier.length(),digest,0);
    flow.slot=slot;flow.started=millis();
    String url="https://auth.openai.com/oauth/authorize?response_type=code&client_id="+encode(CLIENT_ID)+
      "&redirect_uri="+encode("http://127.0.0.1:1455/auth/callback")+"&scope="+encode("openid profile email offline_access")+
      "&code_challenge="+base64url(digest,sizeof(digest))+"&code_challenge_method=S256&state="+flow.state+
      "&id_token_add_organizations=true&codex_cli_simplified_flow=true&originator=codex_cli_rs";
    report["authorizationUrl"]=url;report["stateToken"]=flow.state;report["slot"]=flow.slot;
    report["browserPending"]=true;
}
void browserComplete(const String &input,JsonDocument &report) {
    auto& flow=browserFlows[wifiJobActive];
    JsonDocument args;
    if(deserializeJson(args,input)||flow.slot<0||millis()-flow.started>900000||args["state"].as<String>()!=flow.state||!args["code"].is<const char*>()){report["error"]="invalid_or_expired_browser_callback";return;}
    String encoded="grant_type=authorization_code&client_id="+encode(CLIENT_ID)+"&code="+encode(args["code"].as<String>())+
        "&redirect_uri="+encode("http://127.0.0.1:1455/auth/callback")+"&code_verifier="+encode(flow.verifier);
    int slot=flow.slot;String expected=flow.expected;flow=BrowserFlow{};
    if(wifiJobActive){
        JsonDocument prior;
        if(!loadSession(slot,prior)||prior["account"].as<String>()!=expected){report["error"]="existing_account_mismatch";return;}
    }
    String body;JsonArray requests=report["requests"].as<JsonArray>();JsonObject exchange=requests.add<JsonObject>();
    request("browser-code-exchange","https://auth.openai.com/oauth/token",encoded,"","",exchange,&body,true);
    JsonDocument session;
    if(exchange["http"].as<int>()!=200||!acceptTokens(body,expected,session)){report["error"]="exchange_or_account_mismatch";return;}
    JsonDocument other;if(loadSession(1-slot,other)&&other["account"].as<String>()==session["account"].as<String>()){report["error"]="account_already_paired_in_other_slot";return;}
    if(!saveSession(slot,session)){report["error"]="session_storage_failed";return;}
    cached[slot].clear();nextPoll=millis();consecutiveFailures=0;
    report["account"]=session["account"];
    report["slot"]=slot;report["authenticated"]=true;report["persisted"]=true;report["expires"]=session["expires"];
    if(wifiJobActive)report["polls"]=pollCount;
    else accountReads(session,requests);
}
void login(const String &input,JsonDocument &report) {
    JsonDocument args;
    if(deserializeJson(args,input)||!args["slot"].is<int>()||args["slot"].as<int>()<0||args["slot"].as<int>()>1||(String(args["expected"]|"").length()!=0&&String(args["expected"]|"").length()!=36)){report["error"]="invalid_login_input";return;}
    int slot=args["slot"];String expected=args["expected"];
    JsonDocument payload;payload["client_id"]=CLIENT_ID;String encoded,body;serializeJson(payload,encoded);
    JsonArray requests=report["requests"].as<JsonArray>();
    JsonObject first=requests.add<JsonObject>();
    request("device-code","https://auth.openai.com/api/accounts/deviceauth/usercode",encoded,"","",first,&body);
    JsonDocument code;
    if(first["http"].as<int>()!=200 || deserializeJson(code,body) || !code["device_auth_id"].is<const char*>()) {report["error"]="device_code_failed";return;}
    String userCode=code["user_code"]|code["usercode"]|"";
    if(!userCode.length()){report["error"]="missing_user_code";return;}
    uint32_t interval=max(5L,String(code["interval"]|"5").toInt());
    report["state"]="awaiting_authorization";report["slot"]=slot;report["userCode"]=userCode;
    report["verificationUrl"]="https://auth.openai.com/codex/device";publish(report);
    payload.clear();payload["device_auth_id"]=code["device_auth_id"];payload["user_code"]=userCode;
    encoded="";serializeJson(payload,encoded);
    uint32_t started=millis();JsonDocument poll;bool authorized=false;
    while(!cancelled && millis()-started<900000) {
        for(uint32_t i=0;i<interval*10 && !cancelled;i++)vTaskDelay(pdMS_TO_TICKS(100));
        if(cancelled)break;
        JsonDocument metadata;
        request("device-poll","https://auth.openai.com/api/accounts/deviceauth/token",encoded,"","",metadata.to<JsonObject>(),&body);
        int status=metadata["http"];
        if(status==200 && !deserializeJson(poll,body)){authorized=true;break;}
        if((status==403||status==404) && !metadata["challenge"].as<bool>())continue;
        requests.add<JsonObject>().set(metadata.as<JsonObjectConst>());report["error"]="device_poll_failed";return;
    }
    report.remove("userCode");
    if(!authorized){report["error"]=cancelled?"cancelled":"authorization_timeout";return;}
    if(!poll["authorization_code"].is<const char*>()||!poll["code_verifier"].is<const char*>()){report["error"]="invalid_authorization_response";return;}
    encoded="grant_type=authorization_code&client_id="+encode(CLIENT_ID)+"&code="+encode(poll["authorization_code"].as<String>())+
        "&redirect_uri="+encode("https://auth.openai.com/deviceauth/callback")+"&code_verifier="+encode(poll["code_verifier"].as<String>());
    JsonObject exchange=requests.add<JsonObject>();
    // Single attempt: an ambiguous timeout must not replay a consumed authorization code.
    request("code-exchange","https://auth.openai.com/oauth/token",encoded,"","",exchange,&body,true);
    JsonDocument session;
    if(exchange["http"].as<int>()!=200 || !acceptTokens(body,expected,session)){report["error"]="exchange_or_account_mismatch";return;}
    JsonDocument other;if(loadSession(1-slot,other)&&other["account"].as<String>()==session["account"].as<String>()){report["error"]="account_already_paired_in_other_slot";return;}
    if(!saveSession(slot,session)){report["error"]="session_storage_failed";return;}
    cached[slot].clear();nextPoll=millis();consecutiveFailures=0;
    report["account"]=session["account"];
    report["authenticated"]=true;report["persisted"]=true;report["expires"]=session["expires"];
    accountReads(session,requests);
}
void refreshSession(int slot,JsonDocument &session,JsonDocument &report,bool performRefresh,bool allowAccessFallback=false) {
    JsonArray requests=report["requests"].as<JsonArray>();
    report["slot"]=slot;report["expires"]=session["expires"];
    if(session["refreshPending"]==true){
        report["reauthRequired"]=true;
        report["error"]=session["refreshFailure"]|"refresh_outcome_unknown";
        report["refreshHttp"]=session["refreshHttp"];
        return;
    }
    if(performRefresh) {
        JsonDocument payload;payload["grant_type"]="refresh_token";payload["client_id"]=CLIENT_ID;payload["refresh_token"]=session["refresh"];
        String encoded,body;serializeJson(payload,encoded);
        // Persist before rotation. Power loss or an ambiguous result requires a fresh login.
        session["refreshPending"]=true;
        if(!saveSession(slot,session)){report["error"]="storage_failed_before_refresh";return;}
        JsonObject metadata=requests.add<JsonObject>();
        request("token-refresh","https://auth.openai.com/oauth/token",encoded,"","",metadata,&body);
        int http=metadata["http"]|0;
        JsonDocument replacement;replacement.set(session);
        if(http!=200 || !acceptTokens(body,session["account"].as<String>(),replacement)){
            if(recovery::refreshDefinitelyUnsent(http)){
                session.remove("refreshPending");
                if(!saveSession(slot,session)){report["error"]="refresh_state_storage_failed";return;}
                report["refreshFailure"]="refresh_connection_failed";
                if(!allowAccessFallback || session["expires"].as<int64_t>()<=time(nullptr)){
                    report["error"]="refresh_connection_failed";return;
                }
            }else{
                JsonDocument error;
                deserializeJson(error,body);
                const char* code=error["error"]["code"]|(error["error"]|"");
                const char* reason=recovery::refreshFailure(http,code);
                session["refreshFailure"]=reason;session["refreshHttp"]=http;
                saveSession(slot,session); // The pending marker is already durable.
                report["error"]=reason;report["refreshHttp"]=http;report["reauthRequired"]=true;return;
            }
        }else{
            replacement.remove("refreshPending");
            replacement.remove("refreshFailure");replacement.remove("refreshHttp");
            if(!saveSession(slot,replacement)){report["error"]="refreshed_session_storage_failed";return;}
            session.set(replacement);report["refreshed"]=true;
        }
    }
    report["expires"]=session["expires"];accountReads(session,requests);
}
void refresh(int slot,JsonDocument &report,bool performRefresh) {
    JsonDocument session;
    if(slot<0||slot>1||!loadSession(slot,session)){
        report["error"]=storageReady?"no_device_session":"account_storage_unavailable";
        report["reauthRequired"]=storageReady;return;
    }
    refreshSession(slot,session,report,performRefresh);
}
std::atomic<bool> timeSynchronized{false};
bool clockReady(){
    static bool started=false;
    if(!started){
        sntp_set_time_sync_notification_cb([](struct timeval*){timeSynchronized=true;});
        configTime(0,0,"time.cloudflare.com","pool.ntp.org","time.google.com");
        setenv("TZ",nativeTimezone().c_str(),1);tzset();started=true;
    }
    if(timeSynchronized&&time(nullptr)>=1760000000)return true;
    nativeConnectionState("SYNCING CLOCK");
    uint32_t start=millis();
    while((!timeSynchronized||time(nullptr)<1760000000)&&millis()-start<25000)vTaskDelay(pdMS_TO_TICKS(250));
    return timeSynchronized&&time(nullptr)>=1760000000;
}
String accountError(JsonDocument& report){
    String error=report["error"]|"";
    if(report["reauthRequired"]==true)return "SIGN IN AGAIN";
    if(error.indexOf("storage")>=0)return "ACCOUNT STORAGE ERROR";
    for(JsonObject r:report["requests"].as<JsonArray>()){
        int http=r["http"]|0;
        if(http==401)return "ACCOUNT ACCESS DENIED";
        if(http!=200)return recovery::requestError(http,r["tlsError"]|0);
    }
    return "INVALID SERVER DATA";
}
void pollAccounts(){
    if(WiFi.status()!=WL_CONNECTED){nativeConnectionState("CONNECTING WI-FI");nextPoll=millis()+1000;return;}
    if(!clockReady()){
        JsonDocument status;status["state"]="clock_unavailable";status["retrySeconds"]=10;publish(status);
        nextPoll=millis()+10000;return;
    }
    uint32_t pollStarted=millis();
    // Keep the last complete per-account diagnostics visible during network I/O.
    JsonDocument status;status["native"]=true;status["epoch"]=(int64_t)time(nullptr);
    bool all=true,any=false;uint32_t retryAfter=0;
    for(int slot=0;slot<2;slot++){
        JsonDocument session,report;report["requests"].to<JsonArray>();
        bool loaded=loadSession(slot,session);
        int64_t now=time(nullptr);
        bool renew=loaded&&recovery::refreshDue(now,session["expires"].as<int64_t>(),session["earliestRefresh"].as<int64_t>());
        if(loaded)refreshSession(slot,session,report,renew,true);
        else {report["error"]=storageReady?"no_device_session":"account_storage_unavailable";report["reauthRequired"]=storageReady;}
        bool unauthorized=false;
        for(JsonObject r:report["requests"].as<JsonArray>())if(r["http"]==401&&r["endpoint"]=="usage")unauthorized=true;
        if(unauthorized&&!renew&&loaded&&recovery::retryUnauthorized(now,session["issued"].as<int64_t>(),session["earliestRefresh"].as<int64_t>())){
            report.clear();report["requests"].to<JsonArray>();refreshSession(slot,session,report,true);
        }
        for(JsonObject r:report["requests"].as<JsonArray>())retryAfter=std::max(retryAfter,std::min(3600u,r["retryAfter"]|0u));
        JsonObject usage,credits;
        for(JsonObject r:report["requests"].as<JsonArray>()){
            if(r["http"].as<int>()!=200)continue;
            if(r["endpoint"].as<String>()=="usage")usage=r["data"].as<JsonObject>();
            if(r["endpoint"].as<String>()=="reset-credits")credits=r["data"].as<JsonObject>();
        }
        bool valid=report["error"].isNull()&&!usage.isNull()&&!credits.isNull()&&credits["credits"].is<JsonArray>()&&credits["available_count"].is<int>()&&loaded&&usage["account_id"].as<String>()==session["account"].as<String>();
        if(valid){any=true;cached[slot].clear();nativeDataUpdate(slot,session["account"].as<String>(),usage,credits,cached[slot].to<JsonObject>(),time(nullptr));}
        else {all=false;cached[slot]["label"]=slot==0?"PRIMARY":"SECONDARY";cached[slot]["error"]=accountError(report);}
        bool reauth=report["reauthRequired"]|false;
        cached[slot]["reauthRequired"]=reauth;
        JsonObject summary=status["accounts"][slot].to<JsonObject>();summary["ok"]=valid;
        summary["error"]=cached[slot]["error"];summary["expires"]=report["expires"];
        summary["reauthRequired"]=reauth;
        if(!report["error"].isNull())summary["reason"]=report["error"];
        if(!report["refreshFailure"].isNull())summary["reason"]=report["refreshFailure"];
        if(!report["refreshHttp"].isNull())summary["refreshHttp"]=report["refreshHttp"];
        JsonArray requests=summary["requests"].to<JsonArray>();
        for(JsonObject r:report["requests"].as<JsonArray>()){
            auto item=requests.add<JsonObject>();
            for(const char* key:{"endpoint","http","elapsedMs","tlsError","tlsMessage","httpError","retryAfter","verificationFlags","bodyReadError","firstError"})if(!r[key].isNull())item[key]=r[key];
        }
    }
    wifiUpdateAccountHealth(all);
    pollCount++;if(!all){pollFailures++;consecutiveFailures++;}else consecutiveFailures=0;
    uint32_t waitMs=any&&!retryAfter?30000:recovery::retryMs(consecutiveFailures,retryAfter);
    nextPoll=any&&millis()-pollStarted<waitMs?pollStarted+waitMs:millis()+(all?1000:waitMs);
    JsonDocument data;data["version"]=1;data["source"]="esp32";data["now"]=(int64_t)time(nullptr);
    data["connection"]=all?"LIVE":any?"PARTIAL DATA":"RETRYING";
    if(!all)data["retryAt"]=(int64_t)time(nullptr)+waitMs/1000;
    JsonArray accounts=data["accounts"].to<JsonArray>();for(auto &a:cached)accounts.add(a.as<JsonObject>());
    nativeDataEvents(data["alerts"].to<JsonArray>(),time(nullptr));String value;serializeJson(data,value);nativePublishPacket(value);
    status["state"]="complete";status["polls"]=pollCount;status["failedPolls"]=pollFailures;
    status["historySamples"]=nativeHistoryCount();status["nextPollSeconds"]=waitMs/1000;status["heap"]=ESP.getFreeHeap();status["psramFree"]=ESP.getFreePsram();status["stackFree"]=uxTaskGetStackHighWaterMark(nullptr);publish(status);
}
void worker(void*) {
    bool wasOnline=false;
    for(;;) {
        if(updatePause.load()){vTaskDelay(pdMS_TO_TICKS(100));continue;}
        // Wait outside the activity lock. Leave a queued command in place if a
        // restart or update starts while the worker is waiting for the lock.
        String* command=nullptr;
        xQueuePeek(jobs,&command,pdMS_TO_TICKS(1000));
        ActivityGuard activity;
        if(updatePause.load())continue;
        bool online=WiFi.status()==WL_CONNECTED;
        if(online&&!wasOnline){nextPoll=millis();consecutiveFailures=0;}
        wasOnline=online;
        if(xQueueReceive(jobs,&command,0)!=pdTRUE){
            if(enabled&&(int32_t)(millis()-nextPoll)>=0){nextPoll=millis()+30000;pollAccounts();}
            continue;
        }
        wifiJobActive=command->startsWith("WIFI ");
        if(wifiJobActive)command->remove(0,5);
        inCommand=true;cancelled=false;JsonDocument report;report["state"]="running";publish(report);
        bool local=command->startsWith("FORGET ")||*command=="RECONNECT";
        if(!local&&WiFi.status()!=WL_CONNECTED){report["state"]="wifi_unavailable";publish(report);inCommand=false;delete command;continue;}
        bool synchronized=!local&&clockReady();
        report["clockReady"]=synchronized;report["epoch"]=(int64_t)time(nullptr);
        if(!local&&!report["clockReady"].as<bool>()){report["state"]="clock_unavailable";publish(report);inCommand=false;delete command;continue;}
        JsonArray requests=report["requests"].to<JsonArray>();
        if(command->startsWith("FORGET ")) {
            JsonDocument input;
            if(deserializeJson(input,command->substring(7))||input["confirm"].as<String>()!="FORGET_ACCOUNT"||!input["slot"].is<int>()||input["slot"].as<int>()<0||input["slot"].as<int>()>1)report["error"]="invalid_forget_request";
            else{
                int slot=input["slot"];Preferences p;enabled=false;cached[slot].clear();
                if(!p.begin("oa-native",false,"accounts"))report["error"]="forget_storage_failed";
                else {
                    String accountKey="account"+String(slot),priorKey="prior"+String(slot);
                    p.remove(accountKey.c_str());p.remove(priorKey.c_str());
                    bool ok=p.putBool("enabled",false)==1&&!p.isKey(accountKey.c_str())&&!p.isKey(priorKey.c_str());p.end();
                    if(ok)report["forgotten"]=slot;else report["error"]="forget_storage_failed";
                }
            }
        } else if(command->startsWith("ALERTTEST ")) {
            JsonDocument cases;if(deserializeJson(cases,command->substring(10))||!cases.is<JsonArray>())report["error"]="invalid_test_cases";
            else nativeDataAlertTest(cases.as<JsonArray>(),report["results"].to<JsonArray>());
        } else if(command->startsWith("MODE ")) {
            bool value=command->substring(5)=="1";JsonDocument a,b;
            if(value&&(!loadSession(0,a)||!loadSession(1,b)||!nativeDataBegin()))report["error"]="native_not_ready";
            else {Preferences p;p.begin("oa-native",false,"accounts");bool ok=p.putBool("enabled",value)==1;p.end();if(ok){enabled=value;nextPoll=millis();}else report["error"]="mode_storage_failed";report["native"]=enabled.load();}
        } else if(command->startsWith("SEED ")||command->startsWith("BASELINE ")) {
            bool seed=command->startsWith("SEED ");JsonDocument input,session;
            if(deserializeJson(input,command->substring(seed?5:9))||!input["slot"].is<int>()||input["slot"].as<int>()<0||input["slot"].as<int>()>1||!loadSession(input["slot"],session))report["error"]="invalid_import";
            else {bool ok=seed?nativeDataSeed(session["account"].as<String>(),input.as<JsonObject>()):nativeDataBaseline(input["slot"],session["account"].as<String>(),input["prior"].as<JsonObject>());report["imported"]=ok;if(!ok)report["error"]="import_failed";}
        } else if(*command=="TLSCHECK") {
            request("valid-google-ca","https://chatgpt.com/backend-api/wham/usage","","","",requests.add<JsonObject>());
            request("valid-auth","https://auth.openai.com/.well-known/openid-configuration","","","",requests.add<JsonObject>());
            request("expired-certificate","https://expired.badssl.com/","","","",requests.add<JsonObject>());
            request("wrong-hostname","https://wrong.host.badssl.com/","","","",requests.add<JsonObject>());
            request("untrusted-certificate","https://self-signed.badssl.com/","","","",requests.add<JsonObject>());
        } else if(*command=="RECONNECT") {
            WiFi.disconnect(false,false);report["reconnecting"]=true;
        } else if(*command=="PROBE") {
            request("auth-discovery","https://auth.openai.com/.well-known/openid-configuration","","","",requests.add<JsonObject>());
            request("usage-unauthenticated","https://chatgpt.com/backend-api/wham/usage","","","",requests.add<JsonObject>());
        } else if(command->startsWith("BROWSER ")) {
            browserStart(command->substring(8),report);
        } else if(command->startsWith("CALLBACK ")) {
            browserComplete(command->substring(9),report);
        } else if(command->startsWith("AUTH ")) {
            login(command->substring(5),report);
        } else if(command->startsWith("REFRESH ")) {
            refresh(command->substring(8).toInt(),report,true);
        } else if(command->startsWith("READ ")) {
            refresh(command->substring(5).toInt(),report,false);
        } else if(command->startsWith("ACCESS ")) {
            JsonDocument input;
            if(!deserializeJson(input,command->substring(7)) && input["token"].is<const char*>() && input["account"].is<const char*>()) {
                String token=input["token"],account=input["account"];
                request("usage","https://chatgpt.com/backend-api/wham/usage","",token,account,requests.add<JsonObject>());
                request("reset-credits","https://chatgpt.com/backend-api/wham/rate-limit-reset-credits","",token,account,requests.add<JsonObject>());
            } else report["error"]="invalid_input";
        }
        report["state"]="complete";report["heap"]=ESP.getFreeHeap();
        report["stackFree"]=uxTaskGetStackHighWaterMark(nullptr);publish(report);inCommand=false;delete command;
    }
}
}
void nativeClientBegin() {
    // This board has 8 MiB PSRAM. Keep the full trusted-root chain and TLS
    // workspace there instead of exhausting the small internal/DMA heap.
    // The standard mbedTLS verifier handles cross-signed roots correctly.
    mbedtls_platform_set_calloc_free(
        [](size_t n,size_t bytes)->void*{return heap_caps_calloc(n,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);},
        heap_caps_free);
    updateLock=xSemaphoreCreateMutex();
    lock=xSemaphoreCreateMutex();jobs=xQueueCreate(1,sizeof(String*));
    esp_err_t err=nvs_flash_init_partition("accounts");
    // Never erase account sessions or OTA identity to recover an NVS error.
    // A failed upgrade must leave the previous firmware's data recoverable.
    storageReady=err==ESP_OK;
    // Migrate only the independent sessions created by the earlier hardware test.
    // Verify both copies before releasing their old settings-space allocation.
    Preferences old,destination;
    if(storageReady&&old.begin("oa-native",false)&&destination.begin("oa-native",false,"accounts")){
        bool migrated=true;
        for(const char* key:{"account0","account1"}){
            String value=old.getString(key,"");if(value.isEmpty())continue;
            if(destination.getString(key,"").isEmpty())migrated&=destination.putString(key,value)==value.length()&&destination.getString(key,"")==value;
        }
        if(migrated){old.remove("account0");old.remove("account1");}
        old.end();destination.end();
    }
    Preferences p;if(storageReady&&p.begin("oa-native",true,"accounts")){enabled=p.getBool("enabled",false);p.end();}
    if(enabled&&!nativeDataBegin())enabled=false;
    xTaskCreatePinnedToCore(worker,"native-test",16384,nullptr,1,nullptr,0);
}
bool nativeEnabled(){return enabled;}
String nativeWifiReauth(const String &body){
    JsonDocument input,params;
    if(deserializeJson(input,body))return "{\"error\":\"invalid_request\"}";
    String id=input["id"]|"",action=input["action"]|"",command;
    if(id.length()!=32 || id.length()!=strspn(id.c_str(),"0123456789abcdef"))return "{\"error\":\"invalid_request\"}";
    if(action=="start"&&input["slot"].is<int>()){
        command="WIFI BROWSER ";params["slot"]=input["slot"];
    }else if(action=="finish"){
        String code=input["code"]|"",state=input["state"]|"";
        if(code.length()==0||code.length()>1024||state.length()!=43)return "{\"error\":\"invalid_request\"}";
        command="WIFI CALLBACK ";params["code"]=code;params["state"]=state;
    }else if(action!="result")return "{\"error\":\"invalid_request\"}";
    if(command.length()){
        String payload;serializeJson(params,payload);command+=payload;
    }
    xSemaphoreTake(lock,portMAX_DELAY);
    String response;
    if(action=="result")response=id==wifiRequestId?wifiResult:"{\"error\":\"unknown_request\"}";
    else if(!enabled)response="{\"error\":\"initial_pairing_requires_usb\"}";
    else if(wifiJobPending)response="{\"error\":\"account_request_busy\"}";
    else{
        String* job=new String(command);
        if(xQueueSend(jobs,&job,0)==pdTRUE){
            wifiRequestId=id;wifiJobPending=true;
            wifiResult="{\"state\":\"queued\"}";response=wifiResult;
        }else{delete job;response="{\"error\":\"account_request_busy\"}";}
    }
    xSemaphoreGive(lock);
    return response;
}
String nativeClientCommand(const String &command) {
    if(command=="RESULT") {xSemaphoreTake(lock,portMAX_DELAY);String copy=commandResult;xSemaphoreGive(lock);return copy;}
    if(command=="STATUS") {xSemaphoreTake(lock,portMAX_DELAY);String copy=result;xSemaphoreGive(lock);return copy;}
    if(command=="CANCEL"){cancelled=true;return "OK NATIVE CANCEL";}
    if(command!="PROBE" && command!="TLSCHECK" && command!="RECONNECT" && !command.startsWith("ACCESS ") && !command.startsWith("AUTH ") && !command.startsWith("BROWSER ") && !command.startsWith("CALLBACK ") && !command.startsWith("REFRESH ") && !command.startsWith("READ ") && !command.startsWith("MODE ") && !command.startsWith("SEED ") && !command.startsWith("BASELINE ") && !command.startsWith("ALERTTEST ") && !command.startsWith("FORGET "))return "ERROR NATIVE COMMAND";
    String* job=new String(command);
    if(uxQueueSpacesAvailable(jobs)==0){delete job;return "ERROR NATIVE BUSY";}
    xSemaphoreTake(lock,portMAX_DELAY);commandResult="{\"state\":\"queued\"}";xSemaphoreGive(lock);
    if(xQueueSend(jobs,&job,0)!=pdTRUE){delete job;return "ERROR NATIVE BUSY";}
    return "OK NATIVE QUEUED";
}

bool nativePauseForUpdate(uint32_t timeoutMs){
    bool expected=false;
    if(!updatePause.compare_exchange_strong(expected,true))return false;
    bool acquired=updateLock&&xSemaphoreTake(updateLock,pdMS_TO_TICKS(timeoutMs))==pdTRUE;
    updateLockHeld=acquired;if(!acquired)updatePause=false;return acquired;
}
void nativeResumeAfterUpdate(){
    if(updateLockHeld.exchange(false))xSemaphoreGive(updateLock);
    updatePause=false;
}
