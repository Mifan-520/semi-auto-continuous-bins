# A33E 称重小车蓝牙方案 — 总计划

> 当前日期：2026-06-29
> GitHub：`https://github.com/Mifan-520/esp32-.git`  
> 主工程：`esp32-firmware/`  
> 旧工程：`esp32-master/`、`esp32-slave/` 只作为历史备份/移植参考，不再直接改旧架构。

## 1. 当前架构

物理链路目标：

`称重传感器 → 三合一接线盒 → XK3190-A33E 表头 → RS485 → I6328A 蓝牙模块 → ESP32 集成屏 → ESP-NOW 组网`

DTU 上云策略已调整：后续任何仓发生变化时，先通过 ESP-NOW 同步到所有仓；所有仓拿到同一份当前状态后，再通过本机 GPIO32/GPIO25 串口上报 JSON。JSON 必须带事件来源仓号和当前全局状态，例如“仓几上料/下料了多少 g”“仓几掉线/上线”。

当前固件已实现到：

- UI + 本机仓重编辑 + NVS 持久化
- ESP-NOW 6 仓心跳广播、在线状态、仓重同步
- COM3/COM4 两块 ESP32 集成屏编译、烧录、互收心跳验证
- BLE 直连 I6328A、A33E Modbus 净重读取、全节点自动接管逻辑已进入固件
- DTU 仍是后续阶段，当前只保留接口和框架

硬件锁定：

| 项目 | 配置 |
|---|---|
| 屏幕 | ST7796，横屏 480×320 |
| TFT | CS15 / DC2 / MOSI13 / MISO12 / SCLK14 / BL27 / RST=-1 |
| 触摸 | TP_CS33 / TP_IRQ36 |
| 烧录口 | COM3 / COM4，CH340 |
| ESP-NOW | WiFi STA，固定 ch6，广播 MAC |
| 网关约束 | 1 号机固定网关，用于组网/管理约束；DTU 上云不依赖网关代发 |
| DTU 预留 | 每仓本机 Serial1，TX=GPIO32，RX=GPIO25，默认 9600；收到全仓同步事件后上报当前信息 JSON |

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

### M4：蓝牙读 A33E 净重

- [x] `BleScaleClient.h` 已有框架文件
- [x] 保持寄存器 6 读取 A33E 净重浮点数（寄存器 8 是毛重，不再使用）
- [x] 删除主循环正弦模拟重量；无有效本地/远程读数时回退为 0
- [x] 六台设备均可主动接管；按仓号和芯片 ID 错峰，首个有效读数建立心跳租约
- [x] 物理断联、485 无数据或租约超时后广播释放，其他设备自动竞选
- [ ] 六台实机同时上电、断电、拔 485 的完整接管验证

## 3. 未完成

### M4 剩余：现场验证

- [ ] 确认 A33E 参数：P6=13、站号=1、波特率=9600，并与 BLE-485 模块一致
- [ ] 确认现场浮点字序；默认 ABCD，如表头实测为 CDAB 则启用 `A33E_SWAP_WORDS`
- [ ] 逐台确认仓号唯一，再做六台抢连和故障接管测试

### M5：角色/网关策略

- [ ] 1 号机固定网关规则最终落地
- [ ] 多机接入后，明确普通节点和网关节点行为差异
- [x] BLE 称重需要故障接管：所有节点可接管，但同一时刻只认一个有效净重租约

### M6：开发者模式配置

- [ ] 不再单独做普通配置页，沿用现有“长按 logo 进入开发者模式”
- [ ] 开发者模式里设置本机仓号
- [ ] 开发者模式里设置网关标志
- [ ] 开发者模式里设置蓝牙参数
- [ ] 开发者模式里设置 DTU 参数
- [ ] 以上配置写入 NVS，断电保持
- [ ] 用户现场自行配置这些参数，固件只提供入口和保存能力

### M7：全仓同步事件 + DTU JSON 上云

- [ ] 任意仓发生变化时，先用 ESP-NOW 广播事件到所有仓
- [ ] 所有仓收到事件后，更新本地全仓状态表，再通过本机 GPIO32/GPIO25 串口输出 JSON 行
- [ ] 不设计成“只由 1 号网关代发”；每台屏都具备收到同步事件后上报当前信息的能力
- [ ] JSON 必须包含：
  - 本机仓号 `reporterBin`
  - 事件来源仓号 `sourceBin`
  - 事件类型 `eventType`：`load` / `unload` / `online` / `offline` / `weightSync`
  - 变化重量 `deltaG`，上料为正、下料为负；非重量事件可为 0
  - 来源仓当前仓重 `sourceBinWeightG`
  - 来源仓当前称重 `sourceCurrentWeightG`
  - 6 仓在线状态快照
  - 6 仓仓重快照
- [ ] 示例语义：仓 3 上料 500g、仓 5 掉线、仓 2 恢复上线，都应同步到所有仓并触发 JSON 上报
- [ ] 暂不处理 DTU 联网、ACK、重试、平台协议，只负责串口输出 JSON 行

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
- A33E 业务重量必须读净重 float（保持寄存器 6），不要改回毛重寄存器 8。
- BLE 占用状态必须以“近期收到有效 Modbus 净重”为准，不能仅以 GATT 连接成功为准。
- 仓号在 UI/NVS 中是 0 基，ESP-NOW 包里是 1 基，改逻辑时必须小心。
- `GPIO32/GPIO25` 是用户指定的 DTU 串口脚，不要换；后续每个仓收到全仓同步事件后，都用这组串口上报当前信息 JSON。
- M6 不另做普通配置页，直接扩展现有长按 logo 的开发者模式。
- `node_modules/` 和 `nul` 不应提交。
- 如果继续处理告警残影，优先考虑 TFT 级别清屏矩形，而不是继续堆 LVGL 隐藏逻辑。
