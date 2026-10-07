/**
  ******************************************************************************
  * @file    app_board_cfg.h
  * @brief   THE single point of change: board hardware + node role + link params
  * @note    Board : Genbotter / DRG ST-10 STM32G474VET6
  *          Source: docs/07-板级硬件核对与冲突修正.md (authoritative)
  *                  docs/04-板级配置点与资源预算.md
  *          Rule  : no bare pin / baudrate / AF literal outside BSP/ (accept A-01)
  *
  *  Naming note: docs/07 §6 uses RS485_TX_GPIO_PIN / FLASH_CS_PORT_PIN for
  *  readability; this file keeps the names the existing BSP code already uses
  *  (RS485_TX_PIN / FLASH_CS_PORT + FLASH_CS_PIN) so that v0.1 does not have to
  *  touch bsp_uart.c / bsp_rs485.c. Semantics are identical.
  ******************************************************************************
  */
#ifndef __APP_BOARD_CFG_H
#define __APP_BOARD_CFG_H

#include "main.h"   /* HAL handle types, GPIO symbols */

/*==========================================================================*/
/* 0. Wiring notice - read before the first flash of this firmware           */
/*==========================================================================*/
/* v0.2 runs FreeRTOS (see Core/Inc/FreeRTOSConfig.h).  The BOARD WIRING must
 * be moved to match the new pin map before anything will talk:
 *
 *   CAN transceiver : PB12/PB13  ->  PD0/PD1   (J1 pins 19/20)
 *   RS485 module    : PB10/PB11  ->  PD5/PD6   (J1 pins 24/25)
 *   ESP-12F         : stays on PB10/PB11       (J2 pins 20/17)
 *
 * The RS485/WiFi jumper conflict disappears with this move - they no longer
 * share pins.  Sequence and rationale: docs/07 section 9.
 *
 * v0.2 status: console (USART1 + Common/bsp_printf.c) and UDS (Protocol/uds.c)
 * are now part of the build; the W25Q log area grew to 256 KB and its
 * power-up scan runs inside log_task.  Has NOT been run on hardware yet -
 * acceptance A-02/A-03/A-04/A-09/A-10 are the v0.2 exit conditions (docs/05). */

/*==========================================================================*/
/* 1. Node role / form factor (injected by Keil target Define - do not edit)  */
/*==========================================================================*/
/*  Keil targets and their Define:
 *    A_GW_M1   : NODE_ROLE=0, FORM_MODE=1
 *    A_GW_M2   : NODE_ROLE=0, FORM_MODE=2
 *    B_NODE_M1 : NODE_ROLE=1, FORM_MODE=1
 *    B_NODE_M2 : NODE_ROLE=1, FORM_MODE=2                                 */
#ifndef NODE_ROLE
#define NODE_ROLE               0           /* fallback: gateway */
#endif

#define NODE_ROLE_GATEWAY       0
#define NODE_ROLE_NODE          1

/* v0.1 compatibility aliases: existing code still says MODBUS_NODE_ROLE /
 * NODE_ROLE_MASTER / NODE_ROLE_SLAVE. Retired from v0.4 onwards. */
#define NODE_ROLE_MASTER        NODE_ROLE_GATEWAY
#define NODE_ROLE_SLAVE         NODE_ROLE_NODE
#ifndef MODBUS_NODE_ROLE
#define MODBUS_NODE_ROLE        NODE_ROLE
#endif

/* Form factor: M1 = PC shadow gateway publishes MQTT;  M2 = device-side MQTT-C
 * over ESP-12F.  Both use identical wiring (see docs/07 §5). */
#ifndef FORM_MODE
#define FORM_MODE               2
#endif
#define FORM_M1                 1
#define FORM_M2                 2

/* Derived */
#define ROLE_IS_GW              (NODE_ROLE == NODE_ROLE_GATEWAY)
#define ROLE_IS_NODE            (NODE_ROLE == NODE_ROLE_NODE)
#define NODE_ID                 (ROLE_IS_GW ? 0 : 1)   /* CAN id low offset +N */
#define CONTRACT_VERSION        1u
#define FW_VERSION_BCD          0x100u                 /* 1.0.0 */

/*==========================================================================*/
/* 2. CAN - FDCAN1 on PD0(RX) / PD1(TX), AF9                                 */
/*==========================================================================*/
/* Why not FDCAN2 on PB12/PB13: those two pins are also the on-board W25Q128
 * CS#/CLK - it is either CAN or the on-board flash, not both.  Decision was to
 * keep the flash and move CAN.  See docs/07 §4 C1.
 * AF9 evidence: DS12288 Rev 6 Table 13, Port D (PD0/PD1 rows). */
