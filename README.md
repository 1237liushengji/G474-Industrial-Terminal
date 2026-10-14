# STM32G474 工业数据采集终端（RS485 + CAN + Modbus + MQTT）

基于两块 DRG ST-10 STM32G474VET6 开发板，把既有的 **CAN 双机通信（v3.0）** 与
**Modbus-RTU 双节点（v2.0）** 两个已验证工程，演进为一套带本地 HMI、离线缓存与 MQTT 上云能力的
边缘采集网关。**当前状态：v1.0 已收口（2026-10-14）**——14 项验收判据全部实机执行完毕
（10 项全绿；A-07 的 QoS0 补发与 A-08 的 200ms 时延按形态/分辨率限制记录；A-06 与 485 直读腿
按"未持有 USB-RS485"留白），**24h 泡机通过（task_ovr=0、零复位）**；4 Target 全量重建
0 Error / 0 Warning；上板验收期间累计修复 **9 个实机缺陷**（详见 [迁移说明 §18/§19](docs/迁移说明.md)）。

## 系统框图

```
              PC（实验室内）: Mosquitto 2.1.2 @ E:\Edge\mosquitto-2.1.2 :1883
              Tools/python：Modbus 主站/观测者 · MQTT 订阅校验 · 契约哈希校验
                    ▲                                     ▲
        Wi-Fi 2.4G  │ (设备侧 MQTT-C)          USB-TTL  │ (console + 可选影子网关)
                    │                                     │
 ┌──────────────────┴──── A 板 · GATEWAY ────────────────┴────────────────────┐
 │ USART3/PB10,PB11 → 485 收发器 → Modbus RTU 主站 ═══ RS485 总线(A/B) ═══┐      │
 │ LPUART1/PB10,PB11 → 板载 ESP8266 → MQTT-C 设备侧上云（M2 形态）        │      │
 │ FDCAN2/PB12,PB13 → CAN 收发器 ─────── CAN 500 kbit/s ──────────────────┼──┐   │
 │ SPI1/PA4-PA7 → 板载 W25Q128 16MB：事件日志 + MQTT 待发缓存             │  │   │
 │ I2C1/PA15,PB9 → 24C02（网关参数）                                     │  │   │
 │ LCD(PB3/PB5/PD11-13) + LED PE0/PE1 + KEY PA0/PA2/PA3 + IWDG           │  │   │
 └───────────────────────────────────────────────────────────────────────┘  │   │
                                                                            │   │
 ┌──────────────────────── B 板 · NODE ──────────────────────────────────────┴───┴──┐
 │ USART3/PB10,PB11 → 485 收发器 → Modbus RTU 从站 ═══════════════════════════ RS485 总线 │
 │ FDCAN2/PB12,PB13 → CAN 收发器；SPI1/PA4-7 → 板载 16MB Flash；I2C1 → 24C02          │
 │ LCD + LED + KEY + 本地采集 + 离线自治（A 板掉线时仍独立工作）                        │
 └────────────────────────────────────────────────────────────────────────────────────┘
```

**A 板与 B 板结构同构**：同一套固件、同一份引脚表，只靠 `NODE_ROLE` 区分主从。

## 三条先读结论

1. **一切以实物丝印为准（原理图 PDF 与实物不符，已降级）。** 丝印 + 两个原工程实机跑通给出最终引脚：
   CAN=FDCAN2 `PB12/PB13`（跳线帽）、485=USART3 `PB10/PB11`（跳线帽）、WiFi=LPUART1 `PB10/PB11`（跳线帽，
   方向与 485 镜像）、Flash=SPI1 `PA4-PA7`、24C02=I2C1 `PA15/PB9`、LCD=`PB3/PB5/PD11/12/13`、
   按键=`PA0/PA2/PA3`、LED=`PE0/PE1`。详见 [迁移说明 §17](docs/迁移说明.md)。
2. **唯一的 IO 冲突：485 与 WiFi 共用 PB10/PB11 跳线帽（二选一），且分属不同 UART。** 消解靠角色分工：
   **B 板插 485（Modbus 从站），A 板 M1 插 485 / M2 插 WiFi**，A↔B 数据面统一走 CAN（PB12/13 独立通道）。
   两块板都插 CAN 跳线帽。详见 [迁移说明 §17](docs/迁移说明.md)。
