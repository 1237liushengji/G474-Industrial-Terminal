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
/* 0. Migration guard - DO NOT FLASH until v0.1 is finished                  */
/*==========================================================================*/
/* Remaining v0.1 work: bsp_w25q128.c -> SPI2; hal_conf union; msp.c/it.c ->
 * FDCAN1 + USART2; main.c merge FDCAN/IWDG init; new Common/bsp_printf.c;
 * merge BSP/bsp_led; build 4 Keil targets.  Delete both lines below when done.*/
#define V01_MIGRATION_INCOMPLETE   1
#if (V01_MIGRATION_INCOMPLETE == 1)
#warning "v0.1 migration incomplete: pin macros are already final but some drivers still point at the old peripherals - DO NOT FLASH."
#endif

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

#define CAN_RX_RING_LEN         16u         /* v0.1 keeps SPSC ring; v0.2 -> RTOS queue */
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
#define MB_TIMEOUT_MS            100u       /* as in the original master_main.c */
#define MB_RETRY_MAX             2u         /* 3 attempts total */
#define POLL_PERIOD_MS           500u

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
#define EEPROM_DEV_ADDR          0xA0u

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
#define KEY_DEBOUNCE_MS          20u
#define KEY_LONG_MS              1000u

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
#define MQTT_SEND_BUF            896u
#define MQTT_RECV_BUF            256u
#define MQTT_TOPIC_PREFIX        "gac/line1/gwA"
#define MQTT_BROKER_HOST         "192.168.1.100"   /* TODO: PC LAN IPv4 */
#define MQTT_BROKER_PORT         1883u
#define MQTT_CLIENT_ID           "g474-line1-gwA"
#define BACKFILL_BURST           8u

/*==========================================================================*/
/* 11. Storage quotas                                                        */
/*==========================================================================*/
/* On-board 16 MB confirmed -> log area raised 64 KB to 256 KB:
 * 32 B records -> 8192 entries, about 105 min of offline buffering
 * (about 52 min with the 50/50 event/pending split). */
#define LOG_AREA_SIZE            (256UL * 1024UL)
#define LOG_QUOTA_EVENT          2048u
#define LOG_QUOTA_PENDING        2048u
#define LOG_CACHE_CAP            128u       /* down from 512; reclaims 12 KB RAM */

/*==========================================================================*/
/* 12. Tasks and watchdog (effective once FreeRTOS lands in v0.2)             */
/*==========================================================================*/
#define IWDG_PRESCALER           IWDG_PRESCALER_32
#define IWDG_RELOAD              2000u       /* 32 kHz/32 = 1 kHz -> 2000 = 2 s */

#endif /* __APP_BOARD_CFG_H */
