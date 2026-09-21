# 基于 STM32G474 的 Modbus RTU 工业通信系统

双节点（Master / Slave）RS485 主从式工业通信系统，自研 Modbus RTU 协议栈，配套 PC Python 测试工具。

- MCU：STM32G474VET6 ×2（Genbotter 开发板，两块板硬件一致，外设齐全）
- 链路：RS485 半双工总线（自动方向电路；DE 引脚可选，见 `BSP/bsp_board_cfg.h`）
- 工具链：Keil MDK-ARM（ARMCLANG V6.14）+ STM32CubeG4 HAL
- 基座例程：LED 闪烁工程（`Core/`、`Drivers/`、`MDK-ARM/`），已作为本仓库 v0.1 基线

## 系统框图

```
 PC (Python Modbus 测试工具)
            │ USB-UART
   ┌────────▼─────────┐          ┌──────────────────┐
   │ STM32G474 Master │  RS485   │ STM32G474 Slave  │
   │ 轮询/读写/重试    │◄───────►│ 从站协议栈        │
   │ 超时/统计         │   A/B    │ 寄存器/参数/日志  │
   └──────────────────┘          └─┬───┬───┬───┬────┘
                                   LCD 24C02 W25Q128 按键
```

- A 板（Master）：周期轮询、读写、超时重试、通信统计、故障指示
- B 板（Slave）：帧解析、CRC、异常响应、寄存器管理、参数掉电保存、日志、LCD 状态显示

## 目录结构

```
Modbus-RTU/
├── Core/                  CubeMX 生成（main/gpio/it/system）
├── Drivers/
│   ├── CMSIS/             内核与设备头文件
│   ├── STM32G4xx_HAL_Driver/  ST HAL 驱动
│   └── User/              用户驱动（led 等，BSP 自初始化风格）
├── Protocol/              modbus / modbus_master / modbus_slave / modbus_crc / modbus_register
├── BSP/                   bsp_uart/rs485/tick/eeprom/w25q128/lcd/key + bsp_board_cfg(唯一配置点)
├── Storage/               config（24C02 参数）/ log（W25Q128 环形日志）
├── Common/                ring_buffer / types / 平台无关公共代码
├── App/                   应用层（预留；角色逻辑在 Master/ 与 Slave/）
├── Master/                master_main（A 板角色入口）
├── Slave/                 slave_main（B 板角色入口，LCD 三页 UI）
├── Tools/
│   ├── python/            PC 测试工具 + 离线自测 + 压力测试
│   └── verification/      CRC 交叉验证 + Modbus 黄金测试向量
├── docs/                  设计文档（01~10）
├── MDK-ARM/               Keil 工程（G474.uvprojx）
└── README.md
```

> 说明：CubeMX 例程未含 `.ioc`，全部外设初始化采用「BSP 自初始化」模式
> （参照 `Drivers/User/Src/led.c` 与 `Drivers/User/Inc/led.h`），不依赖 CubeMX
> 重新生成，且可合入你自己的 CubeMX 工程（见 `docs/09-CubeMX合入指南.md`）。
> 源码一律 UTF-8、注释以英文为主；设计文档用中文写在 `docs/`。

## 双板角色编译

固件角色由编译期宏区分（`BSP/bsp_board_cfg.h` 或 Keil Define）：

| 宏 | 值 | 用途 |
|----|-----|------|
| `MODBUS_NODE_ROLE` | `0` | A 板主站固件 |
| `MODBUS_NODE_ROLE` | `1` | B 板从站固件（默认） |

常用配置开关（均在 `BSP/bsp_board_cfg.h`）：

| 宏 | 默认 | 说明 |
|----|------|------|
| `RS485_RX_MODE` | 0 | 0=中断逐字节（默认，真机验证），1=DMA+IDLE+RingBuffer |
| `RS485_USE_DE_PIN` | 0 | 自动收发=0；DE GPIO 控制=1（填引脚） |
| `LCD_CTRL_SELECT` | 0 | 0=ST7789(240x280) 1=ILI9341 2=ST7735 |

## 版本路线（Git tag：v0.1 … v2.0）