#ifndef CAN_USE_FDCAN
#define CAN_USE_FDCAN           1           /* 1=FDCAN1(PD0/PD1)  2=FDCAN2 fallback */
#endif

#if (CAN_USE_FDCAN == 1)
  #define CAN_PERIPH            FDCAN1
  #define CAN_RX_GPIO_PORT      GPIOD
  #define CAN_RX_PIN            GPIO_PIN_0
  #define CAN_TX_GPIO_PORT      GPIOD
  #define CAN_TX_PIN            GPIO_PIN_1
  #define CAN_GPIO_CLK_ENABLE() __HAL_RCC_GPIOD_CLK_ENABLE()
  #define CAN_AF                GPIO_AF9_FDCAN1
  #define CAN_IRQn              FDCAN1_IT0_IRQn
  #define CAN_IRQ_HANDLER       FDCAN1_IT0_IRQHandler
#else
  #define CAN_PERIPH            FDCAN2
  #define CAN_RX_GPIO_PORT      GPIOB
  #define CAN_RX_PIN            GPIO_PIN_12
  #define CAN_TX_GPIO_PORT      GPIOB
  #define CAN_TX_PIN            GPIO_PIN_13
  #define CAN_GPIO_CLK_ENABLE() __HAL_RCC_GPIOB_CLK_ENABLE()
  #define CAN_AF                GPIO_AF9_FDCAN2
  #define CAN_IRQn              FDCAN2_IT0_IRQn
  #define CAN_IRQ_HANDLER       FDCAN2_IT0_IRQHandler
#endif
#define CAN_CLK_ENABLE()        __HAL_RCC_FDCAN_CLK_ENABLE()

/* Bit timing - taken as-is from the hardware-verified CAN v3.0:
 * PCLK1 = 150 MHz, Prescaler 30 -> tq = 200 ns, 10 tq/bit -> 2 us -> 500 kbit/s
 * sample point (1+7)/10 = 80 %, SJW 2 (<= min(Seg1,Seg2)+1 = 3) */
#define CAN_BITRATE             500000UL
#define CAN_NOMINAL_PRESCALER   30u
#define CAN_SEG1                7u
#define CAN_SEG2                2u
#define CAN_SJW                 2u

#define CAN_RX_Q_LEN            32u         /* FreeRTOS queue depth, can_msg_t slots
                                               (docs/02 §5 decision 1: 16 B x 32 = 512 B) */
#define CAN_HB_PERIOD_MS        1000u
#define CAN_LINK_TIMEOUT_MS     3000u       /* v3.0 used 1500; raised to fit ISO-TP */
#define CAN_BACKOFF_INIT_MS     1000u       /* shared backoff for bus-off and MQTT */
#define CAN_BACKOFF_MAX_MS      30000u

/*==========================================================================*/
/* 3. RS485 / Modbus - USART2 on PD5(TX) / PD6(RX), AF7                      */
/*==========================================================================*/
/* Why not USART3 on PB10/PB11: those pins are given to the ESP-12F (silk-
 * screened position).  Why not USART2's default PA2/PA3: those are user keys
 * K0/K1.  So PD5/PD6 - the only usable USART2 data pair on port D.
 * See docs/07 §4 C2/C3.  AF7 evidence: DS12288 Table 13, Port D. */
#define RS485_UART_PERIPH        USART2
#define RS485_UART_IRQn          USART2_IRQn
#define RS485_UART_IRQHandler    USART2_IRQHandler
#define RS485_UART_CLK_ENABLE()  __HAL_RCC_USART2_CLK_ENABLE()

#define RS485_TX_GPIO_PORT       GPIOD
#define RS485_TX_PIN             GPIO_PIN_5
#define RS485_RX_GPIO_PORT       GPIOD
#define RS485_RX_PIN             GPIO_PIN_6
#define RS485_GPIO_CLK_ENABLE()  __HAL_RCC_GPIOD_CLK_ENABLE()
#define RS485_GPIO_AF            GPIO_AF7_USART2

/* DMA, used only when RS485_RX_MODE == 1.  On G4 any DMA1 channel can serve
 * any request via DMAMUX, so the channel stays put and only the request line
 * changes together with the USART instance. */
#define RS485_DMA_INSTANCE       DMA1_Channel2
#define RS485_DMA_REQUEST        DMA_REQUEST_USART2_RX

