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
#include <NimBLEDevice.h>
#include <strings.h>
#include "Config.h"
#include "EspnowMesh.h"   // 查询远程BLE状态, 实现先到先得协调

using BLEDevice = NimBLEDevice;
using BLEClient = NimBLEClient;
using BLEAddress = NimBLEAddress;
using BLEUUID = NimBLEUUID;
using BLERemoteService = NimBLERemoteService;
using BLERemoteCharacteristic = NimBLERemoteCharacteristic;

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
#define BLE_SCALE_READ_INTERVAL_MS 3500   // 降功耗: 每3.5秒读取一次A33E净重
#endif
#define BLE_SCALE_RESYNC_MS 400           // 残包超时
#define BLE_SCALE_DATA_VALID_MS 5000      // 5秒内有读数视为有效
#define BLE_SCALE_RETRY_DELAY_MS 2000     // 给ESP-NOW留出无线时隙，避免密集扫描压住从机ACK
#define BLE_SCALE_SCAN_SECONDS 1          // 1秒扫描窗口，业务主循环始终不阻塞

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

enum BtAsyncScanState : uint8_t {
    BT_SCAN_IDLE = 0,
    BT_SCAN_RUNNING,
    BT_SCAN_FOUND,
    BT_SCAN_COMPLETE,
    BT_SCAN_ERROR
};
static volatile BtAsyncScanState btScanState = BT_SCAN_IDLE;
static uint8_t btTargetBda[6] = {0};
static uint8_t btFoundBda[6] = {0};
static volatile uint8_t btFoundAddrType = BLE_ADDR_PUBLIC;
static portMUX_TYPE btScanMux = portMUX_INITIALIZER_UNLOCKED;


// ---------- 与主循环共享的数据(原子读写, float/uint32 在 ESP32 上原子) ----------
static volatile float    btCurrentWeight = 0.0f;
static volatile uint8_t  myBinIdForBle = 1;   // 本机仓号, 由main设置, 决定是否主动连BLE
static volatile uint32_t btLastRecvMs = 0;
static volatile uint32_t btOkCount = 0;
static volatile uint32_t btErrCount = 0;
static volatile uint32_t btNotifyCount = 0;
static volatile uint32_t btRequestCount = 0;
static volatile bool     btEnabled = true;
static bool              btInitialized = false;

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
    bool sent = btWrite->writeValue(cmd, 8, false);
    uint32_t requests = ++btRequestCount;
    if (!sent || requests == 1 || requests % 10 == 0)
        Serial.printf("[BleScale] Modbus净重请求写入=%d CRC=%02X%02X\n", sent ? 1 : 0, cmd[6], cmd[7]);
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
    btNotifyCount++;
    portENTER_CRITICAL(&btRxMux);
    for (size_t i = 0; i < len && btRxlen < sizeof(btRxbuf); ++i)
        btRxbuf[btRxlen++] = data[i];
    btRxLastMs = millis();
    portEXIT_CRITICAL(&btRxMux);
}

