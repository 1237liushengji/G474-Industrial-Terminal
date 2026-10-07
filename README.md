# STM32G474 工业数据采集终端（RS485 + CAN + Modbus + MQTT）

基于两块 DRG ST-10 STM32G474VET6 开发板，把既有的 **CAN 双机通信（v3.0）** 与
**Modbus-RTU 双节点（v2.0）** 两个已验证工程，演进为一套带本地 HMI、离线缓存与 MQTT 上云能力的
边缘采集网关。当前状态：**v0.7 已落地（双板 TFT：A 板监控页 + B 板链路页 + 按键手动
snapshot/补发 + A-09 引脚断言），4 Target 全量重建 0 Error / 0 Warning，离线回归全绿；
v1.0 仅差硬件验收**（清单见 docs/迁移说明 §14.2 与 README 验收矩阵）。

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
| 手柄串口 | 当前 0 个 COM 口（板子未接线） |
| **已持有** | USB-TTL 串口线 ✅、外接 24C02 模块 ✅、外接 W25Q 模块 ✅、板卡实物丝印照片 ✅ + MCU 数据手册 ✅（板载 ESP8266 焊装、24C02/W25Q128 板上都有） |
| **未持有** | USB-RS485 ❌（仅影响 A-06/A-11 的验收自动化，**不阻塞运行**）、USB-CAN ❌（可选） |
| 容器环境 | **无容器引擎**，且本项目不需要。已装的是 `Docker Sandboxes 0.43.0`（`Docker.sbx`，AI 代理隔离工具，`sbx.exe` 在 `%LOCALAPPDATA%\DockerSandboxes\bin`，非容器引擎）。broker 走原生 Mosquitto |

## 验收矩阵（v1.0 状态 · 2026-10-07）

> 代码层全部就位；**⏳ = 只差实机执行**。实机步骤由 `python Tools/ci/run_acceptance.py`
> 逐项引导（离线段已全绿：A-01 / 回归 / 契约）。判据数字见 docs/05 §2。

| ID | 项目 | 状态 |
|---|---|---|
| A-01 | 单一配置点 | ✅ 脚本 0 命中 |
| A-02 | 双时基一致 | ⏳ 固件就绪（TIM6+FreeRTOS），待 10 min 实机 |
| A-03 | CAN 丢帧核算 | ⏳ 固件就绪，待 30 min 实机 |
| A-04 | bus-off 恢复 | ⏳ 退避状态机就绪，待短接注入 |
| A-05 | 契约哈希三方 | ◐ 离线两腿 ✅（C↔JSON、E71B）；实机 `--port` 腿待跑 |
| A-06 | 双路交叉比对 | ⏳ `mqtt_verify.py` 就绪；离线链路 E2E 已验证 |
| A-07 | 断 broker 补发 | ⏳ backfill/pending 就绪，待实机 |
| A-08 | 端到端时延 | ⏳ 待实机 |
| A-09 | 引脚归属断言 | ◐ 断言已入 BSP 并就位；实机应打印 "A-09: pin ownership OK" |
| A-10 | 栈水位 | ⏳ 心跳/水位采集就绪，待 24 h |
| A-11 | 双路改配置等价 | ⏳ 同一 dirty→24C02 路径就绪，待实机 |
| A-12 | UI 不压制协议 | ⏳ 分层刷新就绪，待实机 |
| A-13 | 掉电参数恢复 | ⏳ 待实机（v2.0 策略原样继承） |
| A-14 | 日志环形恢复 | ⏳ 扫描/环形就绪，待实机 |

## 下一步

**v0.6 代码层已收口**（[docs/迁移说明 §13](docs/迁移说明.md)：落地清单 / 决策 / 证据）。
下一步两件事：

1. **上板验证（无硬件不可替代，即 v1.0 缺口）**：`python Tools/ci/run_acceptance.py` 会先跑全部
   离线回归，再按 README"验收矩阵"逐项引导实机步骤（TFT 改参镜像、`cmd 9 300`→24C02、
   `mqtt_verify.py` A-06、断 broker 补发 A-07、时延 A-08、A-12 UI、A-09 断言实机）。
2. **v0.8 已随本版一并交付**：一键验收脚本 + README 验收矩阵表（见下）；**tag v1.0 等待
   硬件全绿后打**（不打"纸面 v1.0"）。

**上板前三根线**（相对 v0.1 不变）：
- CAN 收发器：`PB12/PB13` → **`PD0/PD1`**（J1 pin 19/20）
- RS485 模块：`PB10/PB11` → **`PD5/PD6`**（J1 pin 24/25）
- ESP-12F：保持 **`PB10/PB11`**（J2 pin 20/17），跳线帽归 WiFi —— 与 RS485 不再共针