/* RX path: 0 = RXNE byte-wise interrupt (the only mode verified on hardware)
 *          1 = DMA circular + IDLE                                         */
#ifndef RS485_RX_MODE
#define RS485_RX_MODE            0
#endif

/* Direction control: the RS485 transceiver is an external module and is
 * usually auto-direction.  Set to 1 if the module exposes DE/RE. */
#ifndef RS485_USE_DE_PIN
#define RS485_USE_DE_PIN         0
#endif
#if (RS485_USE_DE_PIN == 1)
#define RS485_DE_GPIO_PORT       GPIOC
#define RS485_DE_PIN             GPIO_PIN_0    /* TODO: put the real DE pin here */
#define RS485_DE_GPIO_CLK_ENABLE() __HAL_RCC_GPIOC_CLK_ENABLE()
#endif

#define RS485_DEFAULT_BAUDRATE   115200UL
#define RS485_DEFAULT_SLAVE_ID   1u
/* MB_TIMEOUT_MS / MB_RETRY_MAX / POLL_PERIOD_MS stay owned by the application
 * layer for now.  The app currently spells them MASTER_TIMEOUT_MS /
 * MASTER_RETRY_MAX / POLL_PERIOD_MS in App/mb_master_port.c; unifying the
 * names is a v0.4 task (docs/06 conflict #6). */

/*==========================================================================*/
/* 4. ESP-12F (WiFi / MQTT uplink) - USART3 on PB10(TX) / PB11(RX), AF7       */
/*==========================================================================*/
/* WARNING (risk R-8): PB10/PB11 are TT type - 3.3 V tolerant ONLY.  The
 * ESP-12F is 3.3 V logic so it is safe, but a 5 V module here would be
 * over-volted.  See docs/07 §4 C7. */
#define ESP_UART_PERIPH          USART3
#define ESP_UART_IRQn            USART3_IRQn
#define ESP_UART_IRQHandler      USART3_IRQHandler
#define ESP_UART_CLK_ENABLE()    __HAL_RCC_USART3_CLK_ENABLE()
#define ESP_TX_GPIO_PORT         GPIOB
#define ESP_TX_PIN               GPIO_PIN_10
#define ESP_RX_GPIO_PORT         GPIOB
#define ESP_RX_PIN               GPIO_PIN_11
#define ESP_GPIO_CLK_ENABLE()    __HAL_RCC_GPIOB_CLK_ENABLE()
#define ESP_GPIO_AF              GPIO_AF7_USART3
#define ESP_AT_BAUDRATE          115200UL

/*==========================================================================*/
/* 5. Console - USART1 on PA9(TX) / PA10(RX), AF7                            */
/*==========================================================================*/
/* There is NO on-board USB-UART bridge.  PA9/PA10 are broken out to J1 pin
 * 11/12 and also to J4 (2x4 debug header shared with SWD).  An external
 * USB-TTL adapter is required. */
#define CONSOLE_UART_PERIPH      USART1
#define CONSOLE_UART_IRQn        USART1_IRQn
#define CONSOLE_UART_IRQHandler  USART1_IRQHandler
#define CONSOLE_UART_CLK_ENABLE() __HAL_RCC_USART1_CLK_ENABLE()
#define CONSOLE_TX_GPIO_PORT     GPIOA
#define CONSOLE_TX_PIN           GPIO_PIN_9
#define CONSOLE_RX_GPIO_PORT     GPIOA
#define CONSOLE_RX_PIN           GPIO_PIN_10
#define CONSOLE_GPIO_CLK_ENABLE() __HAL_RCC_GPIOA_CLK_ENABLE()
#define CONSOLE_GPIO_AF          GPIO_AF7_USART1
#define CONSOLE_BAUDRATE         115200UL
#define CONSOLE_TX_BUF_LEN       128u       /* per-task pool replacing v3.0's 96 B static buf */
#define CONSOLE_TX_POOL_NUM      4u

/*==========================================================================*/
/* 6. On-board W25Q128 (SPI2) - 16 MB, SPI mode (IO2/IO3 tied high, not QSPI) */
/*==========================================================================*/
/* The single most important correction of the board audit: the original
 * Modbus project drove its flash on SPI1/PA4-PA7, but there is no flash chip
 * on those pins - the chip sits on SPI2.  That is why it stayed "untested on
 * hardware" forever.  See docs/07 §3 / §4 C1.
 * CS is a GPIO soft chip-select; SCK/MISO/MOSI use AF5. */