3. **两个原工程按丝印实机跑通**：`E://STM32//CAN//阶段3_v3.0`（FDCAN2/PB12-13）与 `D://DSH//Modbus-RTU`
   （USART3/PB10-11、SPI1/PA4-7）——本项目现按同一套引脚对齐，上板即可复用其验证成果。

## 文档索引

| 文档 | 内容 | 什么时候读 |
|---|---|---|
| [**07 · 板级硬件核对结论与冲突修正**](docs/07-板级硬件核对与冲突修正.md) | 四方交叉核对、板上真实器件清单、最终引脚表、7 条冲突裁决、修正后的拓扑与配置宏、遗留待确认项 | **最先读**。与其它文档冲突时以本文为准 |
| [00 · 决策记录与硬件约束](docs/00-决策记录与硬件约束.md) | 9 条 ADR、范围裁剪、风险登记（已被 07 部分修订，修订标记在文首） | 读 07 之后 |
| [01 · 参考项目对比与选型](docs/01-参考项目对比与选型.md) | 18 个 GitHub 同类项目的可借鉴点/差异/采纳结论；MQTT 库选型 | 想知道"为什么不直接用现成的" |
| [02 · 系统架构与 FreeRTOS 任务模型](docs/02-系统架构与FreeRTOS任务模型.md) | 分层与目录、7 任务表、4 条关键时序、7 条移植决策 | 写代码前 |
| [03 · 数据契约 v1](docs/03-数据契约-v1.md) | 64 个保持寄存器总表、CAN 帧内字节/过滤器、MQTT 主题/payload/QoS/补发、契约哈希定义 | **改任何数据点前** |
| [04 · 板级配置点与资源预算](docs/04-板级配置点与资源预算.md) | 配置宏、4 Target、引脚总表、NVIC 总表、RAM/Flash 预算、时钟核算（配置宏与引脚表已被 07 取代，用 07 §6/§3） | 遇到"这个宏该放哪"时 |
| [05 · 实施路线与验收矩阵](docs/05-实施路线与验收矩阵.md) | v0.1→v1.0 八个里程碑、14 项带数字的验收判据、11 种故障注入 | 每个 tag 收口时 |
| [06 · 资产复用与冲突消解](docs/06-资产复用与冲突消解.md) | 可复用资产总账、冲突决议、11 处文档-代码漂移、合并前置动作 | v0.1 执行时逐条照做 |

## 目录结构（v0.5：FreeRTOS + CAN 桥 + M1 上行已入仓）

```
App/        app_rtos(T1-T7 任务集+心跳+故障钩子) mb_master_port gw 角色 node_app 节点角色+UI
Gateway/    can_router.c(表驱动路由) bridge.c(CAN↔寄存器桥+镜像+快照+CMD/ACK) uplink_port.c(M1 0xAA55 帧/T4)
Network/    mqttc/(MQTT-C v1.1.6+PAL 移植) esp_at + at_window + mqtt_client + backfill
Protocol/   modbus_*（64 寄存器/FC04/0x0F 空洞）  isotp(多实例)  uds  can_contract.h(契约帧)
BSP/        app_board_cfg.h  bsp_console(USART1)  bsp_uart(RS485 TXE/TC 中断)  rs485 tick eeprom w25q128 lcd key led iwdg can_drv(队列+4 条白名单滤波)
Storage/    config.c(24C02)  log.c(W25Q 256KB 环形)  log_service.c(队列化,log_task)
Common/     types.h  ring_buffer.c  bsp_printf.c(4x128B/任务+互斥)  console.c  dp_table.c(契约唯一源)  fmt.c
Middlewares/FreeRTOS-Kernel/  V10.5.1 裁剪版（tasks/list/queue/port/heap_4, GCC/ARM_CM4F 端口）
Tools/python/    modbus/ can/ contract_check.py + dp_table.json  uplink_frames + mqtt_shadow_gw + mqtt_verify + test_shadow_e2e
Tools/mqtt/      mosquitto.conf + mqtt_topics.md（主题/线协议冻结）
Tools/verification/ crc_check.js  modbus_vectors.js  modbus_vectors.json   黄金向量（11 条，冻结）
Tools/ci/        check_config_points.py/.sh (A-01)  run_acceptance.py（一键验收，v0.8）
docs/       本方案文档集 + 后续实验记录
MDK-ARM/    G474.uvprojx（4 Target）+ build_check.log
```

## 环境事实（已实测）

