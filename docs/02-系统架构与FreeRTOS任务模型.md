# 02 · 系统架构与 FreeRTOS 任务模型

> ⚠ **本文件受 [07 号文档](07-板级硬件核对与冲突修正.md) 修订（2026-09-21）**：
> §2 角色表中的 **USART3 归属**（RS485 vs ESP-12F 二选一）已作废，改为
> **RS485 = USART2 on PD5/PD6**、**ESP-12F = USART3 on PB10/PB11**、**CAN = FDCAN1 on PD0/PD1**；
> §5 决策 2 的 NVIC 表相应地 `FDCAN2_IT0` → `FDCAN1_IT0`、`USART3_IRQ` → `USART2_IRQ`（并新增 ESP 用的 `USART3_IRQ`）。
> **任务表（§3）、时序（§4）、其余移植决策（§5）全部保持有效。**
>
> **v0.2 实施注记（2026-10-07）**：任务表已落地于 `App/app_rtos.c`（wdt/can/mb/log/ui +
> 一个过渡 console_task，v0.5 并入 T4）。三处与本文的理想形态存在**记录在案**的差异：
> ① RS485 RX 暂时保持 ISR 逐字节回调直喂协议引擎（t3.5 依赖真时间戳，见迁移说明 §9.3）；
> ② SysTick 实际为最低优先级 (15,0)（FreeRTOS 移植约定，docs/04 §5 注记）；
> ③ 决策 6 的 `LOG_CACHE_CAP 512→128` 已随 v0.2 落地，v0.6 补发必须走 per-seq 直读接口，
> 不得依赖 `LOG_Read()`。
>
> **v0.4 实施注记**：**T3 bridge_task 已落地**（`Gateway/bridge.c`，prio 4/512w，契约帧收发 +
> 镜像 + 快照 + CMD/ACK）；T4 uplink 待 v0.5。ISO-TP 已实例化（UDS 与快照双 ctx，各自单任务
> 串行——见迁移说明 §11.2 注 6）。

## 1. 分层与目录

```
┌───────────────────────────────────────────────────────────────┐
│ App/       角色应用：gw_app.c(网关) / node_app.c(采集节点)      │  ← 只写编排，不写协议
├───────────────────────────────────────────────────────────────┤
│ Gateway/   协议桥：can_router.c  dp_table.c  bridge.c          │  ← 新增：契约投影 + 三方映射
│            uplink_port.c（上行抽象：M1=PC 帧 / M2=MQTT-C）      │
├───────────────────────────────────────────────────────────────┤
│ Network/   mqttc/ (MQTT-C 移植)  esp_at.c  at_window.c        │  ← 新增：仅 M2 编译
│            backfill.c（离线补发状态机）                          │
├───────────────────────────────────────────────────────────────┤
│ Protocol/  modbus.[ch] modbus_crc.[ch] modbus_register.[ch]    │  ← 原样搬入（HAL-free）
│            modbus_master.[ch] modbus_slave.[ch]                │
│            isotp.[ch] uds.[ch]  ← 原样搬入（HAL-free，时间注入）  │
├───────────────────────────────────────────────────────────────┤
│ BSP/       bsp_board_cfg.h(唯一配置点) bsp_uart bsp_rs485       │
│            can_drv.c(FDCAN) tick eeprom w25q128 lcd key led    │
├───────────────────────────────────────────────────────────────┤
│ Storage/   config.c(24C02) log.c(W25Q 环形) + 配额分区          │
│ Common/    types.h ring_buffer.c console.c bsp_printf.c fmt.c  │
└───────────────────────────────────────────────────────────────┘
```

保留既有 `Master/`、`Slave/` 目录名会与新角色模型混淆 → **重命名**：
`Master/master_main.c` → `App/mb_master_port.c`（仅 M1 的 A 板用），
`Slave/slave_main.c` → `App/node_app.c`（B 板主体），
`CAN双机通信/Core/Src/main.c` 的应用段 → `App/gw_app.c`。
**只做搬移与拆分，不改逻辑**，以便 git 历史可追（`git log --follow`）。

