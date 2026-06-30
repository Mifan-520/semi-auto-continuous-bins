# A33E 称重小车蓝牙方案 — 总计划

> 当前日期：2026-06-30
> GitHub：`https://github.com/Mifan-520/esp32-.git`  
> 主工程：`esp32-firmware/`  
> 旧工程：`esp32-master/`、`esp32-slave/` 只作为历史备份/移植参考，不再直接改旧架构。

## 1. 当前架构

物理链路目标：

`称重传感器 → 三合一接线盒 → XK3190-A33E 表头 → RS485 → I6328A 蓝牙模块 → ESP32 集成屏 → ESP-NOW 组网`

DTU 上云策略已调整：后续任何仓发生变化时，先通过 ESP-NOW 同步到所有仓；所有仓拿到同一份当前状态后，再通过本机 GPIO32/GPIO25 串口上报 JSON。JSON 必须带事件来源仓号和当前全局状态，例如“仓几上料/下料了多少 g”“仓几掉线/上线”。

当前固件已实现到：

- UI + 本机仓重编辑 + NVS 持久化
- ESP-NOW 一主多从：主机每 2 秒广播净重，从机错峰 ACK，主机汇总六仓在线位图
- COM3/COM4 两块 ESP32 集成屏编译、烧录、重量包/ACK 互通验证
- 所有设备使用同一固件并具备主机能力；开发者模式手动指定唯一主机，只有主机初始化 NimBLE
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
- [x] 485 净重与仓重大数字显示统一为两位小数 (`%.2f`)
- [x] 编辑面板二次确认弹窗背景改为不透明白色 (`LV_OPA_COVER`)
- [x] 开发者模式右上角新增红色圆形 × 关闭按钮（点空白处亦可关闭）
- [x] 用户已继续微调布局；当前以代码实际布局为准

### M3：ESP-NOW 组网

- [x] 主机重量包：`MasterWeightPacket`
  - 主机仓号、配置 epoch、净重有效标志、净重、序号、六仓在线位图
- [x] 从机回复包：`FollowerAckPacket`
  - 从机仓号、主机仓号、重量序号、配置 epoch
- [x] 仓重同步包：`BinWeightSyncPacket`
  - magic `0xA33E0002`
  - binId
  - weight
- [x] 主机每 2 秒广播重量包，重量包本身就是主机在线心跳
- [x] 从机按仓号错峰回复两次 ACK，主机 6 秒未收到 ACK 则熄灭对应仓灯
- [x] 排除自身 MAC
- [x] 按仓号更新 6 个状态灯
- [x] 本机作主机时初始化即点亮自己的状态灯（`EspnowMesh_Init` 显式触发 Display 回调）
- [x] 长按 logo 可同时选择本机仓号和蓝牙主机仓号，配置写入 NVS 后自动重启应用角色
- [x] 主机配置通过 epoch 同步，旧配置设备收到新主机包后自动更新并重启
- [x] COM3/COM4 已验证主机重量广播、从机 ACK 和在线位图同步

### M3 收尾：离线/恢复告警逻辑

当前规则：

- 从机 6 秒未收到主机重量包时显示左上角黄色离线告警，六仓灯熄灭
- 净重失效时瞬时重量显示 0，但累计仓重和 NVS 数据保持不变
- 只有恢复有效净重才解除告警；其他节点在线不能掩盖称重离线
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
- [x] 改用 NimBLE；修复 native MAC 字节序反转，实际连接地址为 `39:02:01:D0:5A:50`
- [x] 只有手动指定的主机初始化 BLE，从机运行时不占用 BLE 动态内存
- [x] 非阻塞扫描/独立任务连接；BLE 操作期间 UI 和 ESP-NOW 主循环持续运行
- [x] BLE 读表头间隔与 ESP-NOW 广播统一为 3.5 秒（散热优化）
- [ ] 恢复现场 485 响应后验证净重读取（如 485 读数仍为 0，查 A/B、站号和表头状态）