| 项 | 值 |
|---|---|
| MCU | STM32G474VETx，512 KB Flash / 128 KB SRAM，SYSCLK 150 MHz（HSE 8 MHz→PLL /2 ×75 /2），APB 全 /1 |
| 板卡资料 | STM32G474VET6 数据手册 + **实物正反面丝印照片（最终权威，2026-10-07 终审）**；`STM32G474VET6V1.1原理图.pdf` 经查与实物不符（见迁移说明 §17） |
| RTOS | FreeRTOS-Kernel **V10.5.1**（`Middlewares/`，GCC/ARM_CM4F 端口适配 ARMCLANG V6.14）；HAL 时基 = TIM6，SysTick 归内核 |
| IDE | Keil MDK，ARMCLANG V6.14，DFP `STM32G4xx_DFP 2.2.0`，HAL **V1.2.3**（锁版本，勿顺手升级） |
| PC | Windows 11 22631；Python 3.14.7（`pyserial` / `pypdf` / `pypdfium2`）；Node v26.7.0 |
| Broker | Mosquitto 2.1.2 源码在 `E:\Edge\mosquitto-2.1.2`（**尚无二进制，需安装或自编译**）；开发/CI 用 pip `amqtt`（`Tools/python/test_shadow_e2e.py` 内建） |
| 手柄串口 | 板载 CH340K：**COM3 = 板A、COM5 = 板B**（验收期实测）。⚠️ 默认方式打开串口会在端口开启期间把板**按住复位**——串口工具必须静默打开（`dtr=False/rts=False` 预设后 open，见迁移说明 §19.3） |
| **已持有** | USB-TTL 串口线 ✅、外接 24C02 模块 ✅、外接 W25Q 模块 ✅、板卡实物丝印照片 ✅ + MCU 数据手册 ✅（板载 ESP8266 焊装、24C02/W25Q128 板上都有） |
| **未持有** | USB-RS485 ❌（仅影响 A-06/A-11 的验收自动化，**不阻塞运行**）、USB-CAN ❌（可选） |
| 容器环境 | **无容器引擎**，且本项目不需要。已装的是 `Docker Sandboxes 0.43.0`（`Docker.sbx`，AI 代理隔离工具，`sbx.exe` 在 `%LOCALAPPDATA%\DockerSandboxes\bin`，非容器引擎）。broker 走原生 Mosquitto |

## 验收矩阵（v1.0 收官 · 2026-10-14）

> 全部判据已实机执行：**✅ = 通过；◐ = 实测完成（限制已记录）；⏳ = 未持有硬件留白**。
> 一键验收 `python Tools/ci/run_acceptance.py`；实机证据汇总见 [迁移说明 §18/§19](docs/迁移说明.md)。判据数字见 docs/05 §2。

| ID | 项目 | 状态 |
|---|---|---|
| A-01 | 单一配置点 | ✅ 脚本 0 命中 |
| A-02 | 双时基一致 | ✅ 双板 10min+ 连续，60s 窗口 uptime 漂移 ≤1s，无 HAL 超时（10-12） |
| A-03 | CAN 丢帧核算 | ✅ 30min+ 窗口对账：ΔA_rx=ΔB_tx、ΔB_rx=ΔA_tx、txAck==tx、ovr=0（10-13） |
| A-04 | bus-off 恢复 | ✅ 3s 短接 CAN_H-GND → bus-off×6 → 退避（1→2→4s、30s 封顶）恢复 ≈15s（≤30s 判据）；开路线故障另有对照记录（10-13） |
| A-05 | 契约哈希三方 | ✅ C↔JSON 离线 + 实机三方一致（reg 0x18=0xE71B=meta.ch=snapshot header）；PC 直读腿 ⏳ 未持有 USB-RS485 |
| A-06 | 双路交叉比对 | ⏳ 未持有 USB-RS485（`mqtt_verify.py` 就绪，待适配器） |
| A-07 | 断 broker 补发 | ◐ M1 实测：QoS1（status/snapshot/meta）断线 60s 全量续传；QoS0（telemetry/counters）按契约丢弃；M2 设备侧 backfill 判据待 M2 形态 |
| A-08 | 端到端时延 | ◐ 实测完成：接收节拍 ≈1.01s、无积压；telemetry ts 为秒级 → 200ms 判据待 ms 时间戳专项 |
| A-09 | 引脚归属断言 | ✅ 两板实机通过（"A-09: pin ownership OK"） |
| A-10 | 栈水位 | ✅ **24h 连续：task_ovr=0、零复位**（A 上行 114,693 帧 0 丢；B 243,556 tx 全 ACK）（10-13→14） |
| A-11 | 双路改配置等价 | ◐ CAN CMD 腿 ✅（100× 写入全落地 + 值存活读回）；485 FC06 腿 ⏳ 未持有 USB-RS485 |
| A-12 | UI 不压制协议 | ✅ 双板目视（A 监控页 + B 三页 UI 翻页）+ 操作期 mb_to/mb_rt 增量=0（等价路径窗口） |
| A-13 | 掉电参数恢复 | ✅ 100× 写入 → 断电 → 上电：值 345 从 24C02 加载存活（复位标记 = PIN+BOR 真断电）（10-13） |
| A-14 | 日志环形恢复 | ✅ 上电扫描正确、序号跨环回单调（A 累计 2,700+ / B 300+ 条；含多次断电复测）（10-13→14） |