## 2. 双板角色

| | A 板 = GATEWAY | B 板 = NODE |
|---|---|---|
| 定位 | 协议汇聚 + 上云 + 现场监控 | 数据采集 + 就地 HMI + 参数持久化 |
| USART3 / PB10,PB11 | M1: RS485 主站 · **M2: ESP-12F** | **RS485 Modbus 从站** |
| FDCAN2 / PB12,PB13 | CAN 节点（主问方，收遥测/发命令） | CAN 节点（周期上报，收命令） |
| USART1 / PA9,PA10 | console + M1 上行帧 → PC | console + 诊断 |
| SPI1 / PA4-PA7 | W25Q128：事件日志 + **MQTT 待发缓存** | W25Q128：事件日志 |
| I2C1 / PA15,PB9 | 24C02：网关参数（broker/前缀/周期） | 24C02：现场配置（既有 6 字段） |
| LCD（软 SPI） | **监控页**：双链路状态 + MQTT 收发计数 + broker RSSI | **数据页/异常页/配置页**（既有 3 页，改动最小） |
| 按键 | K0 切页 / K1 手动补发 / WKUP 静默 | K0 切页 / K1 编辑 / WKUP 退出编辑（既有） |
| IWDG | 启用（v3.0 已修好 `hiwdg.Instance` 空指针陷阱） | 启用 |

**为什么测量量的"权威存储"在 B 板而不是 A 板**：B 板是 RS485 段上唯一可被 PC 主站直读的对象，
把契约放在 B 板使"直读值 vs 上云值"比对成立（00 号文档 M2 的关键验收红利）。A 板只做搬运与发布。

## 3. FreeRTOS 任务表

`configTICK_RATE_HZ = 1000`。优先级数值越大越高（CMSIS 约定）。

| # | 任务 | 优先级 | 栈(word) | 触发 | 阻塞点 | 主要职责 | 来源资产 |
|---|---|---|---|---|---|---|---|---|
| T1 | `can_task` | 5 | 512 | 通知(来自 ISR) + 10 ms 超时轮询 | 等 `xQueueCAN_Rx` / 发队列 | 收帧出队→路由；发帧；TxEvent 时延核算；bus-off 退避状态机 | `can.c` 全套（去主循环化） |
| T2 | `mb_task` | 5 | 512 | 通知(RX 字节/帧) + 1 ms | 等 UART 队列 | RS485 收发；`MB_Slave_Poll(now_us)` t3.5 判定；异常响应；统计 | `modbus_slave.c` + `bsp_rs485.c` |
| T3 | `bridge_task` | 4 | 512 | 10 ms 周期 + 事件 | 等两个队列 | `dp_table` 投影：寄存器→CAN 帧、CAN→寄存器；CMD 排队与 `cmd_id` 匹配 | 新增 |
| T4 | `uplink_task` | 3 | 1024 | 100 ms tick + 通知 | 等 socket/UART | M1: 帧化输出到 USART1 · M2: AT 状态机 + MQTT-C keepalive/QoS1/重连退避 | `Network/` 新增 |
| T5 | `log_task` | 2 | 512 | 队列(阻塞型) | 等 `xQueueLog` | 事件写 flash；`CONFIG_Save`；缓存补发读取 —— **所有慢/阻塞写集中于此** | `log.c` `config.c` |
| T6 | `ui_task` | 1 | 1024 | 200 ms 周期 + 脏标记 | 等 `xQueueKey` | LCD 分层重绘（静态层仅切页重绘，数值层 500 ms）；按键消抖 | `bsp_lcd.c` `bsp_key.c` `slave_main.c::UI_*` |
| T7 | `wdt_task` | 6 | 256 | 20 ms | — | 检查 T1–T6 心跳计数器是否推进，全推进才喂 IWDG；`task_ovr(0x1F)` 累加 | `can.c` 的 IWDG 段 |

