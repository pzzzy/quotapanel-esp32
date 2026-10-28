// SPDX-License-Identifier: GPL-3.0-only
#include "wifi_update.h"
#include "ota_format.h"
#include "ota_public_key.h"
#include "firmware_version.h"
#include "native_client.h"
#include "alerts_audio.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_https_server.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <atomic>
#include <algorithm>

#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "Wi-Fi updates require bootloader rollback support"
#endif
// Arduino normally confirms an OTA image before setup; defer that decision.
extern "C" bool verifyRollbackLater(){return true;}
namespace {
httpd_handle_t server=nullptr;
String certificate,privateKey,authorizationHash,hostname;
std::atomic<bool> busy{false},pendingVerify{false},accountsHealthy{false},serverReady{false};
std::atomic<uint32_t> minimumSequence{QP_SEQUENCE},progress{0},restartAt{0},lastTick{0},healthyPolls{0};
std::atomic<const char*> state{"not_configured"};
uint32_t startedAt=0;
String hex(const uint8_t* data,size_t size){
    String s;s.reserve(size*2);const char* digits="0123456789abcdef";
    for(size_t i=0;i<size;i++){s+=digits[data[i]>>4];s+=digits[data[i]&15];}return s;
}
bool hashString(const String& value,String& result){
    uint8_t digest[32];if(mbedtls_sha256_ret((const uint8_t*)value.c_str(),value.length(),digest,0))return false;
    result=hex(digest,32);return true;
}
bool authorized(httpd_req_t* request){
    char value[80]={};
    if(httpd_req_get_hdr_value_len(request,"Authorization")!=71 ||
       httpd_req_get_hdr_value_str(request,"Authorization",value,sizeof(value))!=ESP_OK ||
       memcmp(value,"Bearer ",7))return false;
    String digest;if(!hashString(String(value+7),digest))return false;
    return authorizationHash.length()==64&&otaformat::equal(digest.c_str(),authorizationHash.c_str(),64);
}
esp_err_t reply(httpd_req_t* request,const char* code,const char* message){
    httpd_resp_set_status(request,code);httpd_resp_set_type(request,"application/json");
    httpd_resp_set_hdr(request,"Cache-Control","no-store");httpd_resp_set_hdr(request,"Connection","close");
    return httpd_resp_sendstr(request,message);
}
bool signature(const uint8_t* header){
    uint8_t digest[32];if(mbedtls_sha256_ret(header,otaformat::METADATA_SIZE,digest,0))return false;
    mbedtls_pk_context key;mbedtls_pk_init(&key);
    mbedtls_mpi r,s;mbedtls_mpi_init(&r);mbedtls_mpi_init(&s);
    bool ok=mbedtls_pk_parse_public_key(&key,(const uint8_t*)OTA_PUBLIC_KEY,sizeof(OTA_PUBLIC_KEY))==0&&
        mbedtls_pk_can_do(&key,MBEDTLS_PK_ECDSA)&&
        mbedtls_mpi_read_binary(&r,header+112,32)==0&&mbedtls_mpi_read_binary(&s,header+144,32)==0;
    if(ok){auto ec=mbedtls_pk_ec(key);ok=ec->grp.id==MBEDTLS_ECP_DP_SECP256R1&&
        mbedtls_ecdsa_verify(&ec->grp,digest,sizeof(digest),&ec->Q,&r,&s)==0;}
    mbedtls_mpi_free(&r);mbedtls_mpi_free(&s);mbedtls_pk_free(&key);return ok;
}
bool receive(httpd_req_t* request,uint8_t* data,size_t size,uint32_t deadline){
    size_t offset=0;
    while(offset<size){
        if((int32_t)(millis()-deadline)>=0)return false;
        int n=httpd_req_recv(request,(char*)data+offset,size-offset);
        if(n<=0)return false;offset+=n;
    }return true;
}
esp_err_t statusHandler(httpd_req_t* request){
    if(!authorized(request))return reply(request,"401 Unauthorized","{\"error\":\"unauthorized\"}");
    JsonDocument doc;wifiUpdateStatus(doc.to<JsonObject>());String body;serializeJson(doc,body);
    return reply(request,"200 OK",body.c_str());
}
esp_err_t updateHandler(httpd_req_t* request){
    if(!authorized(request))return reply(request,"401 Unauthorized","{\"error\":\"unauthorized\"}");
    if(pendingVerify.load()||busy.load())return reply(request,"409 Conflict","{\"error\":\"not_ready\"}");
    if(request->content_len<otaformat::HEADER_SIZE+256 || request->content_len>otaformat::HEADER_SIZE+otaformat::MAX_IMAGE)
        return reply(request,"400 Bad Request","{\"error\":\"invalid_length\"}");
    uint8_t header[otaformat::HEADER_SIZE];otaformat::Metadata metadata;
    if(!receive(request,header,sizeof(header),millis()+15000)||
       !otaformat::parse(header,sizeof(header),minimumSequence.load(),metadata)||
       request->content_len!=sizeof(header)+metadata.size||!signature(header))
        return reply(request,"400 Bad Request","{\"error\":\"invalid_signed_update\"}");
    const auto target=esp_ota_get_next_update_partition(nullptr);
    if(!target||target==esp_ota_get_running_partition()||target->size<metadata.size)
        return reply(request,"409 Conflict","{\"error\":\"no_safe_slot\"}");
    busy=true;progress=0;state="waiting_for_account_requests";
    // A token rotation must finish durably before any update-triggered reboot.
    if(!nativePauseForUpdate(90000)){
        busy=false;nativeResumeAfterUpdate();state="account_request_busy";
        return reply(request,"503 Service Unavailable","{\"error\":\"account_request_busy\"}");
    }
    esp_ota_handle_t handle=0;bool opened=false;const char* failure=nullptr;
    mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);
    if(esp_ota_begin(target,metadata.size,&handle)!=ESP_OK)failure="flash_begin_failed";
    else opened=true;
    if(!failure&&mbedtls_sha256_starts_ret(&sha,0)!=0)failure="hash_failed";
    uint8_t buffer[4096];uint32_t received=0,deadline=millis()+180000;state="receiving";
    while(!failure&&received<metadata.size){
        size_t n=std::min(sizeof(buffer),(size_t)(metadata.size-received));
        if(!receive(request,buffer,n,deadline)){failure="upload_interrupted";break;}
        if(mbedtls_sha256_update_ret(&sha,buffer,n)!=0||esp_ota_write(handle,buffer,n)!=ESP_OK){failure="write_failed";break;}
        received+=n;progress=received*100/metadata.size;
    }
    uint8_t digest[32];
    if(!failure&&(mbedtls_sha256_finish_ret(&sha,digest)!=0||!otaformat::equal(digest,metadata.digest,32)))failure="image_hash_mismatch";
    mbedtls_sha256_free(&sha);
    if(!failure){esp_err_t result=esp_ota_end(handle);opened=false;if(result!=ESP_OK)failure="invalid_firmware_image";}
    if(opened)esp_ota_abort(handle);
    if(!failure){
        Preferences p;JsonDocument doc;doc["sequence"]=metadata.sequence;doc["address"]=target->address;doc["version"]=metadata.version;
        String encoded;serializeJson(doc,encoded);
        bool saved=p.begin("qp-ota",false,"accounts")&&p.putString("pending",encoded)==encoded.length()&&p.getString("pending","")==encoded;p.end();
        if(!saved)failure="checkpoint_failed";
    }
    if(!failure&&esp_ota_set_boot_partition(target)!=ESP_OK)failure="boot_selection_failed";
    if(failure){state=failure;busy=false;nativeResumeAfterUpdate();JsonDocument doc;doc["error"]=failure;String body;serializeJson(doc,body);return reply(request,"400 Bad Request",body.c_str());}
    state="rebooting";reply(request,"202 Accepted","{\"accepted\":true,\"state\":\"rebooting\"}");restartAt=millis()+3000;return ESP_FAIL; // Close TLS cleanly before reboot.
}
void monitor(void*){
    for(;;){
        if(pendingVerify.load()){
            // Do not accept an image merely because it reached setup().
            bool healthy=serverReady.load()&&WiFi.status()==WL_CONNECTED&&accountsHealthy.load()&&healthyPolls.load()>=2&&
                millis()-lastTick.load()<3000&&audioReady()&&millis()-startedAt>=30000;
            if(healthy){
                Preferences p;bool saved=p.begin("qp-ota",false,"accounts")&&p.putUInt("minimum",QP_SEQUENCE)==4&&p.getUInt("minimum",0)==QP_SEQUENCE;p.end();
                if(saved&&esp_ota_mark_app_valid_cancel_rollback()==ESP_OK){minimumSequence=QP_SEQUENCE;pendingVerify=false;state="ready";}
            }
            if(pendingVerify.load()&&millis()-startedAt>180000){
                state="rolling_back";if(nativePauseForUpdate(90000)){esp_ota_mark_app_invalid_rollback_and_reboot();ESP.restart();}
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
}
void wifiUpdateAccountHealth(bool healthy){accountsHealthy=healthy;if(healthy)healthyPolls++;else healthyPolls=0;}
bool wifiUpdateBusy(){return busy.load();}
void wifiUpdateStatus(JsonObject status){
    status["version"]=QP_VERSION;status["sequence"]=QP_SEQUENCE;status["minimumSequence"]=minimumSequence.load();
    status["updating"]=busy.load();status["state"]=state.load();status["pendingVerify"]=pendingVerify.load();status["progress"]=progress.load();
    status["hostname"]=hostname;status["port"]=8443;status["uptime"]=millis()/1000;
    status["accountsHealthy"]=accountsHealthy.load();status["healthyPolls"]=healthyPolls.load();
    auto running=esp_ota_get_running_partition(),boot=esp_ota_get_boot_partition();
    status["runningSlot"]=running?running->label:"unknown";status["bootSlot"]=boot?boot->label:"unknown";
}
bool wifiUpdateConfigure(const String& encoded){
    if(busy.load()||encoded.length()>4096)return false;
    JsonDocument doc;if(deserializeJson(doc,encoded))return false;
    String cert=doc["certificate"]|"",key=doc["privateKey"]|"",auth=doc["authorizationHash"]|"";
    if(auth.length()!=64||cert.length()>2048||key.length()>1024)return false;
    for(char c:auth)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
    mbedtls_x509_crt crt;mbedtls_x509_crt_init(&crt);mbedtls_pk_context pk;mbedtls_pk_init(&pk);
    bool valid=mbedtls_x509_crt_parse(&crt,(const uint8_t*)cert.c_str(),cert.length()+1)==0&&
        mbedtls_pk_parse_key(&pk,(const uint8_t*)key.c_str(),key.length()+1,nullptr,0)==0&&mbedtls_pk_check_pair(&crt.pk,&pk)==0;
    mbedtls_x509_crt_free(&crt);mbedtls_pk_free(&pk);if(!valid)return false;
    Preferences p;bool saved=p.begin("qp-ota",false,"accounts")&&p.putString("config",encoded)==encoded.length()&&p.getString("config","")==encoded;p.end();return saved;
}
void wifiUpdateBegin(){
    startedAt=millis();lastTick=millis();
    hostname="quotapanel-"+String((uint32_t)(ESP.getEfuseMac()>>24)&0xffffff,HEX);
    const auto running=esp_ota_get_running_partition();esp_ota_img_states_t imageState;
    pendingVerify=esp_ota_get_state_partition(running,&imageState)==ESP_OK&&imageState==ESP_OTA_IMG_PENDING_VERIFY;
    Preferences p;p.begin("qp-ota",true,"accounts");minimumSequence=std::max(QP_SEQUENCE,p.getUInt("minimum",0));
    JsonDocument config,pending;deserializeJson(config,p.getString("config","{}"));deserializeJson(pending,p.getString("pending","{}"));p.end();
    if(pendingVerify.load()&&(pending["sequence"].as<uint32_t>()!=QP_SEQUENCE||pending["address"].as<uint32_t>()!=running->address||pending["version"].as<String>()!=QP_VERSION)){
        esp_ota_mark_app_invalid_rollback_and_reboot();ESP.restart();
    }
    certificate=config["certificate"]|"";privateKey=config["privateKey"]|"";authorizationHash=config["authorizationHash"]|"";
    state=pendingVerify.load()?"verifying":certificate.length()?"waiting_for_wifi":"not_configured";
    xTaskCreate(monitor,"ota-health",4096,nullptr,1,nullptr);
}
void wifiUpdateTick(){
    lastTick=millis();uint32_t reboot=restartAt.load();if(reboot&&(int32_t)(millis()-reboot)>=0)ESP.restart();
    static uint32_t retry=0;
    if(!server&&certificate.length()&&WiFi.status()==WL_CONNECTED&&(int32_t)(millis()-retry)>=0){
        retry=millis()+30000;
        httpd_ssl_config_t config=HTTPD_SSL_CONFIG_DEFAULT();config.port_secure=8443;
        config.httpd.max_open_sockets=2;config.httpd.stack_size=14336;
        config.httpd.recv_wait_timeout=10;config.httpd.send_wait_timeout=10;
        config.cacert_pem=(const uint8_t*)certificate.c_str();config.cacert_len=certificate.length()+1;
        config.prvtkey_pem=(const uint8_t*)privateKey.c_str();config.prvtkey_len=privateKey.length()+1;
        if(httpd_ssl_start(&server,&config)==ESP_OK){
            httpd_uri_t status={};status.uri="/v1/status";status.method=HTTP_GET;status.handler=statusHandler;
            httpd_uri_t update={};update.uri="/v1/update";update.method=HTTP_POST;update.handler=updateHandler;
            if(httpd_register_uri_handler(server,&status)!=ESP_OK||httpd_register_uri_handler(server,&update)!=ESP_OK){httpd_ssl_stop(server);server=nullptr;state="server_failed";}
            else{serverReady=true;if(MDNS.begin(hostname.c_str()))MDNS.addService("quotapanel","tcp",8443);state=pendingVerify.load()?"verifying":"ready";}
        }else {server=nullptr;state="server_failed";}
    }
}
