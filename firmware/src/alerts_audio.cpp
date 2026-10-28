// SPDX-License-Identifier: GPL-3.0-only
// Offline speech and chimes for the ES8311 speaker output.
#include "alerts_audio.h"
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <driver/i2s.h>
#include <atomic>
#include <math.h>
#include "codec_es8311.h"
#include "voices.generated.h"
namespace {
constexpr int kI2cSda=16,kI2cScl=15,kAmpEnable=1;
constexpr i2s_port_t kI2sPort=I2S_NUM_0;
struct Alert {int type;int account;};
QueueHandle_t queue;
std::atomic<bool> ready{false},muted{false};
std::atomic<int> volume{25};
std::atomic<uint32_t> played{0},failed{0};
bool initI2s() {
    i2s_config_t config = {};
    config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    config.sample_rate = 22050;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    config.dma_buf_count = 8;
    config.dma_buf_len = 128;
    // S3 uses its fractional PLL clock divider; no audio APLL on this chip.
    config.use_apll = false;
    config.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    config.tx_desc_auto_clear = true;
    if (i2s_driver_install(kI2sPort, &config, 0, nullptr) != ESP_OK) return false;
    i2s_pin_config_t pins = {};
    pins.mck_io_num = 4; pins.bck_io_num = 5; pins.ws_io_num = 7;
    pins.data_out_num = 8; pins.data_in_num = I2S_PIN_NO_CHANGE;
    if (i2s_set_pin(kI2sPort, &pins) != ESP_OK || i2s_zero_dma_buffer(kI2sPort) != ESP_OK) {
        i2s_driver_uninstall(kI2sPort);
        return false;
    }

    return true;
}

bool writeSamples(const int16_t* mono,size_t count) {
    int16_t stereo[256];
    for(size_t offset=0;offset<count;offset+=128) {
        if(muted.load())return false;
        size_t n=min((size_t)128,count-offset);
        for(size_t i=0;i<n;i++)stereo[i*2]=stereo[i*2+1]=(int32_t)mono[offset+i]*volume.load()/100;
        size_t sent=0;uint32_t deadline=millis()+100;
        while(sent<n*4 && (int32_t)(deadline-millis())>0) {
            size_t written=0;
            esp_err_t e=i2s_write(kI2sPort,(uint8_t*)stereo+sent,n*4-sent,&written,pdMS_TO_TICKS(20));
            sent+=written;
            if(e!=ESP_OK)return false;
        }
        if(sent!=n*4)return false;
    }
    return true;
}
bool tone(float hz,int duration) {
    int16_t pcm[128];int total=22050*duration/1000;
    for(int offset=0;offset<total;offset+=128) {
        int count=min(128,total-offset);
        for(int i=0;i<count;i++) {
            float t=(float)(offset+i)/22050.f;
            float env=min(1.f,(float)(offset+i)/220.f)*min(1.f,(float)(total-offset-i)/500.f);
            // Soft fundamental plus a little upper partial, with click-free fades.
            pcm[i]=hz>0?10000*env*(sinf(2*M_PI*hz*t)+0.13f*sinf(4*M_PI*hz*t)):0;
        }
        if(!writeSamples(pcm,count))return false;
    }
    return true;
}
bool chime(int kind) {
    static const float notes[6][3]={{660,440,330},{523.25,659.25,783.99},{783.99,587.33,783.99},{659.25,830.61,987.77},{659.25,523.25,659.25},{523.25,392,523.25}};
    for(int i=0;i<3;i++)if(!tone(notes[kind][i],kind==0?180:125)||!tone(0,45))return false;
    return tone(0,180);
}
template<size_t N> bool voice(const int16_t (&samples)[N]) {return writeSamples(samples,N);}
void worker(void*) {
    Alert alert;
    for(;;) {
        if(xQueueReceive(queue,&alert,portMAX_DELAY)!=pdTRUE)continue;
        if(muted.load()||!ready.load())continue;
        i2s_zero_dma_buffer(kI2sPort);digitalWrite(kAmpEnable,LOW);vTaskDelay(pdMS_TO_TICKS(15));
        bool ok=chime(alert.type) && (alert.account==0?voice(voice_primary):voice(voice_secondary)) && tone(0,100);
        if(ok)switch(alert.type){case 0:ok=voice(voice_exhausted);break;case 1:ok=voice(voice_reset);break;case 2:ok=voice(voice_banked_used);break;case 3:ok=voice(voice_banked_gained);break;case 4:ok=voice(voice_warning_10);break;case 5:ok=voice(voice_warning_5);break;}
        if(ok){tone(0,80);played.fetch_add(1);}else if(!muted.load())failed.fetch_add(1);
        vTaskDelay(pdMS_TO_TICKS(80));digitalWrite(kAmpEnable,HIGH);i2s_zero_dma_buffer(kI2sPort);
        vTaskDelay(pdMS_TO_TICKS(350));
    }
}
}
void audioBegin() {
    pinMode(kAmpEnable,OUTPUT);digitalWrite(kAmpEnable,HIGH);
    Preferences p;p.begin("oa-display",true);muted=false;volume=p.getUInt("volume",25);p.end();
    if(!initI2s())return;
    if(!codecEs8311Begin()){i2s_driver_uninstall(kI2sPort);return;}
    queue=xQueueCreate(8,sizeof(Alert));
    if(!queue){i2s_driver_uninstall(kI2sPort);return;}
    ready=xTaskCreatePinnedToCore(worker,"alerts",4096,nullptr,2,nullptr,1)==pdPASS;
}
bool audioQueueAlert(int type,int account) {
    if(!ready.load() || type<0||type>5||account<0||account>1)return false;
    if(muted.load())return true;
    Alert a{type,account};return xQueueSend(queue,&a,0)==pdTRUE;
}
void audioToggleMute() {muted=!muted.load();if(muted)digitalWrite(kAmpEnable,HIGH);}
void audioSetVolume(int percent) {volume=max(0,min(100,percent));Preferences p;p.begin("oa-display",false);p.putUInt("volume",volume);p.end();}
bool audioMuted(){return muted.load();}
bool audioReady(){return ready.load();}
int audioVolume(){return volume.load();}
uint32_t audioPlayed(){return played.load();}
uint32_t audioFailed(){return failed.load();}
