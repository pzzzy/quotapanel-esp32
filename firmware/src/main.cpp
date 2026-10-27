// SPDX-License-Identifier: GPL-3.0-only
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Preferences.h>
#include <Wire.h>
#include "board_display.h"
#include "alerts_audio.h"
#include "native_client.h"

XiaozhiDisplay lcd;
lgfx::LGFX_Sprite canvas(&lcd);
Preferences prefs;
SemaphoreHandle_t stateLock;
String ssid, password, displayTimezone="UTC0";
String packet, transport, networkState = "STARTING";
uint32_t packetAt = 0;
bool configured = false;
int detailPage = -1, detailWindow = 0;
uint64_t lastAlert=0;
String alertBanner;uint32_t bannerUntil=0;
uint8_t* capturedFrame=nullptr;
uint32_t nextDraw = 0;
const uint32_t BG=0x080F1B, CARD=0x142235, WHITE=0xF2F7FF, MUTED=0x8DA3BB;
const uint32_t MINT=0x5DE4BE, BLUE=0x79B8FF, AMBER=0xFFBF69, RED=0xFF7185;

String nativeTimezone(){return displayTimezone;}
void setNetwork(const String &s) {
    xSemaphoreTake(stateLock, portMAX_DELAY); networkState=s; xSemaphoreGive(stateLock);
}
void nativePublishPacket(const String& value){
    xSemaphoreTake(stateLock,portMAX_DELAY);packet=value;packetAt=millis();networkState="LIVE";transport="DIRECT";xSemaphoreGive(stateLock);
}
void networkTask(void*) {
    WiFi.mode(WIFI_STA);WiFi.setSleep(false);WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(),password.c_str());uint32_t lastJoin=millis();
    for(;;){
        if(WiFi.status()!=WL_CONNECTED){
            setNetwork("CONNECTING WI-FI");
            if(millis()-lastJoin>20000){WiFi.disconnect();WiFi.begin(ssid.c_str(),password.c_str());lastJoin=millis();}
        }else if(!nativeEnabled())setNetwork("PAIR ACCOUNTS OVER USB");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
void text(const String&s,int x,int y,uint32_t color=MUTED,int size=1) {
    canvas.setFont(&fonts::Font0);canvas.setTextSize(size);canvas.setTextColor(color);
    canvas.setTextDatum(TL_DATUM);canvas.drawString(s,x,y);
}
void right(const String&s,int x,int y,uint32_t color=MUTED) {
    canvas.setFont(&fonts::Font0);canvas.setTextSize(1);canvas.setTextColor(color);
    canvas.setTextDatum(TR_DATUM);canvas.drawString(s,x,y);canvas.setTextDatum(TL_DATUM);
}
void large(const String&s,int x,int y,uint32_t color=WHITE) {
    canvas.setFont(&fonts::FreeSansBold18pt7b);canvas.setTextSize(1);canvas.setTextColor(color);
    canvas.setTextDatum(TL_DATUM);canvas.drawString(s,x,y);
}
String countdown(int64_t reset,int64_t now) {
    if(reset<=0) return "UNAVAILABLE";
    int64_t d=reset-now;
    if(d<=0) return "REFRESHING";
    char b[32];
    if(d>=86400)snprintf(b,sizeof(b),"%lldd %02lldh %02lldm",d/86400,(d/3600)%24,(d/60)%60);
    else snprintf(b,sizeof(b),"%02lld:%02lld:%02lld",d/3600,(d/60)%60,d%60);
    return b;
}
String creditText(JsonVariant a) {
    if(a["unlimited"]==true)return "UNLIMITED";
    if(a["credits"].isNull())return "--";
    double val=String(a["credits"].as<const char*>()).toDouble();
    if(val<1000)return String(val, val==floor(val)?0:1);
    // Preserve actual balance to whole credits; never label it as dollars.
    String s=String((unsigned long)floor(val));
    for(int i=(int)s.length()-3;i>0;i-=3)s=s.substring(0,i)+","+s.substring(i);
    return s;
}
String bankText(JsonVariant a) { return a["banked"].isNull()?"--":String(a["banked"].as<int>()); }
uint32_t accent(int index){return index==0?MINT:BLUE;}
bool stale(JsonVariant a,int64_t now) {return a["updated"].isNull() || a["updated"].as<int64_t>()==0 || now-a["updated"].as<int64_t>()>90 || !a["error"].isNull();}
String dateText(int64_t timestamp,bool full=false) {
    if(timestamp<=0)return "UNKNOWN";
    time_t ts=timestamp;struct tm t;localtime_r(&ts,&t);char buffer[32];
    strftime(buffer,sizeof(buffer),full?"%b %d %Y %I:%M%p":"%b %d",&t);
    String value(buffer);value.toUpperCase();return value;
}
String durationText(double seconds) {
    if(seconds<300)return "<5m";
    long minutes=(long)(round(seconds/300.0)*5);
    if(minutes>99*1440)return ">99d";
    if(minutes>=1440)return String(minutes/1440)+"d "+String((minutes%1440)/60)+"h";
    return String(minutes/60)+"h "+String(minutes%60)+"m";
}
String runwayText(JsonVariant f,int64_t now,bool unavailable,bool showBeyondReset=false) {
    if(unavailable)return "STALE";
    String state=f["state"]|"learning";
    if(state=="exhausted")return "EXHAUSTED";
    if(state=="no_burn")return "NO RECENT BURN";
    if(state=="reset_pending")return "RESET DUE";
    if(state=="stale")return "STALE";
    if(state=="unavailable")return "UNKNOWN";
    if(f["seconds"].isNull())return "LEARNING";
    if(f["resetFirst"]==true && !showBeyondReset)return "RESET FIRST";
    double eta=f["seconds"].as<double>()-max(0LL,now-(f["asOf"]|now));
    if(eta<=0)return "RECHECKING";
    return "~"+durationText(eta);
}
void drawCard(JsonVariant a,int index,int y,int64_t now,bool connected) {
    uint32_t color=accent(index);bool unavailable=!connected||stale(a,now);
    canvas.fillRoundRect(10,y,300,96,9,CARD);
    canvas.fillRoundRect(10,y+10,3,75,1,color);
    text(a["label"]|"ACCOUNT",20,y+7,color);
    String plan=a["plan"]|"unknown";plan.toUpperCase();
    right(unavailable?"STALE":plan,300,y+7,unavailable?AMBER:MUTED);
    JsonVariant w=a["windows"][0], f=a["forecast"];
    bool known=!w["remaining"].isNull();
    int remaining=known?(int)round(w["remaining"].as<float>()):0;
    uint32_t level=!known?MUTED:remaining<=5?RED:remaining<=15?AMBER:color;
    large(known?String(remaining)+"%":"--",20,y+20,level);text("LEFT",105,y+39,MUTED);
    text("RESET IN",166,y+23,MUTED);text(countdown(w["reset"]|0LL,now),166,y+36,WHITE);
    canvas.fillRoundRect(20,y+52,280,3,1,0x2A3A4E);
    if(known&&remaining>0)canvas.fillRoundRect(20,y+52,max(3,remaining*280/100),3,1,level);
    text("RUNWAY "+runwayText(f,now,unavailable),20,y+61,unavailable?AMBER:WHITE);
    right(f["partial"]==true?"PARTIAL":String(f["confidence"]|"LOW")+" CONF",300,y+61,MUTED);
    text(creditText(a)+" CREDITS",20,y+74,MUTED);
    right(bankText(a)+" BANKED",300,y+74,a["banked"].as<int>()>0?color:MUTED);
    String rate=unavailable||f["rate"].isNull()?"AT RECENT PACE":"BURN "+String(f["rate"].as<double>(),1)+"%/H";
    text(rate,20,y+85,MUTED);
    String expiry=a["banked"]==0?"EXP --":a["bankExpires"].isNull()?"EXP UNKNOWN":String(a["bankExpiryComplete"]==false?"KNOWN ":"EXP ")+dateText(a["bankExpires"].as<int64_t>());
    right(expiry,300,y+85,MUTED);
}
void drawDetails(JsonVariant a,int64_t now,bool connected) {
    uint32_t color=accent(detailPage);bool unavailable=!connected||stale(a,now);
    text("< BACK",12,12,color);
    right(a["label"]|"ACCOUNT",308,12,color);text(audioMuted()?"SOUND OFF":"SOUND ON",115,12,audioMuted()?AMBER:MUTED);
    canvas.drawFastHLine(12,30,296,0x293C51);
    JsonArray windows=a["windows"].as<JsonArray>();
    if(detailWindow>=(int)windows.size())detailWindow=0;
    JsonVariant w=windows[detailWindow], f=w["forecast"];
    text(String(w["label"]|"WINDOW")+" REMAINING",14,42,MUTED);
    large(w["remaining"].isNull()?"--":String((int)round(w["remaining"].as<float>()))+"%",14,58,color);
    text("RESET IN",155,48,MUTED);text(countdown(w["reset"]|0LL,now),155,66,WHITE);
    if(w["reset"].as<int64_t>()>0){time_t ts=w["reset"].as<int64_t>();struct tm t;localtime_r(&ts,&t);char buffer[32];strftime(buffer,sizeof(buffer),"%a %b %d %I:%M%p",&t);text(buffer,155,82,MUTED);}
    canvas.fillRoundRect(10,100,300,115,8,CARD);
    text("CREDIT BALANCE",20,109,MUTED);right(creditText(a),298,109,WHITE);
    text("BANKED RESETS",20,125,MUTED);right(bankText(a),298,125,color);
    text(a["bankExpiryComplete"]==false?"KNOWN EXPIRY":"FIRST EXPIRY",20,141,MUTED);
    right(a["banked"]==0?"--":dateText(a["bankExpires"]|0LL,true),298,141,WHITE);
    text("EXPIRES IN",20,154,MUTED);right(a["bankExpires"].isNull()?"--":countdown(a["bankExpires"].as<int64_t>(),now),298,154,MUTED);
    canvas.drawFastHLine(20,168,280,0x293C51);
    text("RUNWAY (EST)",20,177,MUTED);right(runwayText(f,now,unavailable),298,177,WHITE);
    String range="WAITING FOR HISTORY";
    if(f["state"]=="no_burn")range="NO MEASURABLE RECENT BURN";
    if(!f["lowSeconds"].isNull()) {
        double age=max(0LL,now-(f["asOf"]|now));
        range=durationText(max(0.0,f["lowSeconds"].as<double>()-age))+" - ";
        range+=f["highSeconds"].isNull()?"UNCERTAIN":durationText(max(0.0,f["highSeconds"].as<double>()-age));
    }
    if(f["resetFirst"]==true)range="AT PACE: "+runwayText(f,now,unavailable,true);
    if(unavailable)range="WAITING FOR FRESH DATA";
    text("RANGE",20,194,MUTED);right(range,298,194,MUTED);
    String note=unavailable?"STALE - CHECK MAC / SIGN-IN":f["resetPossible"]==true?"RESET MAY ARRIVE BEFORE EXHAUSTION":f["rate"].isNull()?"LEARNING FROM MEASURED QUOTA USE":String(f["confidence"]|"LOW")+" CONF / "+String(f["rate"].as<double>(),1)+"%/H / "+durationText(f["coverageSeconds"]|0)+" DATA";
    text(note,12,222,unavailable?AMBER:MUTED);
    if(windows.size()>1)text("TAP RIGHT: NEXT WINDOW",12,233,MUTED);
}
void speakerIcon() {
    uint32_t color=!audioReady()?RED:audioMuted()?AMBER:MUTED;
    canvas.fillRect(296,13,4,7,color);canvas.fillTriangle(300,13,305,9,305,24,color);
    if(audioMuted())canvas.drawLine(295,8,310,24,color);
    else {canvas.drawLine(308,12,310,15,color);canvas.drawLine(310,15,310,19,color);canvas.drawLine(310,19,308,22,color);}
}
void processAlerts(JsonDocument& doc,int64_t now) {
    bool changed=false;
    const char* types[]={"exhausted","reset","banked_used","banked_gained","warning_10","warning_5"};
    const char* messages[]={"USAGE EXHAUSTED","ALLOWANCE RESET","BANKED RESET USED","BANKED RESET GAINED","10% OR LESS REMAINS","5% OR LESS REMAINS"};
    for(JsonObject event:doc["alerts"].as<JsonArray>()) {
        uint64_t id=event["id"]|0ULL;if(id<=lastAlert)continue;
        int kind=-1;for(int i=0;i<6;i++)if(event["type"]==types[i])kind=i;
        int account=event["account"]|-1;
        int64_t age=now-(event["at"]|0LL);
        if(kind>=0 && account>=0 && account<2 && age>=0 && age<120) {
            if(!audioQueueAlert(kind,account))break;
            alertBanner=String(account==0?"PRIMARY: ":"SECONDARY: ")+messages[kind];bannerUntil=millis()+10000;
        }
        lastAlert=id;changed=true;
    }
    if(changed){Preferences p;p.begin("oa-display",false);p.putULong64("alertseq",lastAlert);p.end();}
}
void draw() {
    String body,net;uint32_t received;
    xSemaphoreTake(stateLock,portMAX_DELAY);body=packet;net=networkState;received=packetAt;xSemaphoreGive(stateLock);
    canvas.fillScreen(BG);
    if(!configured || body.isEmpty()) {
        text("QUOTAPANEL",20,25,MINT,2);
        large("ESP32",20,65,WHITE);
        text(configured?net:"READY TO PAIR",20,120,AMBER);
        text(configured?"Pair accounts with the USB tool":"Run quotapanel.py configure",20,145,MUTED);
        text("Wi-Fi + direct OpenAI connection",20,162,MUTED);
    } else {
        JsonDocument doc;deserializeJson(doc,body);
        int64_t now=doc["now"].as<int64_t>()+(uint32_t)(millis()-received)/1000;
        processAlerts(doc,now);
        uint32_t freshness=nativeEnabled()?90000:15000;
        if(detailPage>=0)drawDetails(doc["accounts"][detailPage],now,net=="LIVE" && millis()-received<freshness);
        else {
            text("QUOTAPANEL",12,10,WHITE,2);
            bool live=net=="LIVE" && !stale(doc["accounts"][0],now) && !stale(doc["accounts"][1],now) && millis()-received<freshness;
            canvas.fillCircle(242,17,3,live?MINT:AMBER);right(live?"LIVE":"STALE",282,13,live?MINT:AMBER);speakerIcon();
            drawCard(doc["accounts"][0],0,30,now,net=="LIVE" && millis()-received<freshness);drawCard(doc["accounts"][1],1,130,now,net=="LIVE" && millis()-received<freshness);
            int64_t updated=min(doc["accounts"][0]["updated"].as<int64_t>(),doc["accounts"][1]["updated"].as<int64_t>());
            String age=updated>0?String((long)(now-updated))+"s":"--";
            if((int32_t)(bannerUntil-millis())>0)text(alertBanner,12,232,MINT);
            else {text((nativeEnabled()?"DIRECT ":"SYNC ")+age+" AGO",12,232,MUTED);right("TAP ACCOUNT FOR DETAILS",308,232,MUTED);}
        }
    }
    canvas.pushSprite(0,0);
}
void touch() {
    static bool down=false;static uint32_t last=0;
    if(millis()-last<40)return;last=millis();
    Wire.beginTransmission(XZ_TOUCH_ADDR);Wire.write(2);
    if(Wire.endTransmission(false)!=0)return;
    if(Wire.requestFrom(XZ_TOUCH_ADDR,5)!=(uint8_t)5)return;
    int n=Wire.read()&15;int xh=Wire.read(),xl=Wire.read(),yh=Wire.read(),yl=Wire.read();
    int rawX=((xh&15)<<8)|xl,rawY=((yh&15)<<8)|yl;
    int x=319-rawY,y=rawX;
    if(n>0 && n<=2 && !down && x>=0&&x<320&&y>=0&&y<240){
        if(y<30 && x>100 && (detailPage<0 || x<205)){audioToggleMute();}
        else if(detailPage<0){detailPage=y<128?0:1;detailWindow=0;}
        else if(x>160 && y>32){detailWindow++;String body;xSemaphoreTake(stateLock,portMAX_DELAY);body=packet;xSemaphoreGive(stateLock);JsonDocument d;deserializeJson(d,body);if(d["accounts"][detailPage]["windows"].size()<=1)detailPage=-1;}
        else detailPage=-1;
        nextDraw=0;
    }
    down=n>0;
}
bool serialWriteAll(const uint8_t* bytes, size_t length) {
    size_t offset=0;uint32_t progress=millis();
    while(offset<length) {
        int room=Serial.isConnected()?min(64,Serial.availableForWrite()):0;
        if(room>0) {size_t count=Serial.write(bytes+offset,min((size_t)room,length-offset));if(count){offset+=count;progress=millis();}}
        if(millis()-progress>3000)return false;
        delay(1);
    }
    return true;
}
void configure(const String &input){
    JsonDocument net;bool valid=!deserializeJson(net,input);
    String name=net["ssid"]|"",pass=net["password"]|"",zone=net["timezone"]|"UTC0";
    valid=valid&&name.length()>0&&name.length()<=32&&pass.length()<=63&&zone.length()>0&&zone.length()<=96;
    for(size_t i=0;i<zone.length();i++)if(!isalnum(zone[i])&&String("+-:,/.").indexOf(zone[i])<0)valid=false;
    if(!valid){Serial.println("ERROR CONFIG");return;}
    JsonDocument clean;clean["ssid"]=name;clean["password"]=pass;clean["timezone"]=zone;String encoded;serializeJson(clean,encoded);
    Preferences store;bool saved=store.begin("oa-display",false)&&store.putString("net",encoded)==encoded.length()&&store.getString("net","")==encoded;store.end();
    if(!saved){Serial.println("ERROR CONFIG STORAGE");return;}
    Serial.println("OK CONFIG");Serial.flush();delay(150);ESP.restart();
}
void serialCommands() {
    static String line;
    static String transfer;static uint32_t transferLength=0,transferHash=0,transferAt=0;
    if(transferLength&&millis()-transferAt>60000){transfer="";transferLength=0;}
    while(Serial.available()) {
        char c=Serial.read();
        if(c=='\n') {
            if(line.startsWith("NBEGIN ")) {
                unsigned long length=0,hash=0;
                if(sscanf(line.c_str(),"NBEGIN %lu %lx",&length,&hash)==2&&length>0&&length<=8192){transfer="";transfer.reserve(length);transferLength=length;transferHash=hash;transferAt=millis();Serial.println("OK BEGIN");}
                else Serial.println("ERROR BEGIN");
            } else if(line.startsWith("NCHUNK ")) {
                int split=line.indexOf(' ',7);uint32_t offset=line.substring(7,split).toInt();String hex=line.substring(split+1),decoded;bool valid=split>7&&hex.length()>0&&hex.length()<=64&&hex.length()%2==0;
                for(size_t i=0;i<hex.length();i++)if(!isxdigit(hex[i]))valid=false;
                if(valid)for(size_t i=0;i<hex.length();i+=2)decoded+=(char)strtoul(hex.substring(i,i+2).c_str(),nullptr,16);
                if(valid&&transferLength&&offset==transfer.length()&&offset+decoded.length()<=transferLength){transfer+=decoded;transferAt=millis();Serial.println("OK CHUNK "+String(transfer.length()));}
                else Serial.println("ERROR CHUNK");
            } else if(line=="NCOMMIT") {
                uint32_t hash=2166136261u;for(size_t i=0;i<transfer.length();i++)hash=(hash^(uint8_t)transfer[i])*16777619u;
                if(transferLength&&transfer.length()==transferLength&&hash==transferHash){if(transfer.startsWith("CONFIG "))configure(transfer.substring(7));else Serial.println(nativeClientCommand(transfer));}else Serial.println("ERROR TRANSFER");
                transfer="";transferLength=0;
            } else if(line=="STATUS") {
                JsonDocument d;d["firmware"]="quotapanel-0.1.0-alpha.1";d["standalone"]=nativeEnabled();d["audioReady"]=audioReady();d["muted"]=audioMuted();d["volume"]=audioVolume();d["alertsPlayed"]=audioPlayed();d["audioFailures"]=audioFailed();d["configured"]=configured;d["wifi"]=WiFi.status()==WL_CONNECTED;
                d["ip"]=WiFi.localIP().toString();d["rssi"]=WiFi.RSSI();d["heap"]=ESP.getFreeHeap();d["uptime"]=millis()/1000;
                xSemaphoreTake(stateLock,portMAX_DELAY);d["network"]=networkState;d["transport"]=transport;d["packetAge"]=packet.isEmpty()?-1:(int)((millis()-packetAt)/1000);xSemaphoreGive(stateLock);
                serializeJson(d,Serial);Serial.println();
            } else if(line=="DATA") {
                xSemaphoreTake(stateLock,portMAX_DELAY);String reply=packet;xSemaphoreGive(stateLock);reply+="\n";
                serialWriteAll((const uint8_t*)reply.c_str(),reply.length());
            } else if(line.startsWith("NATIVE ")) {
                String reply=nativeClientCommand(line.substring(7));reply+="\n";
                serialWriteAll((const uint8_t*)reply.c_str(),reply.length());
            } else if(line=="REBOOT") {Serial.println("OK REBOOT");Serial.flush();delay(100);ESP.restart();
            } else if(line=="CAPTURE") {
                if(!capturedFrame)capturedFrame=(uint8_t*)ps_malloc(320*240*3);
                if(!capturedFrame)Serial.println("ERROR MEMORY");
                else {canvas.readRectRGB(0,0,320,240,capturedFrame);Serial.println("OK CAPTURE");}
            } else if(line.startsWith("ROW ")) {
                int y=line.substring(4).toInt();
                if(!capturedFrame||y<0||y>=240)Serial.println("ERROR ROW");
                else {
                    char row[1960];int offset=snprintf(row,sizeof(row),"\nROW %d ",y);uint32_t hash=2166136261u;
                    for(int i=0;i<960;i++){uint8_t v=capturedFrame[y*960+i];hash=(hash^v)*16777619u;sprintf(row+offset,"%02x",v);offset+=2;}
                    offset+=snprintf(row+offset,sizeof(row)-offset," %08lx\n",(unsigned long)hash);
                    serialWriteAll((const uint8_t*)row,offset);
                }
            } else if(line.startsWith("ALERT ")) {
                int kind=-1,account=0;sscanf(line.c_str(),"ALERT %d %d",&kind,&account);
                Serial.println(audioQueueAlert(kind,account)?"OK ALERT":"ERROR AUDIO");
            } else if(line.startsWith("VOLUME ")) {audioSetVolume(line.substring(7).toInt());Serial.println("OK VOLUME");
            } else if(line.startsWith("VIEW ")) {detailPage=line.substring(5).toInt();if(detailPage<0||detailPage>1)detailPage=-1;detailWindow=0;nextDraw=0;Serial.println("OK VIEW");}
            else if(line.startsWith("CONFIG "))configure(line.substring(7));

            line="";
        } else if(c!='\r') {if(line.length()<8192)line+=c;else line="";}
    }
}
void setup() {
    Serial.begin(115200);Serial.setTxTimeoutMs(100);
    stateLock=xSemaphoreCreateMutex();
    lcd.init();lcd.setRotation(1);lcd.setBrightness(190);
    canvas.setColorDepth(16);canvas.setPsram(true);
    if(!canvas.createSprite(320,240)){lcd.fillScreen(TFT_RED);while(true)delay(1000);}
    pinMode(XZ_BOOT_KEY,INPUT_PULLUP);
    pinMode(XZ_TOUCH_RST,OUTPUT);digitalWrite(XZ_TOUCH_RST,LOW);delay(15);digitalWrite(XZ_TOUCH_RST,HIGH);delay(100);
    Wire.begin(XZ_TOUCH_SDA,XZ_TOUCH_SCL,400000);Wire.setTimeOut(20);
    prefs.begin("oa-display",true);
    JsonDocument net;deserializeJson(net,prefs.getString("net","{}"));
    ssid=net["ssid"].is<const char*>()?net["ssid"].as<String>():prefs.getString("ssid","");
    password=net["password"].is<const char*>()?net["password"].as<String>():prefs.getString("pass","");
    displayTimezone=net["timezone"]|"UTC0";lastAlert=prefs.getULong64("alertseq",0);prefs.end();

    audioBegin();
    nativeClientBegin();
    configured=ssid.length()>0;
    setenv("TZ",displayTimezone.c_str(),1);tzset();
    if(configured)xTaskCreatePinnedToCore(networkTask,"wifi",4096,nullptr,1,nullptr,0);
    draw();Serial.println("USAGE DISPLAY READY");
}
void loop() {
    serialCommands();touch();
    static bool pressed=false,longPress=false;static uint32_t pressedAt=0;
    bool key=digitalRead(XZ_BOOT_KEY)==LOW;
    if(key&&!pressed){pressedAt=millis();longPress=false;}
    if(key&&pressed&&!longPress&&millis()-pressedAt>=1500){audioToggleMute();longPress=true;nextDraw=0;}
    if(!key&&pressed&&!longPress){detailPage=detailPage<0?0:detailPage==0?1:-1;detailWindow=0;nextDraw=0;}
    pressed=key;
    if((int32_t)(millis()-nextDraw)>=0){draw();nextDraw=millis()+1000;}
    delay(5);
}
