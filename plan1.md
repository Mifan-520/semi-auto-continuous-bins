# A33E 称重小车蓝牙方案 — 总计划

> 当前日期：2026-06-26  
> GitHub：`https://github.com/Mifan-520/esp32-.git`  
> 主工程：`esp32-firmware/`  
> 旧工程：`esp32-master/`、`esp32-slave/` 只作为历史备份/移植参考，不再直接改旧架构。

## 1. 当前架构

物理链路目标：

`称重传感器 → 三合一接线盒 → XK3190-A33E 表头 → RS485 → I6328A 蓝牙模块 → ESP32 集成屏 → ESP-NOW 组网 → 网关机 → DTU JSON 上云`

当前固件已实现到：

- UI + 本机仓重编辑 + NVS 持久化
- ESP-NOW 6 仓心跳广播、在线状态、仓重同步
- COM3/COM4 两块 ESP32 集成屏编译、烧录、互收心跳验证
- 蓝牙/A33E/DTU 仍是后续阶段，当前只保留接口和框架

硬件锁定：

| 项目 | 配置 |
|---|---|
| 屏幕 | ST7796，横屏 480×320 |
| TFT | CS15 / DC2 / MOSI13 / MISO12 / SCLK14 / BL27 / RST=-1 |
| 触摸 | TP_CS33 / TP_IRQ36 |
| 烧录口 | COM3 / COM4，CH340 |
| ESP-NOW | WiFi STA，固定 ch6，广播 MAC |
| 网关约束 | 1 号机固定网关 |
| DTU 预留 | Serial1，TX=GPIO32，RX=GPIO25，默认 9600 |

## 2. 已完成

### M0：老代码备份

- [x] 旧 `esp32-master/`、`esp32-slave/` 已作为回滚点保留
- [x] `main` 和 `v0-legacy` 已推送 GitHub
- [x] 当前新开发集中在 `esp32-firmware/`

### M1：统一固件工程 + UI

- [x] PlatformIO/Arduino 工程已建立
- [x] 复用旧从机字体、logo、LVGL/TFT_eSPI 配置
- [x] 首页包含：logo、仓号、大圆灯、6 仓小灯、编辑按钮、当前称重、仓重大数字、上料/下料按钮
- [x] 中文字体已扩展，支持当前 UI 所需字符
- [x] 上料/下料确认弹窗
- [x] 编辑面板：数字键盘、确认修改、本仓仓重保存
- [x] NVS 保存 6 仓重量和本机仓号，断电重启后恢复
- [x] 离线告警框已改为黄色高亮，并采用白色覆盖块清残影
- [x] 用户已继续微调布局；当前以代码实际布局为准

### M3：ESP-NOW 组网

- [x] 心跳包：`HeartbeatPacket`
  - magic `0xA33E0001`
  - binId
  - role
  - online
  - binWeight
  - currentWeight
  - seq
  - MAC
- [x] 仓重同步包：`BinWeightSyncPacket`
  - magic `0xA33E0002`
  - binId
  - weight
- [x] ESP-NOW 广播心跳
- [x] 排除自身 MAC
- [x] 按仓号更新 6 个状态灯
- [x] 长按 logo 进入开发者模式选择本机仓号
- [x] 换仓后连发上线心跳，通知其他屏幕
- [x] 启动后立即连发上线广播，解决断电重上电后其他仓不知道本机上线的问题
- [x] COM3/COM4 已反复烧录验证互相收到仓号心跳

### M3 收尾：离线/恢复告警逻辑

当前规则：

- 全静默/断网时显示左上角黄色离线告警
- 收到任意有效仓心跳后，认为 ESP-NOW 通信恢复
- 恢复后：
  - 大圆灯转绿
  - 按钮启用
  - 黄色告警框隐藏
  - 同一区域显示白色覆盖块，强制刷掉可能的屏幕残影
- 6 个仓小灯整体右移 5px

注意：

- 若现场仍看到“告警”，需要先区分是黄色告警框残影、大圆灯红色，还是某个仓小灯灰色。
- 如果黄色框仍残留，下一步建议改为直接在 TFT 层 `fillRect()` 清该区域，而不是只依赖 LVGL 对象覆盖。

### M4：蓝牙 SPP 读 A33E 框架

- [x] `BleScaleClient.h` 已有框架文件
- [ ] I6328A 模块和 A33E 表头到位后再启用真实读取
- [ ] 当前主循环仍使用模拟称重，蓝牙读取暂未启用

## 3. 未完成

### M4 剩余：真实称重

- [ ] 蓝牙配对/连接 I6328A
- [ ] 读取 A33E/Modbus-RTU 重量
- [ ] 断线回退和重连策略
- [ ] 替换当前模拟称重

### M5：角色/网关策略

- [ ] 1 号机固定网关规则最终落地
- [ ] 多机接入后，明确普通节点和网关节点行为差异
- [ ] 是否需要故障接管，后续再定

### M6：配置页

- [ ] 运行时设置本机仓号
- [ ] 设置网关标志
- [ ] 设置蓝牙参数
- [ ] 设置 DTU 参数
- [ ] NVS 保存配置

### M7：DTU JSON 上云

- [ ] 使用 GPIO32/GPIO25 串口输出 JSON 行
- [ ] 上报仓号、仓重、当前称重、在线状态、上料/下料事件
- [ ] 暂不处理 DTU 联网、ACK、重试、平台协议

## 4. 关键文件

| 文件 | 作用 |
|---|---|
| `esp32-firmware/src/main.cpp` | 初始化、主循环、ESP-NOW/UI/BLE 框架串联 |
| `esp32-firmware/src/Display.h` | LVGL UI、布局、触摸、编辑面板、告警显示 |
| `esp32-firmware/src/EspnowMesh.h` | ESP-NOW 心跳、在线表、仓重同步、离线检测 |
| `esp32-firmware/src/BleScaleClient.h` | 蓝牙/A33E 读取框架，待硬件验证 |
| `esp32-firmware/src/CloudReport.h` | DTU JSON 上报占位 |
| `esp32-firmware/src/Config.h` | 屏幕、触摸、DTU、网关等常量 |

## 5. 当前验证命令

```bash
# 编译
pio run -d esp32-firmware

# 烧录
pio run -d esp32-firmware -t upload --upload-port COM3
pio run -d esp32-firmware -t upload --upload-port COM4

# 串口监视
pio device monitor -d esp32-firmware -p COM3 -b 115200
pio device monitor -d esp32-firmware -p COM4 -b 115200
```

## 6. 给下一个 AI 的注意事项

- 不要再改旧 `esp32-master/`、`esp32-slave/`，除非用户明确要求。
- 当前有效工程是 `esp32-firmware/`。
- 仓号在 UI/NVS 中是 0 基，ESP-NOW 包里是 1 基，改逻辑时必须小心。
- `GPIO32/GPIO25` 是用户指定的 DTU 串口脚，不要换。
- `node_modules/` 和 `nul` 不应提交。
- 如果继续处理告警残影，优先考虑 TFT 级别清屏矩形，而不是继续堆 LVGL 隐藏逻辑。
