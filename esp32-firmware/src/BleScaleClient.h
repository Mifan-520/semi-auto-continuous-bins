#pragma once

// ============================================================
//  BLE Client 称重读取 — 连接 I6328A-485 透传模块, 读 A33E 毛重
//
//  链路: A33E表头(9600) ──RS485──> I6328A-485(BLE从机)
//                                    ══BLE══ ESP32(BLE主机)
//
//  关键设计: BLE 扫描/连接是阻塞操作(数秒级), 若在主循环里跑,
//  会卡死 ESP-NOW 心跳(2秒一次)导致其他屏判定本机离线。
//  所以 BLE 全部逻辑放在独立 FreeRTOS 任务里, 主循环只读取
//  共享的最新重量值, UI/ESP-NOW 完全不受 BLE 阻塞影响。
//
//  读取到的真实毛重会通过 main.cpp → Display_SetCurrentWeight
//  → ESP-NOW 心跳的 currentWeight 字段自动广播给其他屏。
// ============================================================

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAddress.h>
#include "Config.h"
#include "EspnowMesh.h"   // 查询远程BLE状态, 实现先到先得协调

// ---------- 目标模块 (BLE) ----------
#ifndef BLE_SCALE_DEVICE_NAME
#define BLE_SCALE_DEVICE_NAME "XLCX-D05A50"
#endif

// 用 MAC 直连, 跳过扫描 (WiFi/ESP-NOW 共存时被动扫描收不到设备名)
// 每台屏配自己的模块, 改这里为该模块的 MAC (大写带冒号)
#ifndef BLE_SCALE_DEVICE_MAC
#define BLE_SCALE_DEVICE_MAC "39:02:01:D0:5A:50"
#endif

// ---------- I6328A GATT UUID ----------
static BLEUUID BLE_SCALE_SERVICE_UUID("0000ffe0-0000-1000-8000-00805f9b34fb");
static BLEUUID BLE_SCALE_WRITE_UUID  ("0000ffe1-0000-1000-8000-00805f9b34fb");
static BLEUUID BLE_SCALE_NOTIFY_UUID ("0000ffe2-0000-1000-8000-00805f9b34fb");

// ---------- 节奏 ----------
#ifndef BLE_SCALE_READ_INTERVAL_MS
#define BLE_SCALE_READ_INTERVAL_MS 2000   // 2秒读一次
#endif
#define BLE_SCALE_CONNECT_TIMEOUT_MS 5000 // 连接超时5秒(避免阻塞过久)
#define BLE_SCALE_RESYNC_MS 400           // 残包超时
#define BLE_SCALE_DATA_VALID_MS 5000      // 5秒内有读数视为有效

// ---------- A33E Modbus-RTU ----------
static const uint8_t  A33E_SLAVE_ID = 1;
static const uint16_t A33E_REG_GROSS = 8;
static const uint16_t A33E_REG_COUNT = 2;

// ---------- 状态(任务内部用) ----------
static BLEClient*               btClient = nullptr;
static BLERemoteService*        btService = nullptr;
static BLERemoteCharacteristic* btWrite = nullptr;
static BLERemoteCharacteristic* btNotify = nullptr;
static bool     btConnected = false;

// ---------- 与主循环共享的数据(原子读写, float/uint32 在 ESP32 上原子) ----------
static volatile float    btCurrentWeight = 0.0f;
static volatile uint8_t  myBinIdForBle = 1;   // 本机仓号, 由main设置, 决定是否主动连BLE
static volatile uint32_t btLastRecvMs = 0;
static volatile uint32_t btOkCount = 0;
static volatile uint32_t btErrCount = 0;
static volatile bool     btEnabled = true;

// ---------- 接收缓冲(任务内解析, 回调填充) ----------
static uint8_t  btRxbuf[32];
static size_t   btRxlen = 0;
static uint32_t btRxLastMs = 0;

// ---------- Modbus CRC16 ----------
static uint16_t btModbusCRC16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j)
            crc = (crc & 0x0001) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
    return crc;
}

