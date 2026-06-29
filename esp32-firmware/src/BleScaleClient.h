#pragma once

// ============================================================
//  BLE Client 称重读取 — 连接 I6328A-485 透传模块, 读 A33E 净重
//
//  链路: A33E表头(9600) ──RS485──> I6328A-485(BLE从机)
//                                    ══BLE══ ESP32(BLE主机)
//
//  关键设计: BLE 扫描/连接是阻塞操作(数秒级), 若在主循环里跑,
//  会卡死 ESP-NOW 心跳(2秒一次)导致其他屏判定本机离线。
//  所以 BLE 全部逻辑放在独立 FreeRTOS 任务里, 主循环只读取
//  共享的最新重量值, UI/ESP-NOW 完全不受 BLE 阻塞影响。
//
//  读取到的真实净重会通过 main.cpp → Display_SetCurrentWeight
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
// 六台屏竞争连接同一个模块；所有固件必须配置成同一个模块MAC。
#ifndef BLE_SCALE_DEVICE_MAC
#define BLE_SCALE_DEVICE_MAC "39:02:01:D0:5A:50"
#endif

// ---------- I6328A GATT UUID ----------
static BLEUUID BLE_SCALE_SERVICE_UUID("0000ffe0-0000-1000-8000-00805f9b34fb");
static BLEUUID BLE_SCALE_WRITE_UUID  ("0000ffe1-0000-1000-8000-00805f9b34fb");
static BLEUUID BLE_SCALE_NOTIFY_UUID ("0000ffe2-0000-1000-8000-00805f9b34fb");

// ---------- 节奏 ----------
#ifndef BLE_SCALE_READ_INTERVAL_MS
#define BLE_SCALE_READ_INTERVAL_MS 500    // 500ms读一次，尽快建立/续租有效称重
#endif
#define BLE_SCALE_RESYNC_MS 400           // 残包超时
#define BLE_SCALE_DATA_VALID_MS 5000      // 5秒内有读数视为有效
#define BLE_SCALE_RETRY_DELAY_MS 1000     // 本机失败后的基础重试间隔

// ---------- A33E Modbus-RTU ----------
static const uint8_t  A33E_SLAVE_ID = 1;
static const uint16_t A33E_REG_NET = 6;    // A33E手册：保持寄存器6=净重(float)，8=毛重(float)
static const uint16_t A33E_REG_COUNT = 2;

#ifndef A33E_SWAP_WORDS
#define A33E_SWAP_WORDS 0                 // 现场若确认是CDAB字序再改为1
#endif

// ---------- 状态(任务内部用) ----------
static BLEClient*               btClient = nullptr;
static BLERemoteService*        btService = nullptr;
static BLERemoteCharacteristic* btWrite = nullptr;
static BLERemoteCharacteristic* btNotify = nullptr;
static bool     btConnected = false;
static bool     btLeaseActive = false;
static uint32_t btConnectedAtMs = 0;
static volatile uint32_t btNextAttemptMs = 0;

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
static portMUX_TYPE btRxMux = portMUX_INITIALIZER_UNLOCKED;

static void btClearRx() {
    portENTER_CRITICAL(&btRxMux);
    btRxlen = 0;
    portEXIT_CRITICAL(&btRxMux);
}

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

// ---------- 构造并发送读净重请求 ----------
static void btSendReadNet() {
    if (!btConnected || !btWrite) return;
    uint8_t cmd[8];
    cmd[0] = A33E_SLAVE_ID;
    cmd[1] = 0x03;
    cmd[2] = (A33E_REG_NET >> 8) & 0xFF;
    cmd[3] = A33E_REG_NET & 0xFF;
    cmd[4] = (A33E_REG_COUNT >> 8) & 0xFF;
    cmd[5] = A33E_REG_COUNT & 0xFF;
    uint16_t crc = btModbusCRC16(cmd, 6);
    cmd[6] = crc & 0xFF;
    cmd[7] = (crc >> 8) & 0xFF;
    btWrite->writeValue(cmd, 8, false);
}

