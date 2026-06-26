#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <cstring>
#include "Config.h"

// ============================================================
//  ESP-NOW 组网 (6台屏幕互相广播心跳, 更新在线状态)
//  不依赖WiFi路由器, 同信道(ch6)直接设备对设备
// ============================================================

// 广播MAC: 发给所有同信道设备
static constexpr uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static constexpr uint8_t ESPNOW_CHANNEL = 6;
static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 2000;   // 每2秒广播一次心跳
static constexpr uint32_t OFFLINE_TIMEOUT_MS = 10000;     // 10秒没收到=离线

// 心跳包结构 (固定24字节, 便于广播)
#pragma pack(push, 1)
struct HeartbeatPacket {
    uint32_t magic;          // 魔数 0xA33E0001, 用于识别本协议包
    uint8_t  binId;          // 仓号 1-6 (发送方的仓号)
    uint8_t  role;           // 角色: 0=普通节点, 1=网关
    uint8_t  reserved;       // 保留
    uint8_t  online;         // 发送方自报在线状态 (1=在线)
    float    binWeight;      // 发送方本机仓重
    float    currentWeight;  // 发送方当前称重
    uint32_t seq;            // 序列号(递增)
    uint8_t  mac[6];         // 发送方MAC (冗余, 便于调试)
};
#pragma pack(pop)

static constexpr uint32_t HEARTBEAT_MAGIC = 0xA33E0001;

// 每仓的在线状态(收到的心跳信息)
struct BinState {
    bool     online;         // 是否在线
    uint32_t lastHeardMs;    // 最后一次收到心跳的时间
    float    binWeight;      // 该仓的仓重
    float    currentWeight;  // 该仓的当前称重
    uint8_t  mac[6];         // 该仓的MAC
};

inline BinState binStates[BIN_COUNT] = {};
inline uint32_t hbSeq = 0;
inline uint32_t lastHeartbeatSentMs = 0;
inline uint8_t  myMac[6] = {0};

// 本机信息 (由 main 设置)
inline uint8_t myBinId = 1;        // 本机仓号 1-6
inline bool   myIsGateway = true;  // 本机是否网关

// ===== 回调: 供 Display 调用, 收到某仓心跳/某仓离线时更新UI =====
typedef void (*OnBinStateChangeCb)(uint8_t binId, bool online, float binWeight, float currentWeight);
inline OnBinStateChangeCb gOnBinStateCb = nullptr;

inline void EspnowMesh_SetStateCallback(OnBinStateChangeCb cb) {
    gOnBinStateCb = cb;
}

inline void EspnowMesh_SetMyBin(uint8_t binId) {
    myBinId = binId;
}

inline void EspnowMesh_SetGateway(bool isGw) {
    myIsGateway = isGw;
}

// ===== 本机换仓: 旧仓下线, 新仓上线(通知Display更新灯) =====
inline void EspnowMesh_OnBinChanged(uint8_t oldBinId, uint8_t newBinId) {
    // 旧仓标记离线
    if (oldBinId >= 1 && oldBinId <= BIN_COUNT && oldBinId != newBinId) {
        binStates[oldBinId - 1].online = false;
        if (gOnBinStateCb) gOnBinStateCb(oldBinId, false, 0, 0);
    }
    // 新仓标记在线
    myBinId = newBinId;
    if (newBinId >= 1 && newBinId <= BIN_COUNT) {
        uint8_t idx = newBinId - 1;
        binStates[idx].online = true;
        binStates[idx].lastHeardMs = millis();
        memcpy(binStates[idx].mac, myMac, 6);
        if (gOnBinStateCb) gOnBinStateCb(newBinId, true, 0, 0);
    }
    Serial.printf("[ESPNOW] 本机换仓: %d→%d\n", oldBinId, newBinId);
}

// ===== 广播一个心跳 =====
inline void EspnowMesh_BroadcastHeartbeat(float binWeight, float currentWeight) {
    HeartbeatPacket pkt;
    pkt.magic = HEARTBEAT_MAGIC;
    pkt.binId = myBinId;
    pkt.role = myIsGateway ? 1 : 0;
    pkt.reserved = 0;
    pkt.online = 1;
    pkt.binWeight = binWeight;
    pkt.currentWeight = currentWeight;
    pkt.seq = ++hbSeq;
    memcpy(pkt.mac, myMac, 6);

    esp_err_t result = esp_now_send(BROADCAST_MAC, (const uint8_t*)&pkt, sizeof(pkt));
    if (result != ESP_OK) {
        Serial.printf("[ESPNOW] 发送失败: %s\n", esp_err_to_name(result));
    }
}

