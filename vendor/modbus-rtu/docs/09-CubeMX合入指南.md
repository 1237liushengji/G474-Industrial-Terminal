# 09 CubeMX 工程合入指南

> 本仓库的开发基座是 `G474` LED 例程（CubeMX 风格，但**无 .ioc 文件**），
> 所有外设均采用 **BSP 自初始化**（驱动内部自行开时钟/配 GPIO），
> 因此你可以把本仓库的源码层**整体合入你自己现有的 CubeMX G474 工程**，
> 不必重新用 CubeMX 生成外设代码。

## 1. 合入内容

需要合入的目录（其余为 CubeMX/CMSIS/文档，不必复制）：

```
App/  Protocol/  BSP/  Storage/  Common/  Master/  Slave/
```

把以上目录复制到你的 CubeMX 工程根目录下（与 Core/、Drivers/ 平级）。

## 2. 头文件搜索路径（Keil）

工程选项 → C/C++ (AC6) → Include Paths 追加：

```
../Core/Inc;                       (已有)
../Common;../BSP;../Protocol;../Master;../Slave;../Storage
```

## 3. 编译宏 Define

`Options for Target -> C/C++ -> Define` 追加：

| 宏 | 值 | 说明 |
|----|-----|------|
| `MODBUS_NODE_ROLE` | `0` | A 板：Master 固件 |
| `MODBUS_NODE_ROLE` | `1` | B 板：Slave 固件（默认，可省略） |

出两版固件时改这个宏重新编译即可（或建两个 Target）。

## 4. 需要加入工程的源文件（.c）

**必须**（协议/公共）：

- `Common/ring_buffer.c`
- `Protocol/modbus_crc.c`
- `Protocol/modbus_register.c`
- `Protocol/modbus_slave.c`
- `Protocol/modbus_master.c`
- `Storage/config.c`
- `Storage/log.c`
- `Master/master_main.c`（两版固件都加，内部按角色宏分支）
- `Slave/slave_main.c`

**BSP 驱动**（按需；建议全加，编译尺寸很小）：

- `BSP/bsp_uart.c`、`bsp_rs485.c`、`bsp_tick.c`
- `BSP/bsp_eeprom.c`（24C02 参数）、`bsp_w25q128.c`（日志 Flash）
- `BSP/bsp_lcd.c`、`bsp_key.c`（B 板从站 UI）

## 5. CubeMX 需要配置/保留的外设

| 外设 | 用途 | 说明 |
|------|------|------|
| USART3 (PB10/PB11, AF7) | RS485 | 本仓库驱动自行初始化，CubeMX 中可**不**生成 USART3，避免重复 |
| I2C1 (PA15/PB9) | 24C02 | bsp_eeprom 自初始化 |
| SPI1 (PA4~PA7) | W25Q128 | bsp_w25q128 自初始化 |
| GPIOB/GPIOD 等 | LCD/按键/DE | bsp_lcd / bsp_key / bsp_rs485 自初始化 |

> 注意：`Core/Inc/stm32g4xx_hal_conf.h` 中需要打开 `HAL_I2C_MODULE_ENABLED`、
> `HAL_SPI_MODULE_ENABLED`（DMA/TIM/GPIo 默认已开）。若你的工程已改过该文件，
> 照仓库中的版本核对即可。

## 6. main.c 的 USER CODE 接缝

保持你 CubeMX 生成的 `main()` 不动，在 USER CODE 区加入：

```c
/* USER CODE BEGIN 2 */
LED_Init();
BSP_Tick_Init();
#if (MODBUS_NODE_ROLE == NODE_ROLE_MASTER)
  Master_Main();      /* 永不返回 */
#else
  Slave_Main();       /* 永不返回 */
#endif
/* USER CODE END 2 */
```

需要 include：`bsp_led.h`(led.h) / `bsp_tick.h` / `master_main.h` / `slave_main.h` / `bsp_board_cfg.h`。
CubeMX 重新生成不会覆盖 USER CODE 区，合入后可长期维护。

## 7. 常见问题

1. **串口没反应**：先确认 `RS485_USE_DE_PIN`（bsp_board_cfg.h）。
   - 板上无 DE 引脚（自动收发）→ 保持 0；
   - 若你的板子 DE/RE 需 GPIO 控制 → 置 1 并填真实引脚。
2. **LCD 白屏/花屏**：确认 `LCD_CTRL_SELECT` 与实物控制器一致（0=ST7789 / 1=ILI9341 / 2=ST7735），
   以及初始化引脚方向（如 RGB 顺序反色需改 MADCTL）。
3. **收发方向对调**：PB10/PB11 只可能是 USART3 TX/RX 的 AF7 组合，
   若丝印让你困惑，以 AF 表为准（见 docs/02 §3）。
4. **接收模式切换**：`RS485_RX_MODE` = 1 走 DMA+IDLE（默认），= 0 走中断逐字节；
   两者协议层 API 兼容，可随时切换排查。
5. **Keil 中文注释乱码**：仓库 C/H 源码统一 UTF-8 且注释为英文；
   如你在 Keil 中另加中文注释，请用系统 ANSI(GB2312) 保存。