// ---------- 连接 ----------
static bool btConnectByAddr(BLEAddress addr) {
    Serial.printf("[BleScale] 连接诊断: begin heap=%u max=%u\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    if (!btClient) {
        btClient = BLEDevice::createClient();
        btClient->setConnectTimeout(3);  // 超过3秒会压缩ESP-NOW无线时隙，直接失败重试更安全
        Serial.printf("[BleScale] 连接诊断: client-created heap=%u max=%u\n",
                      ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    }
    Serial.println("[BleScale] 连接诊断: gatt-open...");
    bool ok = btClient->connect(addr);
    Serial.printf("[BleScale] 连接诊断: gatt-open返回=%d\n", ok ? 1 : 0);
    if (!ok) {
        btConnected = false;
        if (btClient) {
            if (btClient->isConnected()) btClient->disconnect();
            BLEDevice::deleteClient(btClient);
            btClient = nullptr;
        }
        return false;
    }
    Serial.println("[BleScale] 连接诊断: get-service...");
    btService = btClient->getService(BLE_SCALE_SERVICE_UUID);
    Serial.printf("[BleScale] 连接诊断: get-service返回=%d\n", btService ? 1 : 0);
    if (!btService) { btClient->disconnect(); btConnected = false; return false; }
    Serial.println("[BleScale] 连接诊断: get-characteristics...");
    btWrite  = btService->getCharacteristic(BLE_SCALE_WRITE_UUID);
    btNotify = btService->getCharacteristic(BLE_SCALE_NOTIFY_UUID);
    Serial.printf("[BleScale] 连接诊断: characteristics write=%d notify=%d\n",
                  btWrite ? 1 : 0, btNotify ? 1 : 0);
    if (!btWrite || !btNotify) { btClient->disconnect(); btConnected = false; return false; }
    bool subscribed = btNotify->canNotify() && btNotify->subscribe(true, btNotifyCallback, true);
    Serial.printf("[BleScale] 通知订阅=%d canNotify=%d\n", subscribed ? 1 : 0, btNotify->canNotify() ? 1 : 0);
    if (!subscribed) {
        btClient->disconnect();
        btConnected = false;
        return false;
    }
    btConnected = true;
    btConnectedAtMs = millis();
    btLastRecvMs = 0;
    btClearRx();
    Serial.printf("[BleScale] ✅ 已连接 %s\n", BLE_SCALE_DEVICE_NAME);
    return true;
}

// ---------- NimBLE异步扫描：低内存、带连接超时，不阻塞业务主循环 ----------
class BtAdvertisedCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* device) override {
        NimBLEAddress address = device->getAddress();
        if (strcasecmp(address.toString().c_str(), BLE_SCALE_DEVICE_MAC) == 0) {
            portENTER_CRITICAL(&btScanMux);
            memcpy(btFoundBda, address.getNative(), 6);
            btFoundAddrType = address.getType();
            btScanState = BT_SCAN_FOUND;
            portEXIT_CRITICAL(&btScanMux);
            NimBLEDevice::getScan()->stop();
        }
    }
};
static BtAdvertisedCallbacks btAdvertisedCallbacks;

static void btScanCompleteCallback(NimBLEScanResults results) {
    if (btScanState != BT_SCAN_FOUND) btScanState = BT_SCAN_COMPLETE;
}

static bool btStartAsyncScan() {
    if (btScanState == BT_SCAN_RUNNING) return false;
    btScanState = BT_SCAN_RUNNING;
    bool ok = NimBLEDevice::getScan()->start(BLE_SCALE_SCAN_SECONDS, btScanCompleteCallback, false);
    if (!ok) {
        btScanState = BT_SCAN_ERROR;
        Serial.println("[BleScale] 启动NimBLE异步扫描失败");
        return false;
    }
    Serial.println("[BleScale] 异步扫描已启动，业务循环继续运行");
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
    uint64_t chipId = ESP.getEfuseMac();
    uint32_t jitter = (uint32_t)(chipId ^ (chipId >> 32)) % 250U;
    return 300U + jitter;  // 已由主机配置保证唯一，不再按仓号做多机竞选错峰
}