// ---------- 解析一帧 ----------
static bool btTryParseFrame() {
    uint8_t rx[sizeof(btRxbuf)];
    size_t rxlen = 0;
    portENTER_CRITICAL(&btRxMux);
    rxlen = btRxlen;
    memcpy(rx, btRxbuf, rxlen);
    portEXIT_CRITICAL(&btRxMux);

    if (rxlen < 5) return false;

    // Modbus异常响应：站号 + (功能码|0x80) + 异常码 + CRC
    for (size_t i = 0; i + 5 <= rxlen; ++i) {
        if (rx[i] == A33E_SLAVE_ID && rx[i + 1] == (0x03 | 0x80)) {
            uint16_t crcCalc = btModbusCRC16(&rx[i], 3);
            uint16_t crcRcvd = rx[i + 3] | (rx[i + 4] << 8);
            if (crcCalc == crcRcvd) {
                btErrCount++;
                Serial.printf("[BleScale] Modbus异常码=0x%02X\n", rx[i + 2]);
                btClearRx();
            }
            return false;
        }
    }

    if (rxlen < 9) return false;
    int start = -1;
    for (size_t i = 0; i + 9 <= rxlen; ++i) {
        if (rx[i] == A33E_SLAVE_ID && rx[i+1] == 0x03 && rx[i+2] == 0x04) {
            start = (int)i; break;
        }
    }
    if (start < 0) return false;
    uint8_t* f = &rx[start];
    uint16_t crcCalc = btModbusCRC16(f, 7);
    uint16_t crcRcvd = f[7] | (f[8] << 8);
    if (crcRcvd != crcCalc) {
        btErrCount++;
        btClearRx();
        return false;
    }
#if A33E_SWAP_WORDS
    uint32_t raw = ((uint32_t)f[5] << 24) | ((uint32_t)f[6] << 16) |
                   ((uint32_t)f[3] << 8) | (uint32_t)f[4];
#else
    uint32_t raw = ((uint32_t)f[3] << 24) | ((uint32_t)f[4] << 16) |
                   ((uint32_t)f[5] << 8) | (uint32_t)f[6];
#endif
    float val;
    memcpy(&val, &raw, 4);
    if (isnan(val) || isinf(val) || fabsf(val) > 1000000000.0f) {
        btErrCount++;
        btClearRx();
        return false;
    }
    btCurrentWeight = val;
    btLastRecvMs = millis();
    btOkCount++;
    return true;
}

// ---------- Notify 回调(BLE 栈上下文, 只填缓冲) ----------
static void btNotifyCallback(BLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify) {
    portENTER_CRITICAL(&btRxMux);
    for (size_t i = 0; i < len && btRxlen < sizeof(btRxbuf); ++i)
        btRxbuf[btRxlen++] = data[i];
    btRxLastMs = millis();
    portEXIT_CRITICAL(&btRxMux);
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
    btConnectedAtMs = millis();
    btLastRecvMs = 0;
    btClearRx();
    Serial.printf("[BleScale] ✅ 已连接 %s\n", BLE_SCALE_DEVICE_NAME);
    return true;
}

static void btDisconnectAndRelease(const char* reason) {
    if (btClient && btClient->isConnected()) btClient->disconnect();
    btConnected = false;
    btConnectedAtMs = 0;
    btLastRecvMs = 0;
    btWrite = btNotify = nullptr;
    btService = nullptr;
    btClearRx();
    if (btLeaseActive) {
        btLeaseActive = false;
        EspnowMesh_SetLocalBleConnected(false);
    }
    if (reason) Serial.printf("[BleScale] %s\n", reason);
}

static uint32_t btElectionDelayMs() {
    uint8_t binId = myBinIdForBle;
    if (binId < 1 || binId > BIN_COUNT) binId = BIN_COUNT;
    uint64_t chipId = ESP.getEfuseMac();
    uint32_t jitter = (uint32_t)(chipId ^ (chipId >> 32)) % 350U;
    return 500U + (uint32_t)(binId - 1U) * 900U + jitter;
}