## 3. 未完成

### M4 剩余：现场验证

- [ ] 确认 A33E 参数：P6=13、站号=1、波特率=9600，并与 BLE-485 模块一致
- [ ] 确认现场浮点字序；默认 ABCD，如表头实测为 CDAB 则启用 `A33E_SWAP_WORDS`
- [ ] 逐台确认仓号唯一，并在开发者模式统一指定当前主机仓

### M5：一主多从 + DTU 仲裁（已完成）

- [x] 主从角色与仓号、DTU 身份三套正交，互不依赖
- [x] 只有主机会初始化 BLE 并连接 A33E 表头
- [x] 从机不初始化 BLE，净重由主机通过 ESP-NOW 广播
- [x] 主机可切换；新主机收到确认后接管 ESP-NOW 和 BLE
- [x] DTU 上云：新增"DTU 节点"身份，固定使用 GPIO25 TX 接 DTU，与仓号和主从角色解耦
- [x] 开发者模式提供 DTU 开关(开/关)，写入 NVS，切换后重启生效
- [x] DTU 节点可以是任意仓（任意一台屏），只需那台物理接 DTU 并在开发者模式勾开
- [x] 所有产生事件的设备都广播到全网，DTU 节点收到后通过串口输出 JSON
- [ ] 1 号机固定网关规则最终落地（已由 DTU 节点方式替代该模式）

### M6：开发者模式配置

- [x] 用现有“长按 logo 进入开发者模式”，不加普通配置页
- [x] 开发者模式里设置本机仓号
- [x] 开发者模式里设置蓝牙主机仓号并同步到在线设备
- [x] 开发者模式里设置 DTU 节点开/关（任意仓均可作为 DTU 出口）
- [x] 新增开发者 UI 所需中文字形，含"关"字（已重新生成 lv_font_chinese_14）
- [x] 长按 logo 改为按住 5 秒（自计时，防误触）
- [x] 右上角红色 × 关闭按钮，点击空白处亦可关闭
- [x] 以上配置写入 NVS，切换后自动重启，断电保持
- [ ] 设置蓝牙参数入口（用户明确不需要，A33E 参数写死在 BleScaleClient.h）
- [ ] 网关标志（已由 DTU 节点方式替代，暂不需要）

### M7：全仓变化事件 + DTU JSON 上云

- [x] 任意仓发生变化时（上料/下料/编辑），广播 `BinEventPacket`(magic `0xA33E2001`) 到全网
- [x] 主机检测到位图变化时广播 online/offline 事件
- [x] 事件类型：`load`(1) / `unload`(2) / `edit`(3) / `online`(4) / `offline`(5)
- [x] DTU 节点（开发者模式勾开的任意仓）收到任意事件 → GPIO25 串口输出 JSON
- [x] JSON 字段（精简设计，仓重由云端累加，`seq` 由 sourceBin 独立递增）：
  - `reporterBin` — 本机仓号（事件来源实际为 sourceBin）
  - `sourceBin` — 事件发生的仓号
  - `eventType` — `load` / `unload` / `edit` / `online` / `offline`
  - `deltaG` — 变化重量（上料为正，下料为负；非重量事件为 0）
  - `newValue` — edit 时为新的仓重值；其他事件为 0
  - `seq` — 事件序号（每 sourceBin 独立递增，云端去重排序）
- [x] 示例语义已验证通过代码走查：仓3上料、仓5掉线、仓2恢复上线均正确触发对应 JSON
- [x] 暂不处理 DTU 联网/ACK/重试/平台协议 — 仅串口输出 JSON，后续再做
- [x] 已删除废弃的 `BinWeightSyncPacket` 全仓仓重同步包（magic/struct/回调/广播函数全部移除，grep 0命中）
- [ ] 主仓变化事件仍需硬件 485/BLE 到场后才能产生真实数据流（代码就绪，等待硬件）

### M8：散热降功耗优化（已完成）

