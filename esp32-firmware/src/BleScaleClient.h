#pragma once

#include <Arduino.h>
#include <BluetoothSerial.h>
#include <cstring>
#include "Config.h"

// ============================================================
//  BLE/SPP 蓝牙客户端 — 连接 I6328A 透传模块, 读 A33E 重量
//  链路: A33E→RS485→I6328A→蓝牙SPP→ESP32
//  A33E Modbus-RTU (P6=13): 读寄存器8(毛重float)
// ============================================================

// I6328A 模块默认名(用户可改, 用AT命令配置)
static constexpr const char* BLE_SCALE_DEVICE_NAME = "I6328A";
static constexpr uint32_t BLE_SCALE_READ_INTERVAL_MS = 500;   // 500ms读取一次
static constexpr uint32_t BLE_SCALE_TIMEOUT_MS = 3000;        // 3秒没读到=离线
static constexpr uint32_t BLE_SCALE_RECONNECT_MS = 5000;      // 断开后5秒重连

// A33E Modbus-RTU 参数
static constexpr uint8_t A33E_SLAVE_ID = 1;
static constexpr uint8_t A33E_FC_READ_HOLDING = 3;
static constexpr uint16_t A33E_REG_GROSS = 0x0008;   // 毛重寄存器(2个寄存器, float32)
static constexpr uint16_t A33E_REG_COUNT = 2;

// Modbus CRC16 计算
inline uint16_t modbusCRC16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

inline BluetoothSerial btScale;
inline bool btScaleConnected = false;
inline bool btScaleEnabled = true;         // 可通过设置false回退到模拟模式
inline float btScaleCurrentWeight = 0.0f;  // 最新读到的真实重量
inline uint32_t btLastReadMs = 0;
inline uint32_t btLastSuccessMs = 0;
inline uint32_t btLastReconnectMs = 0;
inline uint32_t btScaleSuccessCount = 0;
inline uint32_t btScaleErrorCount = 0;

// ===== 发送 Modbus 读毛重命令 (非阻塞) =====
inline void btScaleSendModbusRead() {
    if (!btScaleConnected) return;

    uint8_t cmd[8];
    cmd[0] = A33E_SLAVE_ID;
    cmd[1] = A33E_FC_READ_HOLDING;
    cmd[2] = (A33E_REG_GROSS >> 8) & 0xFF;
    cmd[3] = A33E_REG_GROSS & 0xFF;
    cmd[4] = (A33E_REG_COUNT >> 8) & 0xFF;
    cmd[5] = A33E_REG_COUNT & 0xFF;
    uint16_t crc = modbusCRC16(cmd, 6);
    cmd[6] = crc & 0xFF;
    cmd[7] = (crc >> 8) & 0xFF;

    btScale.write(cmd, 8);
}

// ===== 解析 Modbus 响应 =====
inline bool btScaleParseResponse(float* outWeight) {
    // 标准响应: 01 03 04 [float32_be_4bytes] [CRC16_2bytes] = 9字节
    // 或:      01 83 [error_code] [CRC] (异常响应)
    if (btScale.available() < 9) return false;

    uint8_t buf[16];
    int len = 0;
    // 读取所有可用字节, 找完整的9字节响应帧
    while (btScale.available() && len < 16) {
        buf[len++] = btScale.read();
    }
    if (len < 9) return false;

    // 找到从站地址+功能码
    int start = -1;
    for (int i = 0; i < len - 8; ++i) {
        if (buf[i] == A33E_SLAVE_ID && (buf[i + 1] == A33E_FC_READ_HOLDING || buf[i + 1] == 0x83)) {
            start = i;
            break;
        }
    }
    if (start < 0) return false;

    // 检查功能码
    if (buf[start + 1] == 0x83) {
        // 异常响应
        Serial.printf("[BleScale] A33E异常: code=0x%02X\n", buf[start + 2]);
        return false;
    }

    // 正常响应: 地址+功能码+字节数(4)+4字节float+2字节CRC = 9
    if (start + 9 > len) return false;
    if (buf[start + 2] != 4) return false;  // 字节数应为4

    // 验证CRC
    uint16_t crc_rcvd = buf[start + 7] | (buf[start + 8] << 8);
    uint16_t crc_calc = modbusCRC16(buf + start, 7);
    if (crc_rcvd != crc_calc) {
        Serial.println("[BleScale] CRC校验失败");
        return false;
    }

    // 提取 float32 (大端, Modbus标准)
    uint32_t raw = (buf[start + 3] << 24) | (buf[start + 4] << 16) |
                   (buf[start + 5] << 8) | buf[start + 6];
    float val = *reinterpret_cast<float*>(&raw);

    if (isnan(val) || isinf(val) || val < 0) {
        Serial.printf("[BleScale] 无效值: %.2f\n", val);
        return false;
    }

    *outWeight = val;
    return true;
}

