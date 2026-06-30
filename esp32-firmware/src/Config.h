#pragma once

#include <Arduino.h>

// LCDWIKI 4.0inch ESP32-32E ST7796 E32R40T/E32N40T
static constexpr uint16_t SCREEN_WIDTH = 480;
static constexpr uint16_t SCREEN_HEIGHT = 320;

static constexpr int TFT_CS_PIN = 15;
static constexpr int TFT_DC_PIN = 2;
static constexpr int TFT_RST_PIN = -1;
static constexpr int TFT_MOSI_PIN = 13;
static constexpr int TFT_MISO_PIN = 12;
static constexpr int TFT_SCLK_PIN = 14;
static constexpr int TFT_BL_PIN = 27;
static constexpr int TOUCH_CS_PIN = 33;
static constexpr int TOUCH_IRQ_PIN = 36;

// Final deployment constraints; M1 keeps them as defaults/placeholders.
static constexpr uint8_t BIN_COUNT = 6;
static constexpr uint8_t DEFAULT_LOCAL_ID = 1;
static constexpr bool DEFAULT_GATEWAY_FLAG = true;
static constexpr uint8_t FIXED_GATEWAY_ID = 1;

// DTU 串口: ESP32 上报 JSON 给 DTU。
// 接线: ESP32 GPIO25(TX,输出) → DTU RX(输入); GPIO32(RX) ← DTU TX(备用,一般不用)。
// 命名从 ESP32 视角: TX_PIN=ESP32实际做TX输出的脚(25), RX_PIN=ESP32实际做RX输入的脚(32)。
static constexpr int DTU_TX_PIN = 25;   // ESP32 TX 输出脚
static constexpr int DTU_RX_PIN = 32;   // ESP32 RX 输入脚(备用)
static constexpr uint32_t DTU_BAUD_DEFAULT = 9600;

static constexpr uint32_t MESSAGE_CLEAR_MS = 5000;
static constexpr uint32_t UI_TICK_MS = 5;
static constexpr uint32_t SIM_UPDATE_MS = 200;

static const char* const BIN_NAMES[BIN_COUNT] = {
    "仓1", "仓2", "仓3", "仓4", "仓5", "仓6"
};

static constexpr uint32_t COLOR_ROASTEK = 0x661E2B;
static constexpr uint32_t COLOR_BG = 0xF3F3F3;
static constexpr uint32_t COLOR_PANEL = 0xFFFFFF;
static constexpr uint32_t COLOR_TEXT = 0x222222;
static constexpr uint32_t COLOR_MUTED = 0x888888;
static constexpr uint32_t COLOR_ONLINE = 0x66FF66;
static constexpr uint32_t COLOR_ERROR = 0xCC0000;

