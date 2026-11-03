#include "NotificationSound.h"
#include "ChimePcm.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s_std.h>
#include <math.h>

namespace NotificationSound {
namespace {
constexpr int kAmp=46,kRate=ChimePcm::rate;
i2s_chan_handle_t channel=nullptr;
bool running=false;
uint32_t started=0;

// ES8311 register sequence based on Waveshare's 07_ES8311 (Espressif,
// Apache-2.0). Fixed 16 kHz / 256fs MCLK / I2S 16-bit stereo playback only.
// All codec access runs in hudLoop, alongside touch/PMU on the same Wire bus.
bool reg(uint8_t address,uint8_t value) {
  Wire.beginTransmission(0x18);Wire.write(address);Wire.write(value);
  return Wire.endTransmission()==0;
}
bool codecStart() {
  const uint8_t registers[][2]={
    {0x00,0x1f},{0x00,0x00},{0x00,0x80},
    {0x01,0x3f},{0x02,0x00},{0x03,0x10},{0x04,0x10},
    {0x05,0x00},{0x06,0x03},{0x07,0x00},{0x08,0xff},
    {0x09,0x0c},{0x0a,0x4c}, // DAC input enabled; ADC serial output disabled
    {0x0d,0x01},{0x0e,0xff},{0x14,0x00}, // ADC/PGA remain powered down
    {0x12,0x00},{0x13,0x10},{0x37,0x08},
    {0x32,0xa7},{0x31,0x00} // DAC -12 dB; PCM amplitude further limits loudness
  };
  for(const auto &r:registers)if(!reg(r[0],r[1]))return false;
  return true;
}
void codecStop() {
  // Power down analog paths and clocks (Espressif ES8311 suspend sequence).
  for(const auto &r: {0x31,0x12,0x0e,0x0d,0x01}) {
    uint8_t value=r==0x31?0x60:r==0x12?0x02:r==0x0e?0xff:r==0x0d?0xfc:0x00;
    reg(r,value);
  }
}
void silence() {
  digitalWrite(kAmp,LOW); // Immediate mute, even if codec access fails.
  if(channel) {
    if(running)i2s_channel_disable(channel);
    i2s_del_channel(channel);channel=nullptr;
    codecStop();
  }
  running=false;
}
bool play() {
  silence();
  constexpr int frames=ChimePcm::frames;
  i2s_chan_config_t config=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO,I2S_ROLE_MASTER);
  config.dma_desc_num=(frames+511)/512;config.dma_frame_num=512;config.auto_clear=true;
  if(i2s_new_channel(&config,&channel,nullptr)!=ESP_OK)return false;
  i2s_std_config_t standard={};
  standard.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(kRate);
  standard.slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO);
  standard.gpio_cfg.mclk=GPIO_NUM_42;standard.gpio_cfg.bclk=GPIO_NUM_9;
  standard.gpio_cfg.ws=GPIO_NUM_45;standard.gpio_cfg.dout=GPIO_NUM_8;standard.gpio_cfg.din=I2S_GPIO_UNUSED;
  if(i2s_channel_init_std_mode(channel,&standard)!=ESP_OK)return false;
  int16_t *pcm=(int16_t*)malloc(frames*2*sizeof(int16_t));if(!pcm)return false;
  for(int i=0;i<frames;++i)pcm[i*2]=pcm[i*2+1]=ChimePcm::sample(i);
  size_t loaded=0,bytes=frames*2*sizeof(int16_t);
  bool ok=i2s_channel_preload_data(channel,pcm,bytes,&loaded)==ESP_OK && loaded==bytes;
  free(pcm);
  if(!ok || !codecStart())return false;
  if(i2s_channel_enable(channel)!=ESP_OK)return false;
  running=true;started=millis();digitalWrite(kAmp,HIGH);return true;
}
}
void begin() {pinMode(kAmp,OUTPUT);digitalWrite(kAmp,LOW);}
void stop() {silence();}
void notify() {if(!play()){silence();Serial.println("{\"type\":\"telemetry\",\"event\":\"audio_error\"}");}}
void update(uint32_t now) {
  if(running && uint32_t(now-started)>=uint32_t(ChimePcm::frames*1000/kRate+12))silence();
}
}