// ---------- 构造并发送读毛重请求 ----------
static void btSendReadGross() {
    if (!btConnected || !btWrite) return;
    uint8_t cmd[8];
    cmd[0] = A33E_SLAVE_ID;
    cmd[1] = 0x03;
    cmd[2] = (A33E_REG_GROSS >> 8) & 0xFF;
    cmd[3] = A33E_REG_GROSS & 0xFF;
    cmd[4] = (A33E_REG_COUNT >> 8) & 0xFF;
    cmd[5] = A33E_REG_COUNT & 0xFF;
    uint16_t crc = btModbusCRC16(cmd, 6);
    cmd[6] = crc & 0xFF;
    cmd[7] = (crc >> 8) & 0xFF;
    btWrite->writeValue(cmd, 8, false);
}

// ---------- 解析一帧 ----------
static bool btTryParseFrame() {
    if (btRxlen < 9) return false;
    int start = -1;
    for (size_t i = 0; i + 9 <= btRxlen; ++i) {
        if (btRxbuf[i] == A33E_SLAVE_ID && btRxbuf[i+1] == 0x03 && btRxbuf[i+2] == 0x04) {
            start = (int)i; break;
        }
    }
    if (start < 0) return false;
    uint8_t* f = &btRxbuf[start];
    uint16_t crcCalc = btModbusCRC16(f, 7);
    uint16_t crcRcvd = f[7] | (f[8] << 8);
    if (crcRcvd != crcCalc) return false;
    uint32_t raw = ((uint32_t)f[3] << 24) | ((uint32_t)f[4] << 16) |
                   ((uint32_t)f[5] << 8) | (uint32_t)f[6];
    float val;
    memcpy(&val, &raw, 4);
    if (isnan(val) || isinf(val)) return false;
    btCurrentWeight = val;
    btLastRecvMs = millis();
    btOkCount++;
    return true;
}

// ---------- Notify 回调(BLE 栈上下文, 只填缓冲) ----------
static void btNotifyCallback(BLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify) {
    for (size_t i = 0; i < len && btRxlen < sizeof(btRxbuf); ++i)
        btRxbuf[btRxlen++] = data[i];
    btRxLastMs = millis();
}

// ---------- 连接 ----------
static bool btConnectByAddr(BLEAddress addr) {
    if (!btClient) btClient = BLEDevice::createClient();
    bool ok = btClient->connect(addr);
    if (!ok) { btConnected = false; return false; }
    btService = btClient->getService(BLE_SCALE_SERVICE_UUID);
    if (!btService) { btClient->disconnect(); btConnected = false; return false; }
    btWrite  = btService->getCharacteristic(BLE_SCALE_WRITE_UUID);
    btNotify = btService->getCharacteristic(BLE_SCALE_NOTIFY_UUID);
    if (!btWrite || !btNotify) { btClient->disconnect(); btConnected = false; return false; }
    if (btNotify->canNotify()) btNotify->registerForNotify(btNotifyCallback);
    btConnected = true;
    btRxlen = 0;
    Serial.printf("[BleScale] ✅ 已连接 %s\n", BLE_SCALE_DEVICE_NAME);
    return true;
}