- [x] WiFi 发射功率从 20dBm 降至 8dBm（`esp_wifi_set_max_tx_power(80)`）
- [x] ESP-NOW 主机广播间隔从 2 秒改为 3.5 秒（`WEIGHT_BROADCAST_INTERVAL_MS`）
- [x] BLE 读 A33E 表头间隔从 2 秒改为 3.5 秒（`BLE_SCALE_READ_INTERVAL_MS`）
- [x] 上述三项叠加可显著降低 ESP32 射频发射发热，同房间通信不受影响

## 4. 关键文件

| 文件 | 作用 |
|---|---|
| `esp32-firmware/src/main.cpp` | 初始化、主循环、ESP-NOW/BLE/DTU 框架串联、事件回调→JSON |
| `esp32-firmware/src/Display.h` | LVGL UI、布局、触摸、开发者模式、NVS存取、告警显示 |
| `esp32-firmware/src/EspnowMesh.h` | 主机重量广播、从机 ACK、在线位图、事件包(BinEventPacket)、主机配置同步 |
| `esp32-firmware/src/BleScaleClient.h` | NimBLE/I6328A/A33E 净重读取（3.5秒间隔）、非阻塞扫描和重连 |
| `esp32-firmware/src/CloudReport.h` | DTU JSON 串口上报（GPIO25 TX，9600 8N1） |
| `esp32-firmware/src/Config.h` | 屏幕/触摸/DTU/NVS 等全局常量 |

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

- **工程**：不要再改旧 `esp32-master/`、`esp32-slave/`，当前有效工程是 `esp32-firmware/`。
- **A33E 净重**：必须读保持寄存器 6（float），不要改回寄存器 8（毛重）。
- **主从角色**：主机与净重有效是两个独立状态；GATT 连接成功但无 Modbus 响应时仍显示 0 和离线告警。
- **仓号基址**：UI/NVS 中是 0 基，ESP-NOW 包里是 1 基，改逻辑时必须小心。
- **DTU 串口**：ESP32 GPIO25=TX(输出)→DTU RX(输入)；GPIO32=RX(输入)←DTU TX(备用)。`Serial1.begin(baud, SERIAL_8N1, RX_PIN=32, TX_PIN=25)`。
- **DTU 节点身份**：与仓号、主从三套正交。开发者模式勾开 DTU 开关，NVS 存 key `dtuEn`。只有勾开的那台负责上报 JSON；可以同时勾多台但云端需 `seq` 去重。
- **事件包**：`BinEventPacket`(magic `0xA33E2001`)，全网广播。eventType：1=load/2=unload/3=edit/4=online/5=offline。`BroadcastBinEvent` 广播后立即本地回调（确保 DTU 节点自己产生的数据也能上报，ESP-NOW 会过滤自身 MAC）。
- **online/offline**：由主机在 `ApplyOnlineMask` 里检测位图变化时广播，不是由本机自发。
- **仓重**：每仓只存本地 NVS，主机不维护全局仓重表。云端根据事件 deltaG 累加。
- **已删除的**：`BinWeightSyncPacket`（全仓仓重同步包）及其 magic/回调/广播函数。不要再加回来。
- **开发者模式**：长按 logo **5 秒**进入（自计时，不是 LVGL 默认长按）。右上角红色 × 关闭。三行配置：本机仓号(6选1) / 蓝牙主机仓号(6选1) / DTU开关(开/关)。
- **散热**：WiFi 发射功率已降为 8dBm；ESP-NOW 广播和 BLE 读表头间隔均为 3.5 秒。不要改回满功率或短间隔。
- **中文字库**：`lv_font_chinese_14.c` 已包含"关"字。如需加新字，用原始命令重新生成（注释第4行），记得 include 路径改为 `<lvgl.h>`。
- **告警残影**：优先 TFT 级别 `fillRect()`，不要继续堆 LVGL 隐藏逻辑。
- **不要提交**：`node_modules/` 和 `nul`。
