# STM32G474 工业数据采集终端（RS485 + CAN + Modbus + MQTT）

基于两块 DRG ST-10 STM32G474VET6 开发板，把既有的 **CAN 双机通信（v3.0）** 与
**Modbus-RTU 双节点（v2.0）** 两个已验证工程，演进为一套带本地 HMI、离线缓存与 MQTT 上云能力的
边缘采集网关。当前状态：**v0.4 已落地（CAN↔寄存器数据桥：can_router 表驱动 + 契约帧收发 +
ISO-TP 快照 + CMD 幂等 + 镜像），4 Target 全量重建 0 Error / 0 Warning，离线回归全绿；
尚未上板验证**（上板验收项见 docs/迁移说明 §11.3）。

## 系统框图

```
              PC（实验室内）: Mosquitto 2.1.2 @ E:\Edge\mosquitto-2.1.2 :1883
              Tools/python：Modbus 主站/观测者 · MQTT 订阅校验 · 契约哈希校验
                    ▲                                     ▲
        Wi-Fi 2.4G  │ (设备侧 MQTT-C)          USB-TTL  │ (console + 可选影子网关)
                    │                                     │
 ┌──────────────────┴──── A 板 · GATEWAY ────────────────┴────────────────────┐
 │ USART2/PD5,PD6 → RS485 模块 → Modbus RTU 主站 ═══ RS485 总线(A/B) ═══┐      │
 │ USART3/PB10,PB11 → ESP-12F #1 → MQTT-C 设备侧上云                     │      │
 │ FDCAN1/PD0,PD1 → CAN 收发器 ─────── CAN 500 kbit/s ──────────────────┼──┐   │
 │ SPI2/PB12-PB15 → 板载 W25Q128 16MB：事件日志 + MQTT 待发缓存           │  │   │
 │ I2C1/PA15,PB9 → 24C02（网关参数）                                     │  │   │
 │ LCD(ST7789 1.69″) + LED PE0/PE1 + KEY PA0/PA2/PA3 + IWDG             │  │   │
 └───────────────────────────────────────────────────────────────────────┘  │   │
                                                                            │   │
 ┌──────────────────────── B 板 · NODE ──────────────────────────────────────┴───┴──┐
 │ USART2/PD5,PD6 → RS485 模块 → Modbus RTU 从站 ═══════════════════════════ RS485 总线 │
 │ USART3/PB10,PB11 → ESP-12F #2（可选：第二条上云通道）                              │
 │ FDCAN1/PD0,PD1 → CAN 收发器；SPI2 → 板载 16MB Flash；I2C1 → 24C02                  │
 │ LCD + LED + KEY + 本地采集 + 离线自治（A 板掉线时仍独立工作）                        │
 └────────────────────────────────────────────────────────────────────────────────────┘
```

**A 板与 B 板结构同构**：同一套固件、同一份引脚表，只靠 `NODE_ROLE` 区分主从。

## 三条先读结论

1. **板载 W25Q128 与 FDCAN2 抢 PB12/PB13，二者只能留一个。** 板载 Flash 接在 `PB12(CS#)/PB13(CLK)/PB14(DO)/PB15(DI)`（SPI2），
   而数据手册 Table 12 明确 `PB12=FDCAN2_RX`、`PB13=FDCAN2_TX`。裁决：**CAN 改用 FDCAN1 on PD0/PD1（J1 19/20）**，
   保住板载 16MB Flash。详见 [docs/07](docs/07-板级硬件核对与冲突修正.md) §4 C1。
2. **板上没有 RS485 收发器、没有 WiFi 座、没有 USB 转串口芯片、没有 EEPROM。** 原理图器件清单只有
   MCU / AMS1117 LDO / W25Q128 / USB-C / LCD 排线座 / SWD 座 / IO 引出排针 / LED / 按键 / 晶振。
   RS485 与 ESP-12F 都是**外接模块走排针接线** —— 所以原先"RS485 与 WiFi 互斥"不是死结：
   **RS485 改接 USART2(PD5,PD6)，ESP-12F 留在 USART3(PB10,PB11)，两条链路并存。**
   注意 USART2 的默认对 PA2/PA3 被按键占用，故落到 PD5/PD6。详见 [docs/07](docs/07-板级硬件核对与冲突修正.md) §4 C2/C3。
3. **两个既有工程真正被硬件验证过的部分比想象少。** Modbus v2.0 的验收矩阵里，从站掉电、24C02、W25Q128、
   LCD、串口 五项标的是"待真机"。而且它的 Flash 驱动写在 `SPI1/PA4-PA7` —— **那组引脚上没有 Flash 芯片**，
   板载 Flash 在 `SPI2/PB12-PB15`。这解释了它为何一直是"待真机"。详见 [docs/06](docs/06-资产复用与冲突消解.md) §3。

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

