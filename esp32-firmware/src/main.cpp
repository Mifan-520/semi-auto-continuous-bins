#include <Arduino.h>
#include "Config.h"
#include "CloudReport.h"
#include "EspnowMesh.h"
#include "BleScaleClient.h"
#include "Display.h"

static uint32_t lastLvTick = 0;
static bool selfTestDone = false;
static bool bleRoleStarted = false;
static uint32_t roleGraceStartedMs = 0;

// ESP-NOW 收到某仓状态变化 → 通知Display更新对应灯
void onBinStateChange(uint8_t binId, bool online, float binWeight, float currentWeight) {
    Display_OnBinStateChange(binId, online, binWeight, currentWeight);
}

// 事件类型 → 字符串名
static const char* eventTypeName(uint8_t t) {
    switch (t) {
        case BIN_EVENT_LOAD:    return "load";
        case BIN_EVENT_UNLOAD:  return "unload";
        case BIN_EVENT_EDIT:    return "edit";
        case BIN_EVENT_ONLINE:  return "online";
        case BIN_EVENT_OFFLINE: return "offline";
        default:                return "unknown";
    }
}

// 收到任意仓变化事件 → DTU节点(开发者模式勾开的那台)上报JSON。
// DTU身份与仓号/主从完全解耦, 物理接DTU串口的那台勾开即可。
void onBinEvent(const BinEventPacket& p) {
    if (!Display_IsDtuEnabled()) return;  // 非DTU节点不负责上报
    char dBuf[16], nBuf[16];
    dtostrf(p.deltaG, 0, 2, dBuf);
    dtostrf(p.newValue, 0, 2, nBuf);
    char json[128];
    snprintf(json, sizeof(json),
             "{\"reporterBin\":%u,\"sourceBin\":%u,\"eventType\":\"%s\","
             "\"deltaG\":%s,\"newValue\":%s,\"seq\":%lu}",
             p.reporterBin, p.sourceBin, eventTypeName(p.eventType),
             dBuf, nBuf, (unsigned long)p.seq);
    CloudReport_SendEventJson(json);
    Serial.printf("[DTU] 上报: %s\n", json);
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("[A33E] Unified firmware - UI + ESP-NOW mesh + BLE Scale");
    Serial.printf("[A33E] localId=%u gateway=%s fixedGateway=%u\n",
                  DEFAULT_LOCAL_ID,
                  DEFAULT_GATEWAY_FLAG ? "true" : "false",
                  FIXED_GATEWAY_ID);
    Serial.printf("[A33E] DTU placeholder Serial1 TX=%d RX=%d baud=%lu\n",
                  DTU_TX_PIN,
                  DTU_RX_PIN,
                  static_cast<unsigned long>(DTU_BAUD_DEFAULT));

    // 1. 初始化显示
    Display_Init();

    // 2. 初始化 ESP-NOW 组网
    // Display_Init() 已从 NVS 读出本机仓号；ESP-NOW 必须使用同一个仓号，
    // 否则会出现 UI 显示仓5、网络仍广播仓1 的错位。
    const uint8_t localBinId = Display_GetLocalBinId();
    uint8_t networkBinId = localBinId;
#ifdef A33E_TEST_FORCE_LOCAL_BIN
    networkBinId = A33E_TEST_FORCE_LOCAL_BIN;
    Serial.printf("[TEST] 临时网络仓号覆盖: 仓%d (不写入NVS)\n", networkBinId);
#endif
    EspnowMesh_SetMyBin(networkBinId);
    EspnowMesh_SetMasterSelection(Display_GetMasterBinId(), Display_GetMasterEpoch());
    BleScale_SetMyBinId(networkBinId);
    BleScale_SetEnabled(false);  // 先留3秒接收最新主机配置，只有主机才初始化NimBLE

    EspnowMesh_SetGateway(DEFAULT_GATEWAY_FLAG);
    EspnowMesh_SetStateCallback(onBinStateChange);
    EspnowMesh_SetBinEventCallback(onBinEvent);
    EspnowMesh_SetSilenceCallback(Display_OnSilence);
    EspnowMesh_SetMasterSelectionCallback(Display_OnMasterSelection);
    if (!EspnowMesh_Init()) {
        Serial.println("[A33E] ESP-NOW初始化失败, 仅UI运行");
    }

    // 初始化DTU串口(GPIO25 TX → DTU RX), 仅本机被勾选为DTU节点时才初始化并上报。
    if (Display_IsDtuEnabled()) {
        CloudReport_Init();
        Serial.println("[A33E] 本机为DTU节点, 已初始化DTU串口");
    }

    roleGraceStartedMs = millis();
    lastLvTick = millis();
}

void loop() {
    const uint32_t now = millis();
    const uint32_t elapsed = now - lastLvTick;
    if (elapsed >= UI_TICK_MS) {
        lv_tick_inc(elapsed);
        lastLvTick = now;
    }

    // 启动自测 (一次)
    if (!selfTestDone && now > 1500) {
        Display_SelfTest();
        selfTestDone = true;
    }

    // 上电先监听3秒，避免带旧配置的设备抢先初始化BLE形成双主机。
    if (!bleRoleStarted && now - roleGraceStartedMs >= 3000) {
        bleRoleStarted = true;
        bool isMaster = EspnowMesh_IsMaster();
        BleScale_SetEnabled(isMaster);
        if (isMaster) {
            Serial.printf("[ROLE] 本机仓%d是主机，启动NimBLE称重任务\n", Display_GetLocalBinId());
            BleScale_Init();
        } else {
            Serial.printf("[ROLE] 本机仓%d是从机，主机为仓%d，不初始化BLE\n",
                          Display_GetLocalBinId(), Display_GetMasterBinId());
        }
    }

    // 只显示真实净重。本机或主机数据失效时显示0，但绝不修改累计仓重。
    float currentWeight = BleScale_Loop(0.0f);
    bool scaleDataValid = EspnowMesh_IsMaster()
        ? BleScale_IsConnected()
        : EspnowMesh_RemoteBleActive();
    if (!scaleDataValid) currentWeight = 0.0f;

    // 更新 Display 当前称重
    Display_SetCurrentWeight(currentWeight);
    Display_SetScaleDataOnline(scaleDataValid);

    // 显示循环
    Display_Loop();

    // 主机每2秒广播净重；从机错峰ACK；6秒超时进入原离线告警UI。
    EspnowMesh_SetLocalBleConnected(EspnowMesh_IsMaster() && scaleDataValid);
    EspnowMesh_Loop(Display_GetBinWeight(), Display_GetCurrentWeight());

    delay(2);
}
