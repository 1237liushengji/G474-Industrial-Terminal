/**
  ******************************************************************************
  * @file    bsp_board_cfg.h
  * @brief   Board-level hardware configuration (single point of change)
  * @note    Genbotter STM32G474VET6 board. All pin changes happen here only.
  *          See docs/02-板级硬件配置.md for silkscreen details & rationale.
  ******************************************************************************
  */
#ifndef __BSP_BOARD_CFG_H
#define __BSP_BOARD_CFG_H

#include "main.h"   /* HAL: USART3, GPIOB ... */

/*====================================================================*/
/* Node role: build Master or Slave firmware                           */
/*   Keil: Options -> C/C++ -> Define add MODBUS_NODE_ROLE=0 / =1      */
/*   or edit here. Default SLAVE.                                      */
/*   (duplicated in Common/types.h with #ifndef guards, keep in sync)  */
/*====================================================================*/
#ifndef NODE_ROLE_MASTER
#define NODE_ROLE_MASTER   0
#endif
#ifndef NODE_ROLE_SLAVE
#define NODE_ROLE_SLAVE    1
#endif

#ifndef MODBUS_NODE_ROLE
#define MODBUS_NODE_ROLE        NODE_ROLE_SLAVE
#endif

/*====================================================================*/
/* RS485 link - USART3 on PB10(TX) / PB11(RX)                          */
/* STM32G474 AF table: PB10=USART3_TX(AF7), PB11=USART3_RX(AF7)        */
/*====================================================================*/
#define RS485_UART_PERIPH        USART3
#define RS485_UART_IRQn          USART3_IRQn
#define RS485_UART_IRQHandler    USART3_IRQHandler
#define RS485_UART_CLK_ENABLE()  __HAL_RCC_USART3_CLK_ENABLE()

/* RX path selection:
 *   0 = RXNE interrupt per byte (default; verified on hardware)
 *   1 = DMA circular + IDLE line (V1.4 option, enable by setting 1)     */
#ifndef RS485_RX_MODE
#define RS485_RX_MODE            0
#endif

#define RS485_TX_GPIO_PORT       GPIOB
#define RS485_TX_PIN             GPIO_PIN_10
#define RS485_RX_GPIO_PORT       GPIOB
#define RS485_RX_PIN             GPIO_PIN_11
#define RS485_GPIO_CLK_ENABLE()  __HAL_RCC_GPIOB_CLK_ENABLE()
#define RS485_GPIO_AF            GPIO_AF7_USART3

/*====================================================================*/
/* RS485 direction control (DE/RE)                                     */
/*   The Genbotter board provides no DE/RE pin (auto direction), so    */
/*   RS485_USE_DE_PIN = 0 by default. If your transceiver exposes DE,  */
/*   set to 1 and fill the port/pin below.                             */
/*====================================================================*/
#ifndef RS485_USE_DE_PIN
#define RS485_USE_DE_PIN         0
#endif

#if (RS485_USE_DE_PIN == 1)
#define RS485_DE_GPIO_PORT       GPIOA
#define RS485_DE_PIN             GPIO_PIN_8    /* TODO: change to real DE pin */
#define RS485_DE_GPIO_CLK_ENABLE() __HAL_RCC_GPIOA_CLK_ENABLE()
#endif

/*====================================================================*/
/* Link parameters (defaults; can be changed at runtime later)         */
/*====================================================================*/
#define RS485_DEFAULT_BAUDRATE   115200UL
#define RS485_DEFAULT_SLAVE_ID   1

/*====================================================================*/
/* LCD (SPI TFT, soft SPI - see BSP/bsp_lcd.c)                         */
/* Controller: 0=ST7789(240x240) 1=ILI9341(240x320) 2=ST7735(128x160)  */
/*====================================================================*/
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

/*====================================================================*/
/* W25Q128 SPI flash (SPI1: CS PA4, SCK PA5, MISO PA6, MOSI PA7)      */
/*====================================================================*/
#define FLASH_CS_PORT            GPIOA
#define FLASH_CS_PIN             GPIO_PIN_4
#define FLASH_SCK_PORT           GPIOA
#define FLASH_SCK_PIN            GPIO_PIN_5
#define FLASH_MISO_PORT          GPIOA
#define FLASH_MISO_PIN           GPIO_PIN_6
#define FLASH_MOSI_PORT          GPIOA
#define FLASH_MOSI_PIN           GPIO_PIN_7

/*====================================================================*/
/* Keys (WKUP=PA0 confirm, K0=PA2 next, K1=PA3 modify)                 */
/*====================================================================*/
#define KEY_WKUP_PORT            GPIOA
#define KEY_WKUP_PIN             GPIO_PIN_0
#define KEY_K0_PORT              GPIOA
#define KEY_K0_PIN               GPIO_PIN_2
#define KEY_K1_PORT              GPIOA
#define KEY_K1_PIN               GPIO_PIN_3
#define KEY_GPIO_CLK_ENABLE()    __HAL_RCC_GPIOA_CLK_ENABLE()

#endif /* __BSP_BOARD_CFG_H */