## 目录结构（v0.2：FreeRTOS 已入仓，任务集在 App/app_rtos.c）

```
App/        app_rtos(T1-T7 任务集+心跳+故障钩子) mb_master_port gw 角色 node_app 节点角色+UI
Gateway/    can_router.c(表驱动路由) bridge.c(CAN↔寄存器桥+镜像+快照+CMD/ACK)  （uplink_port 待 v0.5）
Network/    mqttc/ + esp_at + at_window + backfill        （待 v0.6）
Protocol/   modbus_*（64 寄存器/FC04/0x0F 空洞）  isotp(多实例)  uds  can_contract.h(契约帧)
BSP/        app_board_cfg.h  bsp_console(USART1)  bsp_uart(RS485 TXE/TC 中断)  rs485 tick eeprom w25q128 lcd key led iwdg can_drv(队列+4 条白名单滤波)
Storage/    config.c(24C02)  log.c(W25Q 256KB 环形)  log_service.c(队列化,log_task)
Common/     types.h  ring_buffer.c  bsp_printf.c(4x128B/任务+互斥)  console.c  dp_table.c(契约唯一源)  fmt.c
Middlewares/FreeRTOS-Kernel/  V10.5.1 裁剪版（tasks/list/queue/port/heap_4, GCC/ARM_CM4F 端口）
Tools/python/    modbus/ can/ mqtt/ contract_check.py + dp_table.json/  离线回归 + 契约校验 + 影子网关
Tools/verification/ crc_check.js  modbus_vectors.js  modbus_vectors.json   黄金向量（11 条，冻结）
Tools/ci/        check_config_points.sh                  A-01 单一配置点检查（v0.2 新增）
docs/       本方案文档集 + 后续实验记录
MDK-ARM/    G474.uvprojx（4 Target）+ build_check.log
```

## 环境事实（已实测）

| 项 | 值 |
|---|---|
| MCU | STM32G474VETx，512 KB Flash / 128 KB SRAM，SYSCLK 150 MHz（HSE 8 MHz→PLL /2 ×75 /2），APB 全 /1 |
| RTOS | FreeRTOS-Kernel **V10.5.1**（`Middlewares/`，GCC/ARM_CM4F 端口适配 ARMCLANG V6.14）；HAL 时基 = TIM6，SysTick 归内核 |
| IDE | Keil MDK，ARMCLANG V6.14，DFP `STM32G4xx_DFP 2.2.0`，HAL **V1.2.3**（锁版本，勿顺手升级） |
| PC | Windows 11 22631；Python 3.14.7（`pyserial` / `pypdf` / `pypdfium2`）；Node v26.7.0 |
| Broker | Mosquitto 2.1.2，`E:\Edge\mosquitto-2.1.2` |
| 手柄串口 | 当前 0 个 COM 口（板子未接线） |
| **已持有** | USB-TTL 串口线 ✅、外接 24C02 模块 ✅、外接 W25Q 模块 ✅、ESP-12F ×2 ✅ |
| **未持有** | USB-RS485 ❌（仅影响 A-06/A-11 的验收自动化，**不阻塞运行**）、USB-CAN ❌（可选） |
| 容器环境 | **无容器引擎**，且本项目不需要。已装的是 `Docker Sandboxes 0.43.0`（`Docker.sbx`，AI 代理隔离工具，`sbx.exe` 在 `%LOCALAPPDATA%\DockerSandboxes\bin`，非容器引擎）。broker 走原生 Mosquitto |

## 下一步

**v0.4 代码层已收口**（[docs/迁移说明 §11](docs/迁移说明.md)：落地清单 / 偏离注记 / 证据）。
下一步两件事：

1. **上板验证 v0.4 实机行为**（无硬件不可替代）：B 板 TFT 改 `temp_limit` → A 板镜像 ms~秒级同步
   （console `stats` 看 snap ok 计数）；console `cmd 9 300` → 观察 `cmd_ack`、B 板 24C02 落盘、
   断电重启仍在；顺手收口 v0.2/v0.3 遗留验收（A-02/03/04/09/10、A-05 `--port`）。
2. **v0.5 推进（无需硬件可编码）**：M1 影子网关——A 板 USART1 二进制帧协议（`0xAA55` 定界）
   + `Tools/python/mqtt_shadow_gw.py` + Mosquitto 联调。

**上板前三根线**（相对 v0.1 不变）：
- CAN 收发器：`PB12/PB13` → **`PD0/PD1`**（J1 pin 19/20）
- RS485 模块：`PB10/PB11` → **`PD5/PD6`**（J1 pin 24/25）
- ESP-12F：保持 **`PB10/PB11`**（J2 pin 20/17），跳线帽归 WiFi —— 与 RS485 不再共针