// ===== 初始化 =====
inline void BleScale_Init() {
    // BluetoothSerial 与 WiFi/ESP-NOW 可共存 (ESP32双模)
    Serial.printf("[BleScale] 准备连接 I6328A (\"%s\")...\n", BLE_SCALE_DEVICE_NAME);
    // 注意: 如果已在主循环调用 begin() 可能导致连接阻塞, 用 tryConnect
    btScale.begin("A33E-Reader", false);  // 从机模式? 我们是主设备去连I6328A
    Serial.println("[BleScale] BluetoothSerial已启动");
}

// ===== 尝试连接 I6328A (非阻塞) =====
inline void btScaleTryConnect() {
    if (btScaleConnected) return;
    if (millis() - btLastReconnectMs < BLE_SCALE_RECONNECT_MS) return;
    btLastReconnectMs = millis();

    Serial.printf("[BleScale] 尝试连接 %s...\n", BLE_SCALE_DEVICE_NAME);
    bool ok = btScale.connect(BLE_SCALE_DEVICE_NAME);
    if (ok) {
        btScaleConnected = true;
        Serial.println("[BleScale] 已连接 I6328A");
        btLastSuccessMs = millis();
    } else {
        Serial.println("[BleScale] 连接失败 (I6328A未上电/未配对/距离远), 5秒后重试");
    }
}

// ===== 断开连接 =====
inline void btScaleDisconnect() {
    if (btScaleConnected) {
        btScale.disconnect();
        btScaleConnected = false;
        Serial.println("[BleScale] 已断开");
    }
}

// ===== 主循环 (由 main 调用) =====
inline float BleScale_Loop(float fallbackWeight) {
    if (!btScaleEnabled) return fallbackWeight;

    // 尝试连接
    if (!btScaleConnected) {
        btScaleTryConnect();
        return fallbackWeight;
    }

    // 检查蓝牙是否仍然连接
    if (!btScale.isReady() || !btScale.connected()) {
        btScaleConnected = false;
        Serial.println("[BleScale] 连接丢失");
        return fallbackWeight;
    }

    uint32_t now = millis();

    // 定时发送 Modbus 读命令
    if (now - btLastReadMs >= BLE_SCALE_READ_INTERVAL_MS) {
        btLastReadMs = now;
        btScaleSendModbusRead();
    }

    // 尝试解析响应
    float weight;
    if (btScaleParseResponse(&weight)) {
        btScaleCurrentWeight = weight;
        btLastSuccessMs = now;
        btScaleSuccessCount++;

        if (btScaleSuccessCount == 1 || btScaleSuccessCount % 50 == 0) {
            Serial.printf("[BleScale] 重量: %.1f kg (第%u次)\n", weight, btScaleSuccessCount);
        }
        return weight;
    }

    btScaleErrorCount++;
    // 超时回退到模拟
    if (now - btLastSuccessMs > BLE_SCALE_TIMEOUT_MS) {
        // 连续超时, 仍返回 fallbackWeight (不丢失称重显示)
    }

    // 最近有成功读数, 保持上一个值
    if (btLastSuccessMs > 0 && now - btLastSuccessMs < BLE_SCALE_TIMEOUT_MS) {
        return btScaleCurrentWeight;
    }

    return fallbackWeight;
}

// ===== 查询函数 =====
inline float BleScale_GetWeight() { return btScaleCurrentWeight; }
inline bool BleScale_IsConnected() { return btScaleConnected && (millis() - btLastSuccessMs < BLE_SCALE_TIMEOUT_MS); }
inline uint32_t BleScale_GetSuccessCount() { return btScaleSuccessCount; }
inline uint32_t BleScale_GetErrorCount() { return btScaleErrorCount; }

// ===== 启用/禁用蓝牙 (回退到模拟) =====
inline void BleScale_SetEnabled(bool enabled) { btScaleEnabled = enabled; }
inline bool BleScale_IsEnabled() { return btScaleEnabled; }

// ===== 设置设备名 (如果 I6328A 改了名) =====
// 在 main.cpp 调用: BleScale_SetDeviceName("MyI6328");
inline void BleScale_SetDeviceName(const char* name) {
    // 注意: 修改后需重新调用 Init
    // 实际存储见 Settings/NVS
    Serial.printf("[BleScale] 目标设备名: %s\n", name);
}
