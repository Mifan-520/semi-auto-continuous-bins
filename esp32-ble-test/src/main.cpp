// ============================================================
//  ESP32 BLE 实时读取 A33E 毛重 (经 I6328A-485 透传)
//
//  链路: A33E表头(9600) ──RS485──> I6328A-485(9600,BLE从机)
//                                    ══BLE══ ESP32(BLE主机)
//
//  原理:
//    ESP32 通过 BLE Write(0xFFE1) 发 Modbus-RTU 请求帧
//    模块从 UART/485 透传给 A33E, A33E 回 Modbus 响应
//    响应经模块 UART→BLE Notify(0xFFE2) 回到 ESP32
//    ESP32 累积 Notify 字节, 凑齐整帧后解析 float 毛重
//
//  Modbus 请求: 读保持寄存器, 站号1, 地址8(毛重float), 数量2
//    01 03 00 08 00 02 45 C1
//
//  Modbus 响应: 01 03 04 [4字节float] [CRC2字节] = 9字节
// ============================================================

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAddress.h>

// ---------- 目标设备 ----------
// 注意: AT+LEGA 返回 505AD0010239 是反序的,
//       ESP32 扫描/手机看到的 MAC 是 39:02:01:D0:5A:50
//       蓝牙名 XLCX-D05A50 不受波特率影响
static const char* TARGET_NAME = "XLCX-D05A50";
static const bool  USE_MAC = false;
static const char* TARGET_MAC = "39:02:01:D0:5A:50";

// ---------- I6328A GATT UUID (16位标准UUID的128位形式) ----------
static BLEUUID SERVICE_UUID("0000ffe0-0000-1000-8000-00805f9b34fb");
static BLEUUID WRITE_UUID  ("0000ffe1-0000-1000-8000-00805f9b34fb");
static BLEUUID NOTIFY_UUID ("0000ffe2-0000-1000-8000-00805f9b34fb");

// ---------- 读毛重 Modbus-RTU 请求 ----------
// 站号1, 功能码03, 起始地址0x0008, 数量2, CRC
static uint8_t MODBUS_REQ[] = {0x01, 0x03, 0x00, 0x08, 0x00, 0x02, 0x45, 0xC1};
static const uint32_t READ_INTERVAL_MS = 500;   // 500ms 读一次
static const uint32_t RESYNC_TIMEOUT_MS = 200;  // 200ms 没凑成帧就认为残包, 清缓冲重新累积

// ---------- 状态 ----------
static BLEClient* pClient = nullptr;
static BLERemoteService* pService = nullptr;
static BLERemoteCharacteristic* pWrite = nullptr;
static BLERemoteCharacteristic* pNotify = nullptr;
static bool connected = false;

// ---------- 接收缓冲 (Notify 异步 + 可能分包, 累积凑帧) ----------
static uint8_t  rxbuf[32];
static size_t   rxlen = 0;
static uint32_t rxLastMs = 0;
static uint32_t lastSendMs = 0;
static uint32_t okCount = 0;
static uint32_t errCount = 0;
static uint32_t recvCount = 0;       // 收到 Notify 的次数
static uint32_t sentSinceLastRecv = 0;

// ---------- Modbus CRC16 ----------
static uint16_t modbusCRC16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc & 0x0001) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
        }
    }
    return crc;
}

// ---------- float 字节序工具 ----------
static float bytesToFloat(const uint8_t* p, bool swapWords) {
    uint8_t b[4];
    if (swapWords) {
        // CDAB (寄存器内大端, 寄存器间小端)
        b[0] = p[2]; b[1] = p[3]; b[2] = p[0]; b[3] = p[1];
    } else {
        // ABCD (全大端)
        b[0] = p[0]; b[1] = p[1]; b[2] = p[2]; b[3] = p[3];
    }
    float f;
    memcpy(&f, b, 4);
    return f;
}

// ---------- 尝试从缓冲解析一帧 Modbus 响应 ----------
// 成功返回 true 并把重量写入 outGross
static bool tryParseFrame(float* outGross, bool* outSwap) {
    if (rxlen < 9) return false;

    // 找帧头: 01 03 04 (站号1 + 功能码3 + 字节数4)
    int start = -1;
    for (size_t i = 0; i + 9 <= rxlen; ++i) {
        if (rxbuf[i] == 0x01 && rxbuf[i+1] == 0x03 && rxbuf[i+2] == 0x04) {
            start = i;
            break;
        }
    }
    if (start < 0) return false;

    const uint8_t* frame = &rxbuf[start];
    // 验 CRC (前7字节: 站号+功能码+字节数+4字节float)
    uint16_t crcCalc = modbusCRC16(frame, 7);
    uint16_t crcRcvd = frame[7] | (frame[8] << 8);
    if (crcRcvd != crcCalc) {
        Serial.printf("[Modbus] CRC校验失败 calc=%04X rcvd=%04X\n", crcCalc, crcRcvd);
        return false;
    }

    // 两种字节序都解析, 调用方决定哪个合理
    float fBE  = bytesToFloat(frame + 3, false);  // ABCD
    float fSW  = bytesToFloat(frame + 3, true);   // CDAB
    *outSwap = false;
    *outGross = fBE;
    // 同时打印两种, 方便对照哪个对
    Serial.printf("[Modbus] raw=%02X %02X %02X %02X  big-endian=%.3f  swap-word=%.3f\n",
                  frame[3], frame[4], frame[5], frame[6], fBE, fSW);
    return true;
}

