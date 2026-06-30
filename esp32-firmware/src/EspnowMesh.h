#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "Config.h"

// 一主多从通信：
// - 只有被选中的主机连接 BLE，并每 2 秒广播一次净重。
// - 从机收到重量包后按仓号错峰回复 ACK。
// - 主机根据 6 秒内的 ACK 生成六仓在线位图，再随重量包广播给所有从机。
// - 重量包本身就是主机心跳，不再维护另一套心跳协议。

static constexpr uint8_t ESPNOW_CHANNEL = 6;
static constexpr uint8_t ESPNOW_BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static constexpr uint32_t WEIGHT_BROADCAST_INTERVAL_MS = 2000;
static constexpr uint32_t DEVICE_OFFLINE_TIMEOUT_MS = 6000;
static constexpr uint32_t MASTER_OFFLINE_TIMEOUT_MS = 6000;

static constexpr uint32_t MASTER_WEIGHT_MAGIC = 0xA33E1001;
static constexpr uint32_t FOLLOWER_ACK_MAGIC = 0xA33E1002;
static constexpr uint32_t BIN_WEIGHT_SYNC_MAGIC = 0xA33E0002;
static constexpr uint32_t MASTER_SELECT_MAGIC = 0xA33E1003;
static constexpr uint8_t ESPNOW_PROTOCOL_VERSION = 2;

#pragma pack(push, 1)
struct MasterWeightPacket {
    uint32_t magic;
    uint8_t version;
    uint8_t masterBin;
    uint8_t netValid;
    uint8_t onlineMask;
    float netWeight;
    uint32_t seq;
    uint32_t masterEpoch;
};

struct FollowerAckPacket {
    uint32_t magic;
    uint8_t version;
    uint8_t binId;
    uint8_t masterBin;
    uint8_t reserved;
    uint32_t weightSeq;
    uint32_t masterEpoch;
};

struct BinWeightSyncPacket {
    uint32_t magic;
    uint8_t binId;
    float weight;
};

struct MasterSelectPacket {
    uint32_t magic;
    uint8_t version;
    uint8_t masterBin;
    uint8_t sourceBin;
    uint8_t reserved;
    uint32_t masterEpoch;
};
#pragma pack(pop)

struct BinState {
    bool online;
    uint32_t lastHeardMs;
    float binWeight;
    float currentWeight;
};

inline BinState binStates[BIN_COUNT] = {};
inline uint8_t myBinId = DEFAULT_LOCAL_ID;
inline uint8_t selectedMasterBin = 3;
inline uint32_t selectedMasterEpoch = 1;
inline uint8_t myMac[6] = {};
inline bool meshInitialized = false;

inline volatile bool localNetValid = false;
inline volatile float localNetWeight = 0.0f;
inline volatile bool remoteNetValid = false;
inline volatile float remoteNetWeight = 0.0f;
inline volatile uint32_t lastMasterFrameMs = 0;
inline volatile uint32_t lastMasterSeq = 0;
inline volatile uint8_t lastOnlineMask = 0;

inline uint32_t lastWeightBroadcastMs = 0;
inline uint32_t weightSeq = 0;
inline uint32_t meshStartedMs = 0;
inline uint32_t followerLastAckMs[BIN_COUNT] = {};
inline bool silenceState = false;

inline volatile bool ackPending = false;
inline volatile uint32_t ackDueMs = 0;
inline volatile uint32_t ackWeightSeq = 0;
inline volatile uint8_t ackRepeatsRemaining = 0;
inline volatile bool masterSelectionPending = false;
inline volatile uint8_t pendingMasterBin = 0;
inline volatile uint32_t pendingMasterEpoch = 0;
inline volatile bool onlineMaskPending = false;
inline volatile uint8_t pendingOnlineMask = 0;
inline volatile float pendingOnlineWeight = 0.0f;
inline volatile bool binWeightSyncPending = false;
inline volatile uint8_t pendingSyncBin = 0;
inline volatile float pendingSyncWeight = 0.0f;
inline portMUX_TYPE meshMux = portMUX_INITIALIZER_UNLOCKED;

typedef void (*OnBinStateChangeCb)(uint8_t binId, bool online, float binWeight, float currentWeight);
typedef void (*OnBinWeightSyncCb)(uint8_t binId, float weight);
typedef void (*OnSilenceCb)(bool silent);
typedef void (*OnRemoteBleCb)(bool remoteBleConnected, float weight);
typedef void (*OnMasterSelectionCb)(uint8_t masterBin, uint32_t masterEpoch);