// ============================================================
//  BLE 独立任务 — 所有阻塞操作都在这里, 不碰主循环
// ============================================================
static void bleScaleTask(void* arg) {
    uint32_t lastSend = 0;
    Serial.printf("[BleScale] 本机仓号=%d, 已加入BLE自动接管竞选\n", myBinIdForBle);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(50));

        if (!btEnabled) {
            if (btConnected || btLeaseActive) btDisconnectAndRelease("BLE已禁用，释放称重租约");
            btNextAttemptMs = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        uint32_t now = millis();

        // 已有有效远程租约：未拿到数据的本机必须让出；若双方同时成功则按仓号+MAC裁决。
        if (EspnowMesh_RemoteBleActive()) {
            bool localDataValid = btLeaseActive && btLastRecvMs > 0 &&
                                  (now - btLastRecvMs < BLE_SCALE_DATA_VALID_MS);
            if (!localDataValid || EspnowMesh_RemoteBleWinsTie()) {
                if (btConnected || btLeaseActive)
                    btDisconnectAndRelease("其他屏持有有效称重租约，本机让出");
                btNextAttemptMs = 0;
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }

        // 无远程租约时所有设备都参与；仓号+芯片ID错峰，先成功者用心跳阻止后续抢连。
        if (!btConnected) {
            if (btNextAttemptMs == 0) {
                uint32_t delayMs = btElectionDelayMs();
                btNextAttemptMs = now + delayMs;
                Serial.printf("[BleScale] 无有效租约，%ums后尝试接管\n", delayMs);
            }
            if ((int32_t)(now - btNextAttemptMs) < 0) continue;
            btNextAttemptMs = 0;
            Serial.println("[BleScale] 尝试连接称重模块(MAC直连)...");
            btWrite = btNotify = nullptr;
            btService = nullptr;
            btClearRx();
            btConnectByAddr(BLEAddress(BLE_SCALE_DEVICE_MAC));
            if (btConnected) {
                lastSend = 0;
                Serial.println("[BleScale] GATT已连接，等待首个有效净重后再广播占用");
            } else {
                btNextAttemptMs = millis() + BLE_SCALE_RETRY_DELAY_MS + btElectionDelayMs();
                Serial.println("[BleScale] 连接失败，进入错峰重试");
            }
            continue;
        }

        // ---- 连接丢失检测 ----
        if (!btClient || !btClient->isConnected()) {
            btDisconnectAndRelease("BLE物理连接丢失，已广播释放");
            btNextAttemptMs = 0;
            continue;
        }

        now = millis();
        if (btRxlen > 0 && now - btRxLastMs > BLE_SCALE_RESYNC_MS) btClearRx();

        bool noFirstData = btLastRecvMs == 0 && now - btConnectedAtMs >= BLE_SCALE_DATA_VALID_MS;
        bool dataExpired = btLastRecvMs > 0 && now - btLastRecvMs >= BLE_SCALE_DATA_VALID_MS;
        if (noFirstData || dataExpired) {
            btDisconnectAndRelease(noFirstData ?
                "连接后始终未收到A33E净重，释放并重试" :
                "A33E净重数据超时，释放并重试");
            btNextAttemptMs = 0;
            continue;
        }

        if (now - lastSend >= BLE_SCALE_READ_INTERVAL_MS) {
            lastSend = now;
            btClearRx();
            btSendReadNet();
        }
        if (btTryParseFrame()) {
            btClearRx();
            if (!btLeaseActive) {
                btLeaseActive = true;
                EspnowMesh_SetLocalBleConnected(true);
                Serial.println("[BleScale] ✅ 首个有效净重已收到，广播称重租约");
            }
            uint32_t ok = btOkCount;
            float w = btCurrentWeight;
            if (ok == 1 || ok % 10 == 0)
                Serial.printf("[BleScale] 净重=%.3f kg (第%u次)\n", w, ok);
        }
    }
}

// ============================================================
//  对外接口
// ============================================================
inline void BleScale_SetMyBinId(uint8_t binId) {
    myBinIdForBle = binId;
    btNextAttemptMs = 0;  // 仓号变化后重新计算错峰优先级
}

inline void BleScale_Init() {
    Serial.printf("[BleScale] 启动 BLE 任务, 目标: %s [%s]\n",
                  BLE_SCALE_DEVICE_NAME, BLE_SCALE_DEVICE_MAC);
    BLEDevice::init("A33E-Reader");
    // 独立任务, 栈8K(BLE需要大栈), 固定到核心1(让核心0跑WiFi/ESP-NOW)
    xTaskCreatePinnedToCore(bleScaleTask, "bleScale", 8192, nullptr, 1, nullptr, 1);
}

// 主循环调用: 返回最新净重。fallback=无有效读数时的安全回退值
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