#define FLASH_SPI_PERIPH         SPI2
#define FLASH_SPI_AF             GPIO_AF5_SPI2
#define FLASH_SPI_CLK_ENABLE()   __HAL_RCC_SPI2_CLK_ENABLE()
#define FLASH_CS_PORT            GPIOB
#define FLASH_CS_PIN             GPIO_PIN_12
#define FLASH_SCK_PORT           GPIOB
#define FLASH_SCK_PIN            GPIO_PIN_13
#define FLASH_MISO_PORT          GPIOB
#define FLASH_MISO_PIN           GPIO_PIN_14
#define FLASH_MOSI_PORT          GPIOB
#define FLASH_MOSI_PIN           GPIO_PIN_15
#define FLASH_GPIO_CLK_ENABLE()  __HAL_RCC_GPIOB_CLK_ENABLE()
#define FLASH_EXPECTED_JEDEC     0xEF4018UL  /* W25Q128 = EF 40 18; verify at boot */

/*==========================================================================*/
/* 7. 24C02 (I2C1) - PA15(SCL) / PB9(SDA), AF4, external module              */
/*==========================================================================*/
/* WARNING: PA15 is JTDI out of reset.  The debugger MUST stay in SWD 2-pin
 * mode (PA13/PA14 only); enabling JTAG or trace kills both I2C1 and the LCD's
 * PB3.  See docs/07 §4 C6. */
#define EEPROM_I2C_PERIPH        I2C1
#define EEPROM_I2C_CLK_ENABLE()  __HAL_RCC_I2C1_CLK_ENABLE()
#define EEPROM_SCL_PORT          GPIOA
#define EEPROM_SCL_PIN           GPIO_PIN_15
#define EEPROM_SDA_PORT          GPIOB
#define EEPROM_SDA_PIN           GPIO_PIN_9
#define EEPROM_GPIO_CLK_ENABLE() do { __HAL_RCC_GPIOA_CLK_ENABLE(); \
                                      __HAL_RCC_GPIOB_CLK_ENABLE(); } while (0)
#define EEPROM_GPIO_AF           GPIO_AF4_I2C1
/* EEPROM_DEV_ADDR stays owned by BSP/bsp_eeprom.c */

/*==========================================================================*/
/* 8. LCD (bit-banged SPI TFT)                                               */
/*==========================================================================*/
/* Controller: 0=ST7789(1.69", 240x280, GRAM 240x320, Y offset 20)
 *             1=ILI9341(240x320)   2=ST7735(128x160)
 * PB3 is JTDO/TRACESWO out of reset - same warning as §7. */
#define LCD_CTRL_SELECT          0
#define LCD_CS_PORT              GPIOD
#define LCD_CS_PIN               GPIO_PIN_11
#define LCD_SCL_PORT             GPIOB
#define LCD_SCL_PIN              GPIO_PIN_3
#define LCD_SDA_PORT             GPIOB
#define LCD_SDA_PIN              GPIO_PIN_5
#define LCD_DC_PORT              GPIOD
#define LCD_DC_PIN               GPIO_PIN_12
#define LCD_BL_PORT              GPIOD
#define LCD_BL_PIN               GPIO_PIN_13
#define LCD_GPIO_CLK_ENABLE()    do { __HAL_RCC_GPIOB_CLK_ENABLE(); \
                                      __HAL_RCC_GPIOD_CLK_ENABLE(); } while (0)

/*==========================================================================*/
/* 9. Keys and LEDs                                                          */
/*==========================================================================*/
/* LED : the board carries ONE user LED (blue).  Measured on the PE0/PE1 group
 *       (vendor examples agree); the schematic's "PC0" is a stale net label.
 *       The second, white LED is a hard-wired power indicator.
 * KEY : three keys wired to J2 pins 42/40/37 (PA0/PA2/PA3). */
#define KEY_WKUP_PORT            GPIOA
#define KEY_WKUP_PIN             GPIO_PIN_0
#define KEY_K0_PORT              GPIOA
#define KEY_K0_PIN               GPIO_PIN_2
#define KEY_K1_PORT              GPIOA
#define KEY_K1_PIN               GPIO_PIN_3
#define KEY_GPIO_CLK_ENABLE()    __HAL_RCC_GPIOA_CLK_ENABLE()
/* KEY_DEBOUNCE_MS / KEY_LONG_MS stay owned by BSP/bsp_key.c (v0.1 keeps
 * module-local tunables where they already live; centralising them here is
 * a follow-up, not a restructure). */