inline OnBinStateChangeCb gOnBinStateCb = nullptr;
inline OnBinWeightSyncCb gOnBinWeightSyncCb = nullptr;
inline OnSilenceCb gOnSilenceCb = nullptr;
inline OnRemoteBleCb gOnRemoteBleCb = nullptr;
inline OnMasterSelectionCb gOnMasterSelectionCb = nullptr;

inline bool EspnowMesh_IsMaster() {
    return myBinId == selectedMasterBin;
}

inline void EspnowMesh_SetStateCallback(OnBinStateChangeCb cb) { gOnBinStateCb = cb; }
inline void EspnowMesh_SetWeightSyncCallback(OnBinWeightSyncCb cb) { gOnBinWeightSyncCb = cb; }
inline void EspnowMesh_SetSilenceCallback(OnSilenceCb cb) { gOnSilenceCb = cb; }
inline void EspnowMesh_SetRemoteBleCallback(OnRemoteBleCb cb) { gOnRemoteBleCb = cb; }
inline void EspnowMesh_SetMasterSelectionCallback(OnMasterSelectionCb cb) { gOnMasterSelectionCb = cb; }

inline void EspnowMesh_SetMyBin(uint8_t binId) {
    if (binId >= 1 && binId <= BIN_COUNT) myBinId = binId;
}

inline void EspnowMesh_SetGateway(bool) {}

inline void EspnowMesh_SetMasterSelection(uint8_t masterBin, uint32_t masterEpoch) {
    if (masterBin < 1 || masterBin > BIN_COUNT) masterBin = 3;
    selectedMasterBin = masterBin;
    selectedMasterEpoch = masterEpoch == 0 ? 1 : masterEpoch;
}

inline uint8_t EspnowMesh_GetMasterBin() { return selectedMasterBin; }
inline uint32_t EspnowMesh_GetMasterEpoch() { return selectedMasterEpoch; }

inline bool EspnowMesh_RemoteBleActive() {
    uint32_t last = lastMasterFrameMs;
    return !EspnowMesh_IsMaster() && remoteNetValid && last > 0 &&
           millis() - last < MASTER_OFFLINE_TIMEOUT_MS;
}

inline float EspnowMesh_RemoteBleWeight() {
    return EspnowMesh_RemoteBleActive() ? remoteNetWeight : 0.0f;
}

inline bool EspnowMesh_RemoteBleWinsTie() { return !EspnowMesh_IsMaster(); }

inline void EspnowMesh_SetLocalBleConnected(bool connected) {
    localNetValid = connected;
    if (!connected) localNetWeight = 0.0f;
}

inline uint8_t EspnowMesh_BuildOnlineMask(uint32_t now) {
    uint8_t mask = 0;
    if (selectedMasterBin >= 1 && selectedMasterBin <= BIN_COUNT)
        mask |= static_cast<uint8_t>(1U << (selectedMasterBin - 1));
    for (uint8_t i = 0; i < BIN_COUNT; ++i) {
        uint8_t binId = i + 1;
        if (binId == selectedMasterBin) continue;
        if (followerLastAckMs[i] > 0 && now - followerLastAckMs[i] < DEVICE_OFFLINE_TIMEOUT_MS)
            mask |= static_cast<uint8_t>(1U << i);
    }
    return mask;
}

inline void EspnowMesh_ApplyOnlineMask(uint8_t mask, float currentWeight) {
    for (uint8_t i = 0; i < BIN_COUNT; ++i) {
        bool next = (mask & (1U << i)) != 0;
        bool changed = binStates[i].online != next;
        binStates[i].online = next;
        if (next) binStates[i].lastHeardMs = millis();
        binStates[i].currentWeight = currentWeight;
        if (changed && gOnBinStateCb)
            gOnBinStateCb(i + 1, next, binStates[i].binWeight, currentWeight);
    }
    lastOnlineMask = mask;
}