设计准则：
1. **任何 ISR 只做 `xQueueSendFromISR` + `portYIELD_FROM_ISR`**，绝不在 ISR 里解析协议。
   v3.0"RxFifo0Callback 只入 ring、不解析 ID"的做法直接继承，只是 ring 换成 FreeRTOS 队列（或保留 SPSC ring + 队列通知，二选一，见 §5 决策）。
2. **协议层不感知 RTOS**。`MB_Slave_Poll(now_us)` / `MB_Master_Poll(now_us)` / `IsoTp_Task(now)` 保持"时间由调用者注入"的既有签名 ——
   这是 Modbus 工程能跑 PC 离线测试（`LoopbackTransport`）的原因，必须保住。
3. **T4 永不阻塞 T1/T2**。uplink 的所有等待都在自己任务内；MQTT 发送失败只影响自己的状态机。这是"上云不得阻塞轮询"约束的实现方式。
4. **T5 吸收所有阻塞**（flash 页写 ~ms 级、EEPROM ACK 轮询 ≤50 ms、`LOG_Init` 的 64 KB 线性扫描）。
   v2.0 把这三件事塞进协议循环是明确缺陷，FreeRTOS 化正好强制修正。

## 4. 关键时序

### 4.1 数据上行（M2）

```
B板 T6采样(1s, sample_period_ms=500 的整数倍) → MB_REG_SetHolding
   → T3 每 1 s 组 0x101(TELEMETRY_FAST) → T1 发送队列 → FDCAN2 → 空口 216 µs
   → TxEventFifo 确认 → can_tx_ack++
A板 FDCAN2 IT → 队列 → T1 出队 → T3 路由命中 0x101 → 写入 A 板镜像寄存器
   → T4 每 1 s 读镜像 → fmt_fixed → JSON → MQTT-C serialize → esp_at 透传 → Wi-Fi
PC mosquitto_sub → 校验
```
端到端时延预算：采样→CAN 5 ms(周期对齐) + 空口 0.3 ms + A 板排队 10 ms + JSON+MQTT 5 ms + Wi-Fi TCP ~15 ms + broker ~2 ms
= **典型 ≤ 40 ms，P99 ≤ 200 ms**（验收 A-08 用 `uptime_s` 与 PC 收包时间差统计）。

### 4.2 命令下行

见 03 号文档 §7。总预算：MQTT 到 A 板 ~20 ms + A 组帧 5 ms + CAN 空口 0.3 ms + B 执行 + 24C02 写(≤5 ms，走 T5) + ACK 回程
→ **`cmd_ack` P95 ≤ 100 ms**，A 板 500 ms 超时（含一次重发）。
注意：`CONFIG_Save` 在 T5 → **寄存器值与 `cmd_result` 立即生效并可回 ACK，flash 写在其后异步完成**。
若要求"存盘成功后才 ACK"，需把 ACK 延后到 `log_task` 回执 —— 这是一个明确的语义选择，v1 采用"生效即 ACK"，
理由：`Slave_ApplyConfigChange()` 在 v2.0 就是这个顺序（先应用后存盘），保持一致以免行为漂移。

### 4.3 RS485 方向切换（半双工正确性）

`RS485_SendFrame()` 现状（`bsp_rs485.c:97-108`）：置 `s_tx_active` → (若 `RS485_USE_DE_PIN`)DE=1+200 次空转 →
`BSP_UART_SendBytes()`（TXE 轮询）→ **`while(!(ISR&TC))` 忙等** → `ICR=TCCF` → DE=0。

FreeRTOS 化改造（v0.2 必做）：
```
发送：把整帧入 tx 队列 → 使能 TXE 中断逐字节 → 最后一字节写完后使能 TC 中断
     → TC ISR 中：清 TC(写 ICR) → 若为 RS485 则切回 RX 模式 → xTaskNotifyGive(mb_task)
```
理由：忙等 TC 在 RTOS 下会独占 CPU 到 T1/T2 饿死。TC 等待本身**必须保留**（半双工提前拉低 DE 会截断末位），
只是从忙等改中断。**t3.5 与 TC 时长量级不同，不可混为一谈**：t3.5@115200 = 334 µs（帧间隔判定），
末字节 TC 完成 = 一位时间 86.8 µs 量级，两者各管各的。

