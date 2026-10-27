#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <LovyanGFX.hpp>
#include "board_pins.h"

class XiaozhiDisplay : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9341 panel_;
    lgfx::Bus_SPI bus_;
    lgfx::Light_PWM light_;
public:
    XiaozhiDisplay() {
        auto b = bus_.config();
        b.spi_host = SPI2_HOST;
        b.spi_mode = 0;
        b.freq_write = 40000000;
        b.freq_read = 16000000;
        b.spi_3wire = false;
        b.use_lock = true;
        b.dma_channel = SPI_DMA_CH_AUTO;
        b.pin_sclk = XZ_LCD_SCLK;
        b.pin_mosi = XZ_LCD_MOSI;
        b.pin_miso = XZ_LCD_MISO;
        b.pin_dc = XZ_LCD_DC;
        bus_.config(b);
        panel_.setBus(&bus_);
        auto p = panel_.config();
        p.pin_cs = XZ_LCD_CS;
        p.pin_rst = -1; // LCD reset shares ESP32 CHIP_PU/EN
        p.pin_busy = -1;
        p.panel_width = XZ_LCD_W;
        p.panel_height = XZ_LCD_H;
        p.memory_width = XZ_LCD_W;
        p.memory_height = XZ_LCD_H;
        p.offset_x = 0;
        p.offset_y = 0;
        p.offset_rotation = 0;
        p.readable = false;
        // The board's IPS panel requires display inversion (matches the
        // LovyanGFX Xiaozhi/ESP32-S3 Touch LCD profile).
        p.invert = true;
        p.rgb_order = false;
        p.bus_shared = false;
        panel_.config(p);
        setPanel(&panel_);
        auto l = light_.config();
        l.pin_bl = XZ_LCD_BL;
        l.invert = false;
        l.freq = 44100;
        l.pwm_channel = 7;
        light_.config(l);
        panel_.setLight(&light_);
    }
};
extern XiaozhiDisplay lcd;