inline void EspnowMesh_BroadcastHeartbeat(float binWeight, float currentWeight) {
    if (!meshInitialized || !EspnowMesh_IsMaster()) return;
    uint32_t now = millis();
    uint8_t mask = EspnowMesh_BuildOnlineMask(now);
    MasterWeightPacket pkt{};
    pkt.magic = MASTER_WEIGHT_MAGIC;
    pkt.version = ESPNOW_PROTOCOL_VERSION;
    pkt.masterBin = selectedMasterBin;
    pkt.netValid = localNetValid ? 1 : 0;
    pkt.onlineMask = mask;
    pkt.netWeight = pkt.netValid ? currentWeight : 0.0f;
    pkt.seq = ++weightSeq;
    pkt.masterEpoch = selectedMasterEpoch;
    esp_now_send(ESPNOW_BROADCAST_MAC, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
    binStates[myBinId - 1].binWeight = binWeight;
    EspnowMesh_ApplyOnlineMask(mask, pkt.netWeight);
    if (pkt.seq == 1 || pkt.seq % 5 == 0) {
        Serial.printf("[ESPNOW] 主机仓%d广播: seq=%u 净重=%.3f valid=%u onlineMask=0x%02X\n",
                      pkt.masterBin, pkt.seq, pkt.netWeight, pkt.netValid, pkt.onlineMask);
    }
}

inline void EspnowMesh_AnnounceOnline(float binWeight, float currentWeight, uint8_t repeats = 3, uint16_t gapMs = 30) {
    if (!EspnowMesh_IsMaster()) return;
    for (uint8_t i = 0; i < repeats; ++i) {
        EspnowMesh_BroadcastHeartbeat(binWeight, currentWeight);
        if (i + 1 < repeats) delay(gapMs);
    }
    lastWeightBroadcastMs = millis();
}

inline void EspnowMesh_BroadcastBinWeightSync(uint8_t binId, float weight) {
    if (!meshInitialized || binId < 1 || binId > BIN_COUNT) return;
    BinWeightSyncPacket pkt{BIN_WEIGHT_SYNC_MAGIC, binId, weight};
    esp_now_send(ESPNOW_BROADCAST_MAC, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
    Serial.printf("[ESPNOW] 仓重同步广播: 仓%d → %.1f kg\n", binId, weight);
}

inline void EspnowMesh_BroadcastMasterSelection(uint8_t masterBin, uint32_t masterEpoch) {
    if (!meshInitialized || masterBin < 1 || masterBin > BIN_COUNT) return;
    MasterSelectPacket pkt{};
    pkt.magic = MASTER_SELECT_MAGIC;
    pkt.version = ESPNOW_PROTOCOL_VERSION;
    pkt.masterBin = masterBin;
    pkt.sourceBin = myBinId;
    pkt.masterEpoch = masterEpoch;
    for (uint8_t i = 0; i < 5; ++i) {
        esp_now_send(ESPNOW_BROADCAST_MAC, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
        delay(35);
    }
    portENTER_CRITICAL(&meshMux);
    pendingMasterBin = masterBin;
    pendingMasterEpoch = masterEpoch;
    masterSelectionPending = true;
    portEXIT_CRITICAL(&meshMux);
}

inline void EspnowMesh_OnBinChanged(uint8_t oldBinId, uint8_t newBinId, float newBinWeight, float currentWeight) {
    (void)oldBinId;
    (void)currentWeight;
    EspnowMesh_SetMyBin(newBinId);
    if (newBinId >= 1 && newBinId <= BIN_COUNT) binStates[newBinId - 1].binWeight = newBinWeight;
}

inline void EspnowMesh_QueueMasterSelection(uint8_t masterBin, uint32_t masterEpoch) {
    if (masterBin < 1 || masterBin > BIN_COUNT) return;
    if (masterEpoch < selectedMasterEpoch) return;
    if (masterEpoch == selectedMasterEpoch && masterBin == selectedMasterBin) return;
    portENTER_CRITICAL(&meshMux);
    pendingMasterBin = masterBin;
    pendingMasterEpoch = masterEpoch;
    masterSelectionPending = true;
    portEXIT_CRITICAL(&meshMux);
}

inline void onEspNowRecv(const uint8_t* macAddr, const uint8_t* data, int len) {
    if (!data || len < static_cast<int>(sizeof(uint32_t))) return;
    if (memcmp(macAddr, myMac, 6) == 0) return;
    uint32_t magic = 0;
    memcpy(&magic, data, sizeof(magic));

    if (magic == MASTER_WEIGHT_MAGIC && len == static_cast<int>(sizeof(MasterWeightPacket))) {
        MasterWeightPacket pkt{};
        memcpy(&pkt, data, sizeof(pkt));
        if (pkt.version != ESPNOW_PROTOCOL_VERSION || pkt.masterBin < 1 || pkt.masterBin > BIN_COUNT) return;
        if (pkt.masterEpoch < selectedMasterEpoch) return;
        if (pkt.masterEpoch > selectedMasterEpoch || pkt.masterBin != selectedMasterBin) {
            EspnowMesh_QueueMasterSelection(pkt.masterBin, pkt.masterEpoch);
            return;
        }
        if (EspnowMesh_IsMaster()) return;

        lastMasterFrameMs = millis();
        lastMasterSeq = pkt.seq;
        remoteNetValid = pkt.netValid != 0;
        remoteNetWeight = remoteNetValid ? pkt.netWeight : 0.0f;

        // 本机刚收到主机数据，自己的灯可立即点亮；主机将在收到 ACK 后同步给其他设备。
        uint8_t visibleMask = pkt.onlineMask | static_cast<uint8_t>(1U << (myBinId - 1));
        portENTER_CRITICAL(&meshMux);
        pendingOnlineMask = visibleMask;
        pendingOnlineWeight = remoteNetWeight;
        onlineMaskPending = true;
        ackWeightSeq = pkt.seq;
        ackDueMs = millis() + 25U + static_cast<uint32_t>(myBinId - 1U) * 35U;
        ackRepeatsRemaining = 2;
        ackPending = true;
        portEXIT_CRITICAL(&meshMux);
        return;
    }

    if (magic == FOLLOWER_ACK_MAGIC && len == static_cast<int>(sizeof(FollowerAckPacket))) {
        if (!EspnowMesh_IsMaster()) return;
        FollowerAckPacket pkt{};
        memcpy(&pkt, data, sizeof(pkt));
        if (pkt.version != ESPNOW_PROTOCOL_VERSION || pkt.masterBin != selectedMasterBin ||
            pkt.masterEpoch != selectedMasterEpoch || pkt.binId < 1 || pkt.binId > BIN_COUNT ||
            pkt.binId == selectedMasterBin) return;
        bool firstAck = followerLastAckMs[pkt.binId - 1] == 0;
        followerLastAckMs[pkt.binId - 1] = millis();
        if (firstAck) Serial.printf("[ESPNOW] 收到仓%d从机ACK\n", pkt.binId);
        return;
    }

    if (magic == BIN_WEIGHT_SYNC_MAGIC && len == static_cast<int>(sizeof(BinWeightSyncPacket))) {
        BinWeightSyncPacket pkt{};
        memcpy(&pkt, data, sizeof(pkt));
        if (pkt.binId < 1 || pkt.binId > BIN_COUNT) return;
        portENTER_CRITICAL(&meshMux);
        pendingSyncBin = pkt.binId;
        pendingSyncWeight = pkt.weight;
        binWeightSyncPending = true;
        portEXIT_CRITICAL(&meshMux);
        return;
    }

    if (magic == MASTER_SELECT_MAGIC && len == static_cast<int>(sizeof(MasterSelectPacket))) {
        MasterSelectPacket pkt{};
        memcpy(&pkt, data, sizeof(pkt));
        if (pkt.version != ESPNOW_PROTOCOL_VERSION) return;
        EspnowMesh_QueueMasterSelection(pkt.masterBin, pkt.masterEpoch);
    }
}

inline void onEspNowSend(const uint8_t*, esp_now_send_status_t) {}

inline bool EspnowMesh_Init() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    WiFi.macAddress(myMac);

    if (esp_now_init() != ESP_OK) return false;
    esp_now_register_send_cb(onEspNowSend);
    esp_now_register_recv_cb(onEspNowRecv);

    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, ESPNOW_BROADCAST_MAC, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(ESPNOW_BROADCAST_MAC) && esp_now_add_peer(&peer) != ESP_OK) return false;

    meshInitialized = true;
    meshStartedMs = millis();
    if (EspnowMesh_IsMaster()) {
        followerLastAckMs[myBinId - 1] = meshStartedMs;
        binStates[myBinId - 1].online = true;
        binStates[myBinId - 1].lastHeardMs = meshStartedMs;
        // 主机自己立即上线: 显式触发一次回调, 点亮本机对应的状态灯。
        // (后续 ApplyOnlineMask 因状态未变化不会回调, 这里补一次。)
        if (gOnBinStateCb)
            gOnBinStateCb(myBinId, true, binStates[myBinId - 1].binWeight, 0.0f);
    }
    Serial.printf("[ESPNOW] 初始化完成: 本机仓%d, 主机仓%d, epoch=%u, channel=%u\n",
                  myBinId, selectedMasterBin, selectedMasterEpoch, ESPNOW_CHANNEL);
    return true;
}

inline void EspnowMesh_ProcessMasterSelection() {
    if (!masterSelectionPending) return;
    uint8_t masterBin;
    uint32_t epoch;
    portENTER_CRITICAL(&meshMux);
    masterBin = pendingMasterBin;
    epoch = pendingMasterEpoch;
    masterSelectionPending = false;
    portEXIT_CRITICAL(&meshMux);
    if (masterBin < 1 || masterBin > BIN_COUNT || epoch < selectedMasterEpoch) return;
    if (masterBin == selectedMasterBin && epoch == selectedMasterEpoch) return;
    selectedMasterBin = masterBin;
    selectedMasterEpoch = epoch;
    Serial.printf("[ESPNOW] 主机配置更新: 仓%d, epoch=%u\n", masterBin, epoch);
    if (gOnMasterSelectionCb) gOnMasterSelectionCb(masterBin, epoch);
}

inline void EspnowMesh_ProcessPendingUi() {
    bool hasMask;
    uint8_t mask;
    float weight;
    bool hasSync;
    uint8_t syncBin;
    float syncWeight;
    portENTER_CRITICAL(&meshMux);
    hasMask = onlineMaskPending;
    mask = pendingOnlineMask;
    weight = pendingOnlineWeight;
    onlineMaskPending = false;
    hasSync = binWeightSyncPending;
    syncBin = pendingSyncBin;
    syncWeight = pendingSyncWeight;
    binWeightSyncPending = false;
    portEXIT_CRITICAL(&meshMux);
    if (hasMask) EspnowMesh_ApplyOnlineMask(mask, weight);
    if (hasSync && syncBin >= 1 && syncBin <= BIN_COUNT) {
        binStates[syncBin - 1].binWeight = syncWeight;
        if (gOnBinWeightSyncCb) gOnBinWeightSyncCb(syncBin, syncWeight);
    }
}

inline void EspnowMesh_SendPendingAck(uint32_t now) {
    if (!ackPending || static_cast<int32_t>(now - ackDueMs) < 0) return;
    uint32_t seq;
    uint8_t repeatsLeft;
    portENTER_CRITICAL(&meshMux);
    seq = ackWeightSeq;
    repeatsLeft = ackRepeatsRemaining;
    if (repeatsLeft > 1) {
        ackRepeatsRemaining = repeatsLeft - 1;
        ackDueMs = now + 240U;
        ackPending = true;
    } else {
        ackRepeatsRemaining = 0;
        ackPending = false;
    }
    portEXIT_CRITICAL(&meshMux);
    FollowerAckPacket pkt{};
    pkt.magic = FOLLOWER_ACK_MAGIC;
    pkt.version = ESPNOW_PROTOCOL_VERSION;
    pkt.binId = myBinId;
    pkt.masterBin = selectedMasterBin;
    pkt.weightSeq = seq;
    pkt.masterEpoch = selectedMasterEpoch;
    esp_now_send(ESPNOW_BROADCAST_MAC, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
}

inline void EspnowMesh_Loop(float myBinWeight, float myCurrentWeight) {
    if (!meshInitialized) return;
    uint32_t now = millis();
    EspnowMesh_ProcessMasterSelection();
    EspnowMesh_ProcessPendingUi();

    if (EspnowMesh_IsMaster()) {
        localNetWeight = localNetValid ? myCurrentWeight : 0.0f;
        if (lastWeightBroadcastMs == 0 || now - lastWeightBroadcastMs >= WEIGHT_BROADCAST_INTERVAL_MS) {
            lastWeightBroadcastMs = now;
            EspnowMesh_BroadcastHeartbeat(myBinWeight, localNetWeight);
        }
        if (silenceState) {
            silenceState = false;
            if (gOnSilenceCb) gOnSilenceCb(false);
        }
    } else {
        EspnowMesh_SendPendingAck(now);
        bool silent = lastMasterFrameMs == 0
            ? now - meshStartedMs >= MASTER_OFFLINE_TIMEOUT_MS
            : now - lastMasterFrameMs >= MASTER_OFFLINE_TIMEOUT_MS;
        if (silent != silenceState) {
            silenceState = silent;
            if (silent) {
                remoteNetValid = false;
                remoteNetWeight = 0.0f;
                EspnowMesh_ApplyOnlineMask(0, 0.0f);
            }
            if (gOnSilenceCb) gOnSilenceCb(silent);
        }
    }
}

inline bool EspnowMesh_IsBinOnline(uint8_t binId) {
    return binId >= 1 && binId <= BIN_COUNT && binStates[binId - 1].online;
}

inline float EspnowMesh_GetBinWeight(uint8_t binId) {
    return (binId >= 1 && binId <= BIN_COUNT) ? binStates[binId - 1].binWeight : 0.0f;
}