// ===== ESP-NOW 接收回调 (Arduino-ESP32 2.x 旧签名) =====
inline void onEspNowRecv(const uint8_t* mac_addr, const uint8_t* data, int len) {
    if (len != (int)sizeof(HeartbeatPacket)) return;
    HeartbeatPacket pkt;
    memcpy(&pkt, data, sizeof(pkt));
    if (pkt.magic != HEARTBEAT_MAGIC) return;
    if (pkt.binId < 1 || pkt.binId > BIN_COUNT) return;

    // 排除自己(自己发的广播也会收到)
    if (memcmp(pkt.mac, myMac, 6) == 0) return;

    uint8_t idx = pkt.binId - 1;  // 0-5

    // 每5秒打印一次心跳摘要(避免刷屏)
    static uint32_t lastRecvLogMs = 0;
    if (millis() - lastRecvLogMs > 5000) {
        lastRecvLogMs = millis();
        Serial.printf("[ESPNOW] 收: 仓%d MAC %02X:%02X:..seq=%u bw=%.1f cw=%.1f\n",
                      pkt.binId, pkt.mac[0], pkt.mac[1], pkt.seq, pkt.binWeight, pkt.currentWeight);
    }

    bool wasOnline = binStates[idx].online;
    binStates[idx].online = true;
    binStates[idx].lastHeardMs = millis();
    binStates[idx].binWeight = pkt.binWeight;
    binStates[idx].currentWeight = pkt.currentWeight;
    memcpy(binStates[idx].mac, pkt.mac, 6);

    // 状态变化或首次收到 → 通知UI
    if (!wasOnline) {
        Serial.printf("[ESPNOW] 仓%d 上线 (MAC %02X:%02X:%02X:%02X:%02X:%02X)\n",
                      pkt.binId, pkt.mac[0], pkt.mac[1], pkt.mac[2],
                      pkt.mac[3], pkt.mac[4], pkt.mac[5]);
        if (gOnBinStateCb) gOnBinStateCb(pkt.binId, true, pkt.binWeight, pkt.currentWeight);
    }
}

// ===== ESP-NOW 发送回调 =====
inline void onEspNowSend(const uint8_t* mac_addr, esp_now_send_status_t status) {
    // 静默, 仅调试时可打开
    // Serial.printf("[ESPNOW] 发送状态: %s\n", status==ESP_NOW_SEND_SUCCESS?"OK":"FAIL");
}

// ===== 初始化 ESP-NOW =====
inline bool EspnowMesh_Init() {
    // WiFi 必须处于 STA 模式 (但不连接路由器), ESP-NOW 才能工作
    WiFi.mode(WIFI_STA);

    // 设置信道为 ESPNOW_CHANNEL (必须所有设备一致)
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    // 获取本机MAC
    WiFi.macAddress(myMac);
    Serial.printf("[ESPNOW] 本机MAC: %02X:%02X:%02X:%02X:%02X:%02X 信道:%d 仓号:%d\n",
                  myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5],
                  ESPNOW_CHANNEL, myBinId);

    // 初始化 ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESPNOW] 初始化失败!");
        return false;
    }

    // 注册发送/接收回调
    esp_now_register_send_cb(onEspNowSend);
    esp_now_register_recv_cb(onEspNowRecv);

    // 添加广播对等体
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.println("[ESPNOW] 添加广播对等体失败!");
        return false;
    }

    // 初始化本机仓状态: 自己在线, 通知Display
    uint8_t myIdx = (myBinId >= 1 && myBinId <= BIN_COUNT) ? myBinId - 1 : 0;
    binStates[myIdx].online = true;
    binStates[myIdx].lastHeardMs = millis();
    binStates[myIdx].binWeight = 0;
    binStates[myIdx].currentWeight = 0;
    memcpy(binStates[myIdx].mac, myMac, 6);
    if (gOnBinStateCb) gOnBinStateCb(myBinId, true, 0, 0);

    Serial.println("[ESPNOW] 初始化完成 (广播模式, 无需路由器)");
    return true;
}

// ===== 主循环调用: 定时发心跳 + 检测离线 =====
inline void EspnowMesh_Loop(float myBinWeight, float myCurrentWeight) {
    uint32_t now = millis();

    // 定时广播心跳 + 日志
    if (now - lastHeartbeatSentMs >= HEARTBEAT_INTERVAL_MS) {
        lastHeartbeatSentMs = now;
        EspnowMesh_BroadcastHeartbeat(myBinWeight, myCurrentWeight);
        static uint32_t lastSendLogMs = 0;
        if (now - lastSendLogMs > 5000) {
            lastSendLogMs = now;
            Serial.printf("[ESPNOW] 发: 仓%d seq=%u bw=%.1f cw=%.1f\n",
                          myBinId, hbSeq, myBinWeight, myCurrentWeight);
        }
    }

    // 检测其他仓离线
    for (uint8_t i = 0; i < BIN_COUNT; ++i) {
        uint8_t binId = i + 1;
        if (binId == myBinId) continue;  // 跳过自己
        if (!binStates[i].online) continue;
        if (now - binStates[i].lastHeardMs > OFFLINE_TIMEOUT_MS) {
            binStates[i].online = false;
            Serial.printf("[ESPNOW] 仓%d 离线 (心跳超时)\n", binId);
            if (gOnBinStateCb) gOnBinStateCb(binId, false, 0, 0);
        }
    }
}

// ===== 查询某仓是否在线 =====
inline bool EspnowMesh_IsBinOnline(uint8_t binId) {
    if (binId < 1 || binId > BIN_COUNT) return false;
    if (binId == myBinId) return true;  // 自己永远在线
    return binStates[binId - 1].online;
}

// ===== 获取某仓仓重 =====
inline float EspnowMesh_GetBinWeight(uint8_t binId) {
    if (binId < 1 || binId > BIN_COUNT) return 0;
    return binStates[binId - 1].binWeight;
}