// ---------- Notify 回调: 模块上行数据到这里 ----------
static void notifyCallback(BLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify) {
    // 诊断: 每次回调都打印原始字节
    Serial.printf("[BLE-RECV %uB] ", (unsigned)len);
    for (size_t i = 0; i < len; i++) Serial.printf("%02X ", data[i]);
    Serial.println();
    recvCount++;
    sentSinceLastRecv = 0;
    // 追加到缓冲
    for (size_t i = 0; i < len && rxlen < sizeof(rxbuf); ++i) {
        rxbuf[rxlen++] = data[i];
    }
    rxLastMs = millis();
}

// ---------- 连接 ----------
static bool connectToServer(BLEAddress addr) {
    Serial.printf("[BLE] 连接 %s ...\n", addr.toString().c_str());
    pClient = BLEDevice::createClient();
    if (!pClient->connect(addr)) {
        Serial.println("[BLE] 连接失败");
        return false;
    }
    pService = pClient->getService(SERVICE_UUID);
    if (!pService) {
        Serial.println("[BLE] 找不到 Service 0xFFE0");
        pClient->disconnect();
        return false;
    }
    pWrite  = pService->getCharacteristic(WRITE_UUID);
    pNotify = pService->getCharacteristic(NOTIFY_UUID);
    if (!pWrite || !pNotify) {
        Serial.println("[BLE] 找不到 Write/Notify 特征");
        pClient->disconnect();
        return false;
    }
    if (pNotify->canNotify()) {
        pNotify->registerForNotify(notifyCallback);
    }
    connected = true;
    Serial.println("[BLE] ✅ 已连接, 开始读 A33E 毛重...");
    return true;
}

static bool scanAndConnect() {
    Serial.println("[BLE] 扫描 (5秒)...");
    BLEScan* scan = BLEDevice::getScan();
    scan->setActiveScan(true);
    BLEScanResults results = scan->start(5, false);
    for (int i = 0; i < results.getCount(); i++) {
        BLEAdvertisedDevice d = results.getDevice(i);
        std::string nm = d.haveName() ? d.getName() : std::string("");
        if (nm.find(TARGET_NAME) != std::string::npos) {
            Serial.printf("[BLE] 扫到 %s [%s]\n", nm.c_str(), d.getAddress().toString().c_str());
            return connectToServer(d.getAddress());
        }
    }
    Serial.println("[BLE] 未扫到目标, 本轮扫到:");
    for (int i = 0; i < results.getCount(); i++) {
        BLEAdvertisedDevice d = results.getDevice(i);
        std::string nm = d.haveName() ? d.getName() : std::string("(无名)");
        Serial.printf("   - %s [%s]\n", nm.c_str(), d.getAddress().toString().c_str());
    }
    return false;
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=============================");
    Serial.println(" ESP32 BLE 实时读 A33E 毛重");
    Serial.println("=============================");
    BLEDevice::init("ESP32-A33E-Reader");
}

void loop() {
    // 1. 连接维护
    if (!connected) {
        if (USE_MAC) connectToServer(BLEAddress(TARGET_MAC));
        else scanAndConnect();
        if (!connected) { delay(5000); return; }
        return;
    }
    if (!pClient || !pClient->isConnected()) {
        Serial.println("[BLE] 断开, 重连");
        connected = false;
        pWrite = pNotify = nullptr;
        pService = nullptr;
        rxlen = 0;
        delay(2000);
        return;
    }

    uint32_t now = millis();

    // 2. 残包超时清缓冲 (避免上一帧没收齐污染下一帧)
    if (rxlen > 0 && now - rxLastMs > RESYNC_TIMEOUT_MS) {
        rxlen = 0;
    }

    // 3. 定时发请求
    if (now - lastSendMs >= READ_INTERVAL_MS) {
        lastSendMs = now;
        // 诊断: 发请求前缓冲里还剩多少 (上一轮没解析掉的字节)
        if (rxlen > 0) {
            Serial.printf("[DBG] 发新请求, 旧缓冲剩 %u 字节未解析: ", (unsigned)rxlen);
            for (size_t i = 0; i < rxlen; i++) Serial.printf("%02X ", rxbuf[i]);
            Serial.println();
        }
        rxlen = 0;  // 发新请求前清缓冲, 只等本次响应
        if (pWrite) {
            pWrite->writeValue(MODBUS_REQ, sizeof(MODBUS_REQ), false);
            sentSinceLastRecv++;
        }
        // 每10次请求打印一次心跳, 确认主循环活着
        if (sentSinceLastRecv % 10 == 0) {
            Serial.printf("[DBG] 心跳: 已发 %u 次请求, 共收到 %u 次Notify, OK=%u ERR=%u\n",
                          sentSinceLastRecv, recvCount, okCount, errCount);
        }
    }

    // 4. 收齐整帧后解析
    float gross;
    bool swap;
    if (tryParseFrame(&gross, &swap)) {
        okCount++;
        // 这里先按 big-endian 显示, 如果你看 swap-word 那个数对,
        // 告诉我, 我把解析固定成 swap 字节序
        Serial.printf(">>> 毛重 = %.3f kg  (OK=%u ERR=%u)\n", gross, okCount, errCount);
        rxlen = 0;
    }
}