> **上板验收期间累计修复 9 个实机缺陷**（详见 [迁移说明 §18/§19](docs/迁移说明.md)）：
> heap 堆不足（malloc 死循环）· IRAM 越界配置 · 板载一键下载电路进 bootloader（option bytes 根治）·
> LOG 阻塞致 IWDG 4.5s 复位循环（v0.2 潜伏）· node 心跳缺失致 peer=0（v0.4 潜伏）·
> **主站 demo 写每 5s 覆写 temp_limit（v0.1 遗留）** · **24C02 地址未左移致 EEPROM 自 v0.1 从未通信** ·
> **影子网关默认开串口把板按住复位（v0.5 起从未真正工作）** · **telemetry JSON 多闭合括号致其从未到达 MQTT（v0.5 起）**。
> 实证打通：485 主从 / CAN 双向+心跳 / ISO-TP 快照 / CAN 改参闭环 / 参数掉电存活 / 影子网关全链路（warn=0）/ 24h 泡机。

## 验收收口与后续（v1.0）

**v1.0 已收口并打 tag**（2026-10-14）：14 项判据全部实机执行（10 项全绿 + A-07/A-08 限制记录 +
A-06 及 485 直读腿按"未持有硬件"留白）；上板验收期间修复 9 个实机缺陷；24h 泡机通过。
代码仓库：`https://github.com/1237liushengji/G474-Industrial-Terminal`（公开）。

**留作后续的增强项（不阻塞 v1.0）**：
1. 购入 USB-RS485 后补 A-06 双路比对与 A-05/A-11 的 485 直读腿（工具已就绪）；
2. telemetry 增加 ms 级时间戳字段 → 专项复测 A-08 的 P99≤200ms；
3. M2 形态（板载 ESP8266 + 设备侧 backfill）跑通 A-07 的完整补发判据（回填 ≤24s / LOG_BACKFILL 事件）。

**上板接线（丝印终审版，2026-10-10 实机验证）**：
- **跳线帽**：B 板插 485+CAN；A 板 M1 插 485+CAN / M2 插 **WiFi**+CAN（详见迁移说明 §17）
- **串口通道**：两板均用**板载 CH340K（USB 转串口，COM3=A / COM5=B）**，无需外接线。⚠️ 无论板载或
  外接 USB-TTL：**串口工具必须以 `dtr/rts=False` 静默打开**——实机实锤：默认方式打开会在端口开启
  期间把板**按住复位**（全程 0 字节，关闭瞬时才重启）；外接 USB-TTL 另需"只接 TXD→PA10、RXD→PA9、
  GND"三根（迁移说明 §18.3/§19.3）
- **ST-Link ×2 同时可用**：`STM32_Programmer_CLI -c port=SWD index=0/1` 分别操作 A/B 板；
  调试期读取用 `mode=hotplug`（不复位不打断，Normal 模式会软复位）
- **两板 option bytes 已固化 `nSWBOOT0=0 + nBOOT0=1`**（2026-10-11，实机排障产物）：
  板载 CH340 一键下载电路的 DTR→BOOT0 脉冲不再能把芯片带入系统 bootloader
  （此前表现为"打开串口即静默/CPU 进 bootloader"）。**副作用**：串口 ISP 下载
  （FlyMcu/硬件 BOOT0）失效，烧录统一走 ST-Link；如需恢复：`-ob nSWBOOT0=1`。