#define LED_PORT                 GPIOE
#define LED1_PIN                 GPIO_PIN_0    /* blue */
#define LED2_PIN                 GPIO_PIN_1    /* green (may be unpopulated) */
#define LED_GPIO_CLK_ENABLE()    __HAL_RCC_GPIOE_CLK_ENABLE()
/* Status LEDs must be written via atomic GPIOx->BSRR.  v3.0 once chased a
 * phantom "LED freeze" caused by the TogglePin/ODR read-modify-write path
 * losing writes under sniff load. */

/*==========================================================================*/
/* 10. Uplink parameters (form factor M2)                                    */
/*==========================================================================*/
#define UPLINK_TELEMETRY_MS      1000u
#define UPLINK_STATUS_MS         5000u
#define UPLINK_COUNTERS_MS       10000u
#define UPLINK_SNAPSHOT_MS       30000u
#define MQTT_KEEPALIVE_S         30u
#define MQTT_QOS_TELEMETRY       0
#define MQTT_QOS_DEFAULT         1
/* v0.5 实测修正：docs/03 §5.1 估 meta≈640 B，实测 1405 B（55 条 dp 描述
 * 平均 ~24 B/条）。发送缓冲按 meta 定，1536 留头/边界余量。
 * 静止数组，禁止 malloc（docs/03 §5.1）。 */
#define MQTT_SEND_BUF            1536u
#define MQTT_RECV_BUF            256u
#define MQTT_TOPIC_PREFIX        "gac/line1/gwA"
#define MQTT_BROKER_HOST         "192.168.1.100"   /* TODO: PC LAN IPv4 */
#define MQTT_BROKER_PORT         1883u
#define MQTT_CLIENT_ID           "g474-line1-gwA"
#define BACKFILL_BURST           8u

/*==========================================================================*/
/* 11. Storage sizing - owned by the Storage module (see note)               */
/*==========================================================================*/
/* The log/flash sizing (LOG_AREA_SIZE, LOG_RECORD_SIZE, LOG_SECTOR_*,
 * LOG_MAX_RECORDS) stays owned by Storage/log.h, where it is already a
 * self-consistent derived chain.
 *
 * v0.2 landed both deferred changes:
 *   - LOG_AREA_SIZE 64 KB -> 256 KB (4x offline buffer) together with moving
 *     the blocking power-up scan into log_task (Storage/log_service.c);
 *   - LOG_CACHE_CAP 512 -> 128, reclaiming 12 KB of SRAM for the RTOS heap
 *     (docs/02 decision 6, docs/04 §6).  Backfill (v0.6) must use the
 *     per-seq direct flash read API, never LOG_Read(). */

/*==========================================================================*/
/* 12. Tasks and watchdog (effective once FreeRTOS lands in v0.2)             */
/*==========================================================================*/
#define IWDG_PRESCALER           IWDG_PRESCALER_32
/* 32 kHz/32 = 1 kHz timebase.  Kept at the CAN v3.0 value of 3 s: the longest
 * legitimate blocking section is a flash sector erase (worst case 400 ms), and
 * that margin was never tightened on hardware.  See docs/05 A-14. */
#define IWDG_RELOAD              3000u

/*==========================================================================*/
/* 13. NVIC priorities (FreeRTOS, 4 preemption bits, group 4)                */
/*==========================================================================*/
/* Authoritative table: docs/04 §5 (as revised by docs/07).  Rule: any ISR
 * that calls a *FromISR API must sit at a numerically GREATER or equal
 * priority than configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (= 5).
 * SysTick/PendSV/SVCall are owned by the kernel port and are not listed.
 *
 *   FDCAN1_IT0  (6,0)  was (1,0) on v3.0 hardware - ILLEGAL once FromISR is
 *                      called from the RX callback; cost of the fix is
 *                      re-running acceptance A-03/A-04 (docs/05).
 *   USART1/2/3  (7,0)  console / RS485 / ESP-AT
 *   TIM6_DAC    (8,0)  HAL time base, no FromISR calls needed
 *   EXTI0/2/3   (9,0)  reserved for interrupt-mode keys                          */
#define IRQ_PRIO_CAN             6u
#define IRQ_PRIO_CONSOLE         7u
#define IRQ_PRIO_RS485           7u
#define IRQ_PRIO_ESP             7u
#define IRQ_PRIO_TIMEBASE        8u
#define IRQ_PRIO_KEY             9u

#endif /* __APP_BOARD_CFG_H */
