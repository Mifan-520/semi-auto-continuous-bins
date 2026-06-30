#pragma once

#include <Arduino.h>
#include "Config.h"

// M1 placeholder only. Later phases will call this to send one JSON line to DTU.
// 接线约定: ESP32 GPIO25 (TX, 输出) → DTU RX (输入)
//          ESP32 GPIO32 (RX, 输入) ← DTU TX (一般不用, DTU 只收不发)
// Serial1.begin(baud, config, RX_PIN, TX_PIN), 故 RX=32, TX=25。
inline void CloudReport_Init(uint32_t baud = DTU_BAUD_DEFAULT) {
    Serial1.begin(baud, SERIAL_8N1, 32, 25);  // RX=GPIO32, TX=GPIO25
}

inline void CloudReport_SendEventJson(const char* jsonLine) {
    if (!jsonLine || !jsonLine[0]) return;
    Serial1.println(jsonLine);
}