| 版本 | 内容 | 状态 |
|------|------|------|
| v0.1 | 工程初始化：例程上移、骨架、设计文档 | ✅ |
| v0.2 | UART/RS485 驱动（USART3，TC 等待切向） | ✅ |
| v0.3 | Modbus CRC16（查表+逐位，PC 交叉验证） | ✅ |
| v0.4 | Slave 0x03 + 寄存器表 + t3.5 帧状态机 | ✅ |
| v0.5 | Slave 0x01 / 0x06 / 0x10 | ✅ |
| v0.6 | 异常响应 + 统计 + 黄金测试向量 | ✅ |
| v0.7 | Master 协议机 + 双角色 main 架构 | ✅ |
| v0.8 | 超时 / 重试 / 统计 / 故障指示 | ✅ |
| v0.9 | 24C02 参数掉电保存（版本+CRC，修改即存） | ✅ |
| v1.0 | LCD 状态显示（三页 UI + 按键编辑保存） | ✅ |
| v1.1 | W25Q128 环形日志（扇区轮转 + 上电恢复） | ✅ |
| v1.2 | Python 测试工具（CRC/master/simulator/CLI） | ✅ |
| v1.3 | 压力 / 稳定性测试 | ✅ |
| v1.4 | DMA + RingBuffer 优化 | ✅ |
| v2.0 | 最终整合与验收 | ✅ |

## 快速开始

1. **烧录**：Keil 打开 `MDK-ARM/G474.uvprojx`；
   - A 板：Define 加 `MODBUS_NODE_ROLE=0` 编译烧录
   - B 板：默认 SLAVE 编译烧录
2. **连板**：两板 RS485 A-A、B-B 相连（自动方向电路无需 DE 线）；
   上电后 Master 蓝灯随轮询脉冲，从站绿灯随响应闪。
3. **对码**：B 板 LCD 首页应显示 ID/波特率与 RX/TX 计数增长。
4. **PC 工具**：USB-RS485 适配器并接总线（与两板 A-A、B-B 并联）；
   无适配器时可直接把 USB-UART 的 TTL 电平跨接在任一块板 MCU 侧的
   USART3 引脚上（仅调试从站时用，注意收发器方向）：
   ```bash
   python Tools/python/modbus_tool.py --port COMx --baud 115200
   > read 1 40001 5
   > write 1 40010 350
   > stats
   ```
   无硬件时可 `--sim` 用内置从站模拟器；回归 `python Tools/python/run_tests.py`，
   压力测试 `python Tools/python/pressure_test.py --count 10000`。

## 设计文档（docs/）

| # | 文档 | 内容 |
|---|------|------|
| 01 | 系统架构设计 | 分层、数据流、目录职责 |
| 02 | 板级硬件配置 | 引脚表、待确认项、丝印解读 |
| 03 | 寄存器映射表 | 保持/输入/线圈唯一数据契约 |
| 04 | 状态机与帧时序 | 从站/主站 FSM、t3.5 |
| 05 | 协议帧格式与异常码 | 功能码/异常/CRC 向量 |
| 06 | 测试计划与验收标准 | N/E/P 系列用例 |
| 07 | 版本路线图 | Git 里程碑 |
| 08 | Storage 设计 | 24C02 参数区、W25Q128 日志区 |
| 09 | CubeMX 合入指南 | 源码合入已有 CubeMX 工程步骤 |

## 验收状态（对照 specs §21）

| 项目 | 实现 | PC 离线验证 | 待真机 |
|------|------|:---:|:---:|
| 10000 帧连续无丢帧 | ✅ pressure_test | ✅ 0 lost | 真机压力 |
| 人为 CRC 错误检测 | ✅ CRC + E7 向量 | ✅ | ✅ |
| 从站断电 → 超时重试 | ✅ v0.8 | 模拟超时 | 真机拔线 |
| 异常响应 0x01/02/03 | ✅ | ✅ 11 组向量 | ✅ |
| 参数掉电恢复 | ✅ v0.9 | 结构验证 | 24C02 真机 |
| 错误写 W25Q128 | ✅ v1.1 | 代码/编译 | 真机日志 |
| LCD 实时显示 | ✅ v1.0 | 编译 | LCD 真机 |
| Python 工具读写 | ✅ v1.2 | ✅ 离线全过 | 串口真机 |
