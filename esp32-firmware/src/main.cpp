#include <Arduino.h>
#include "Config.h"
#include "CloudReport.h"
#include "EspnowMesh.h"
#include "BleScaleClient.h"
#include "Display.h"

static uint32_t lastLvTick = 0;
static bool selfTestDone = false;
static float simFallbackWeight = 0.0f;  // BLE断连时的回退重量(0kg)

// ESP-NOW 收到某仓状态变化 → 通知Display更新对应灯
void onBinStateChange(uint8_t binId, bool online, float binWeight, float currentWeight) {
    Display_OnBinStateChange(binId, online, binWeight, currentWeight);
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
    EspnowMesh_SetMyBin(localBinId);
    BleScale_SetMyBinId(localBinId);   // 让BLE任务知道本机仓号, 决定是否主动连模块
    EspnowMesh_SetGateway(DEFAULT_GATEWAY_FLAG);
    EspnowMesh_SetStateCallback(onBinStateChange);
    EspnowMesh_SetWeightSyncCallback(Display_OnBinWeightSync);
    EspnowMesh_SetSilenceCallback(Display_OnSilence);
    if (!EspnowMesh_Init()) {
        Serial.println("[A33E] ESP-NOW初始化失败, 仅UI运行");
    } else {
        // 上电后立即广播上线，不等待2秒心跳周期，避免其他仓不知道本机已恢复。
        EspnowMesh_AnnounceOnline(Display_GetBinWeight(), Display_GetCurrentWeight(), 5, 40);
    }

    // 3. 蓝牙称重: 启用 BLE 读 A33E 毛重
    BleScale_Init();

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

    // 模拟称重更新 (200ms间隔, 正弦波动)
    if (now - lastSimUpdate >= SIM_UPDATE_MS) {
        lastSimUpdate = now;
        simFallbackWeight = 25.0f + 4.0f * sinf(now / 1600.0f);
    }

    // 蓝牙读真实重量 (优先), 断连时回退到 simFallbackWeight(0kg)
    float currentWeight = BleScale_Loop(simFallbackWeight);

    // 更新 Display 当前称重
    Display_SetCurrentWeight(currentWeight);

    // 显示循环
    Display_Loop();

    // ESP-NOW 组网循环: 广播心跳 + 离线检测
    EspnowMesh_Loop(Display_GetBinWeight(), Display_GetCurrentWeight());

    delay(2);
}