### 4.4 t3.5 帧间隔（沿用代码，不采用文档值）

```c
/* modbus.h: 11-bit 字符（1 起始 + 8 数据 + 1 校验/停止 + 1 停止） */
static inline uint32_t MB_T35_US(uint32_t baud){ return baud ? 38500000UL/baud : 10000; }
```
115200 → **334 µs**；9600 → 4010 µs。
`docs/04` 写的"10-bit 字符 / 350 µs"与 `docs/03` 的"115200→0.304 ms"均为**过期文档**，以代码为准（00 号文档 R-6）。
时基来源必须是 DWT（`bsp_tick.c::BSP_Tick_GetUs`），FreeRTOS 的 1 ms tick 精度不足以判 334 µs —— 见 §5 决策 3。

## 5. 移植决策（逐条落到代码位置）

### 决策 1：ISR ↔ 任务 交接用 FreeRTOS 队列，保留 SPSC ring 作无锁快路径

FDCAN 接收峰值速率在 500 kbit/s 下可达 ~4000 帧/s，`xQueueSendFromISR` 每帧约 1–2 µs，够用。
**统一改用队列**（`xQueueHandle can_rx_q`，长度 32，元素 = `can_msg_t` 16 B = 512 B），删除 `can.c` 内部 ring；
但保留 `Common/ring_buffer.c` 给 UART 字节流（队列逐字节交接开销大，ring + 单次通知更合适）。

### 决策 2：中断优先级（v0.2 的头号 killer）

```
FreeRTOS: configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5
规则: 只有抢占优先级 ≥ 5（数值更大、优先级更低）的 ISR 才可调 *FromISR API
现状: FDCAN2 = (1,0)  ✗ 违规   USART3 = (5,0)  △ 边界   USART1 = (3,0)  ✗ 违规
整改: SysTick = 0(由 FreeRTOS 接管, 最高) PendSV = configLIBRARY_LOWEST(15)
      FDCAN2 → (6,0)   USART3 → (7,0)   USART1 → (7,0)   TIM6(HAL时基) → (8,0)
      EXTI(按键) → (9,0)   IWDG 无需中断
```
副作用必须核算：FDCAN 优先级降到 6 后，500 kbit/s 下 FIFO 满溢出风险由 `rxOverruns` 计数监控
（验收 A-03 要求 `ovr == 0`）。若出现 ovr，说明出队路径太慢，降 T3/T4 频率而非改优先级。

### 决策 3：时基三轨制，职责分明

| 时基 | 用途 | 精度 | 来源 |
|---|---|---|---|
| `xTaskGetTickCount()` | 任务周期、超时、心跳、IWDG | 1 ms | FreeRTOS（SysTick） |
| `HAL_GetTick()` | HAL 内部超时（FDCAN/UART/SPI 驱动） | 1 ms | **重映射到 TIM6**（覆写 `HAL_InitTick/TickInc/PriorityGroupConfig`） |
| `BSP_Tick_GetUs()` | t3.5、Modbus 事务计时、CAN 时延统计 | ~µs（DWT CYCCNT @150 MHz） | `bsp_tick.c` 原样保留 |

三者不可互相顶替。典型错误是"HAL_GetTick 不推进了" —— 表现是 FDCAN/UART 的所有 HAL 超时瞬间返回、
状态机疯狂重入（R-2）。验收项 A-02 专测此项。

### 决策 4：console 与上行链路共用 USART1 的仲裁