// ============================================================
//  BLE 独立任务 — 所有阻塞操作都在这里, 不碰主循环
// ============================================================
static void bleScaleTask(void* arg) {
    uint32_t lastSend = 0;

    // ---- 角色判定: 仓号最小的屏才有资格连BLE, 其他屏永不主动连 ----
    // 这样彻底避免多台同时抢连的竞态, 也避免BLE射频争抢导致ESP-NOW掉线。
    // 仓号最小的屏若离线, 其他屏通过 gRemoteBleActive 超时后才有资格接管(下方处理)。
    // 简化版: 直接用本机仓号判断, 1号最高优先级。
    bool iAmCandidate = (myBinIdForBle == 1);  // 只有1号机主动连
    Serial.printf("[BleScale] 本机仓号=%d, %s主动连BLE\n",
                  myBinIdForBle, iAmCandidate ? "有资格" : "无资格(只接收共享重量)");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(50));

        if (!btEnabled) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

        // ---- 如果已有其他屏连上BLE, 本机绝不连(无论自己是不是候选) ----
        if (EspnowMesh_RemoteBleActive()) {
            if (btConnected) {
                btClient->disconnect();
                btConnected = false;
                EspnowMesh_SetLocalBleConnected(false);
                Serial.println("[BleScale] 其他屏已连BLE, 本机断开让出");
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // ---- 不是候选机(仓号非最小) → 永不主动连, 只等共享重量 ----
        if (!iAmCandidate) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // ---- 候选机且无人占用BLE → 尝试连接 ----
        if (!btConnected) {
            Serial.println("[BleScale] 本机为候选, 尝试连接(MAC直连)...");
            btWrite = btNotify = nullptr;
            btService = nullptr;
            btRxlen = 0;
            btConnectByAddr(BLEAddress(BLE_SCALE_DEVICE_MAC));
            if (btConnected) {
                EspnowMesh_SetLocalBleConnected(true);
                Serial.println("[BleScale] ✅ 本机已连上, 广播通知其他屏");
            } else {
                Serial.println("[BleScale] 连接失败, 5秒后重试");
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
            continue;
        }

        // ---- 连接丢失检测 ----
        if (!btClient || !btClient->isConnected()) {
            btConnected = false;
            EspnowMesh_SetLocalBleConnected(false);
            btWrite = btNotify = nullptr;
            btService = nullptr;
            btRxlen = 0;
            Serial.println("[BleScale] 连接丢失, 释放BLE占用");
            continue;
        }

        uint32_t now = millis();
        if (btRxlen > 0 && now - btRxLastMs > BLE_SCALE_RESYNC_MS) btRxlen = 0;
        if (now - lastSend >= BLE_SCALE_READ_INTERVAL_MS) {
            lastSend = now;
            btRxlen = 0;
            btSendReadGross();
        }
        if (btTryParseFrame()) {
            btRxlen = 0;
            uint32_t ok = btOkCount;
            float w = btCurrentWeight;
            if (ok == 1 || ok % 10 == 0)
                Serial.printf("[BleScale] 毛重=%.3f kg (第%u次)\n", w, ok);
        }
    }
}

// ============================================================
//  对外接口
// ============================================================
inline void BleScale_SetMyBinId(uint8_t binId) { myBinIdForBle = binId; }

inline void BleScale_Init() {
    Serial.printf("[BleScale] 启动 BLE 任务, 目标: %s [%s]\n",
                  BLE_SCALE_DEVICE_NAME, BLE_SCALE_DEVICE_MAC);
    BLEDevice::init("A33E-Reader");
    // 独立任务, 栈8K(BLE需要大栈), 固定到核心1(让核心0跑WiFi/ESP-NOW)
    xTaskCreatePinnedToCore(bleScaleTask, "bleScale", 8192, nullptr, 1, nullptr, 1);
}

// 主循环调用: 返回最新毛重。fallback=无有效读数时的回退值
// 优先级: 本机BLE读数 > 其他屏共享的BLE重量 > 回退值
inline float BleScale_Loop(float fallbackWeight) {
    if (!btEnabled) return fallbackWeight;
    // 1. 本机连着模块且有近期读数 → 返回本机真实重量
    if (btLastRecvMs > 0 && (millis() - btLastRecvMs < BLE_SCALE_DATA_VALID_MS))
        return btCurrentWeight;
    // 2. 本机没连, 但其他屏连上了 → 返回其他屏共享的重量
    if (EspnowMesh_RemoteBleActive())
        return EspnowMesh_RemoteBleWeight();
    // 3. 都没有 → 回退
    return fallbackWeight;
}

inline bool     BleScale_IsConnected() { return btConnected && btLastRecvMs > 0 && (millis() - btLastRecvMs < BLE_SCALE_DATA_VALID_MS); }
inline float    BleScale_GetWeight()   { return btCurrentWeight; }
inline uint32_t BleScale_GetOkCount()  { return btOkCount; }
inline void     BleScale_SetEnabled(bool e) { btEnabled = e; }
inline bool     BleScale_IsEnabled()   { return btEnabled; }