// ============================================================
//  BLE 独立任务 — 所有阻塞操作都在这里, 不碰主循环
// ============================================================
static void bleScaleTask(void* arg) {
    uint32_t lastSend = 0;
    Serial.printf("[BleScale] 本机仓号=%d, 当前为指定主机\n", myBinIdForBle);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(50));

        if (!btEnabled) {
            if (btConnected || btLeaseActive) btDisconnectAndRelease("BLE已禁用，释放称重租约");
            btNextAttemptMs = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        uint32_t now = millis();

        // 只有开发者模式选中的主机才会启动本任务，因此不再进行多机抢连。
        if (!btConnected) {
            if (btScanState == BT_SCAN_FOUND) {
                uint8_t addrType;
                portENTER_CRITICAL(&btScanMux);
                addrType = btFoundAddrType;
                btScanState = BT_SCAN_IDLE;
                portEXIT_CRITICAL(&btScanMux);
                Serial.printf("[BleScale] 已发现目标，BLE专用任务连接 type=%d\n", (int)addrType);
                // getNative()是NimBLE内部小端字节序，重新用字节构造会把MAC反转。
                // 必须用标准字符串构造，确保实际连接39:02:01:D0:5A:50。
                BLEAddress addr(std::string(BLE_SCALE_DEVICE_MAC), addrType);
                bool ok = btConnectByAddr(addr);
                if (ok && btConnected) {
                    lastSend = 0;
                    Serial.println("[BleScale] GATT已连接，等待首个有效净重后再广播占用");
                } else {
                    btNextAttemptMs = millis() + BLE_SCALE_RETRY_DELAY_MS + btElectionDelayMs();
                    Serial.println("[BleScale] 连接失败，进入错峰重试");
                }
                continue;
            }
            if (btScanState == BT_SCAN_COMPLETE || btScanState == BT_SCAN_ERROR) {
                Serial.println(btScanState == BT_SCAN_COMPLETE ?
                    "[BleScale] 本轮未发现目标，进入错峰重试" :
                    "[BleScale] 异步扫描异常，进入错峰重试");
                btScanState = BT_SCAN_IDLE;
                btNextAttemptMs = now + BLE_SCALE_RETRY_DELAY_MS + btElectionDelayMs();
                continue;
            }
            if (btScanState == BT_SCAN_RUNNING) continue;
            if (btNextAttemptMs == 0) {
                uint32_t delayMs = btElectionDelayMs();
                btNextAttemptMs = now + delayMs;
                Serial.printf("[BleScale] %ums后尝试连接称重模块\n", delayMs);
            }
            if ((int32_t)(now - btNextAttemptMs) < 0) continue;
            btNextAttemptMs = 0;
            Serial.println("[BleScale] 开始非阻塞扫描称重模块...");
            btWrite = btNotify = nullptr;
            btService = nullptr;
            btClearRx();
            if (!btStartAsyncScan()) {
                btNextAttemptMs = millis() + BLE_SCALE_RETRY_DELAY_MS + btElectionDelayMs();
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
    if (btInitialized) return;
    btInitialized = true;
    Serial.printf("[BleScale] 启动 BLE 任务, 目标: %s [%s]\n",
                  BLE_SCALE_DEVICE_NAME, BLE_SCALE_DEVICE_MAC);
    BLEDevice::init("A33E-Reader");
    BLEDevice::setMTU(23);  // I6328A串口透传按默认ATT MTU工作，避免255字节MTU下只写不回
    unsigned int mac[6] = {0};
    if (sscanf(BLE_SCALE_DEVICE_MAC, "%02x:%02x:%02x:%02x:%02x:%02x",
               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
        for (int i = 0; i < 6; ++i) btTargetBda[i] = (uint8_t)mac[i];
    }
    NimBLEScan* scan = BLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&btAdvertisedCallbacks, false);
    scan->setActiveScan(true);
    scan->setInterval(80);
    scan->setWindow(48);
    scan->setMaxResults(10);  // 现场周围BLE设备较多，2条会在看到目标前就丢弃后续广播
    // 独立任务, 栈8K(BLE需要大栈), 固定到核心1(让核心0跑WiFi/ESP-NOW)
    xTaskCreatePinnedToCore(bleScaleTask, "bleScale", 8192, nullptr, 1, nullptr, 1);
}

// 主循环调用: 返回最新净重。fallback=无有效读数时的安全回退值
// 优先级: 本机BLE读数 > 其他屏共享的BLE重量 > 回退值
inline float BleScale_Loop(float fallbackWeight) {
    // 1. 本机连着模块且有近期读数 → 返回本机真实重量
    if (btEnabled && btLastRecvMs > 0 && (millis() - btLastRecvMs < BLE_SCALE_DATA_VALID_MS))
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