M1 下 USART1 既是 console（文本行）又是上行帧（二进制）→ 必须帧定界。规定：
- console 行：以 `\r\n` 结尾的可打印 ASCII，首字节 ∈ 0x20–0x7E。
- 上行帧：以 `0xAA 0x55` 双字节魔数开头 + 长度 + payload + CRC16，**首字节 0xAA 与 console 文本互斥**。
- 发送侧：单一 `xQueueUart1Tx` + `uxItemSize=1` 不可用，改为 mutex 保护的 `BSP_UART_Send()`；
  `BSP_UART_Printf` 的 `static char txBuf[96]` 非重入问题 → 改为 `xTaskGetTickCount()` 派生的 per-task 静态池（4 × 128 B）。
  **不使用 `vsnprintf` 浮点**（同 03 号文档 §5.2）。

### 决策 5：ESP-12F AT 窗口仲裁（M2，R-3）

```
states: RESET → AT_READY → WIFI_JOIN → TCP_OPEN → [PASS_THRU] → (err) → RECOVER
PASS_THRU 下禁止发 AT。需要 AT 时：
  1. 静默 ≥ 21 bit 时间（ESP-AT 的 +++ 守卫，实际用 10 ms 无字节）
  2. 发 "+++"（不带换行），等 1 s 期待 "OK"
  3. 超时未 OK → 判定链路已断 → 直接 RECOVER（重发 AT+CIPCLOSE / 必要时 AT+RST）
```
MQTT-C 的 `mqtt_deserialize_loop_wait` 需要"可阻塞的 recv"→ 用 FreeRTOS 队列 + `MQTT-YIELD`/自定义
`read/write` 回调实现：`read()` 从 `esp_rx_ring` 取，取不到就 `xQueueReceive(..., pdMS_TO_TICKS(10))`。
**MQTT keepalive 与 AT 状态机不能互相等待** —— keepalive 由 uplink_task 自己按 tick 触发，
不依赖 AT 应答（透传下 AT 无应答是正常态）。

### 决策 6：`Storage/log` 双配额分区（R-5）

同一 64 KB 区内按记录类型区分，不切物理地址段（切段要改 `log.c` 的轮转/恢复算法，风险高）。
规则：`LOG_WriteEvent()` 与 `LOG_WritePending()` 共用环形游标，但各自维护配额计数；
写入前若"本类已用 ≥ 1024 条"则先淘汰本类最旧（通过 `event` 字段线性定位），
上电恢复沿用既有 `seq` 连续性扫描，不改算法。
**注意既有 `LOG_Read(index)` 返回的是"缓存内最旧"，缓存只有 `LOG_CACHE_CAP=512` 条 →
补发读取不能依赖 `LOG_Read`，必须新增按 `seq` 直读 flash 的接口。** 这是 v0.6 的实打实工作项。
同时 `LOG_CACHE_CAP` 由 512 降到 128 以回收 RAM（512×32 B = 16 KB，见 §6）。

### 决策 7：`App/` 必须补上真实数据源

v2.0 的 `temp/humi/volt/curr` 是**硬编码演示值**，`s_input[]`（ADC1/ADC2/内核温度/VREFINT）恒为 0 且 ADC 从未初始化。
新系统若继承这一点，上云链路"看起来通"但数据是假的 —— 这是演示工程与工业系统的分水岭，必须显式补：

| 数据点 | 真实源 | 工作量 | 备注 |
|---|---|---|---|
| `mcu_t(0x21)` `vref(0x22)` | ADC1 内部通道（内核温度、VREFINT） | 小 | 无需外部硬件，先打通采集路径 |
| `adc1_raw(0x20)` | ADC1 外部通道（板载电位器/空闲引脚） | 小 | 需查板子是否引出 ADC 输入；若无则挂到 LCD 背光 PWM 回采等，宁缺勿假 |
| `temp/humi` | 无板载环境传感器 | — | **保持"仿真源"并在 meta 中标 `"sim":true`**，不得伪装成实测量 |
| `volt/curr` | 无板载电量测 | — | 同上 |

**契约要求**：`meta` payload 增 `"sim":["temp","humi","volt","curr"]` 数组，显式声明哪些点是仿真的。
这条看起来是文档洁癖，但它防止了"演示值被当成实测值写进验收报告"这类真实事故。
