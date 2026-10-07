/**
  ******************************************************************************
  * @file    bsp_console.h
  * @brief   Register-level USART console (USART1 on PA9/PA10, AF7).
  *
  *  There is NO on-board USB-UART bridge - an external USB-TTL adapter on
  *  J1 pin 11/12 (or J4) is required.  Pins come from app_board_cfg.h §5.
  *
  *  Role in the system (docs/02 decision 4):
  *   - text console for the debug commands (Common/console.c)
  *   - M1 form factor will multiplex binary uplink frames over the same wire
  *     (0xAA55 magic framing, v0.5) - the console API never emits raw 0xAA
  *     as a first byte (all output is printable ASCII starting 0x20..0x7E).
  *
  *  TX is blocking (poll TXE): console traffic is low-rate and this path must
  *  also work from the fault handlers before any scheduler exists.
  *  RX is an interrupt-driven byte ring; the consumer polls via
  *  BSP_Console_GetChar() from its own task.
  ******************************************************************************
  */
#ifndef __BSP_CONSOLE_H
#define __BSP_CONSOLE_H

#include <stdint.h>

/** RX software ring depth (bytes). */
#define BSP_CONSOLE_RX_RING       128U

/** Initialize USART1 (8N1), enable RX interrupt, print nothing. */
void BSP_Console_Init(void);

/** 1 once BSP_Console_Init() has completed (fault handlers use this). */
uint8_t BSP_Console_IsReady(void);

/** Blocking transmit of len bytes (polls TXE; no interrupts required). */
void BSP_Console_Send(const uint8_t *data, uint16_t len);

/** Convenience for string literals / NUL-terminated buffers. */
void BSP_Console_Puts(const char *s);

/** Bytes waiting in the RX ring. */
uint16_t BSP_Console_Available(void);

/**
  * @brief  Pop one received byte.
  * @retval 1 got a byte, 0 ring empty
  */
int32_t BSP_Console_GetChar(uint8_t *c);

/** USART1 ISR body (vector symbol wired via app_board_cfg.h). */
void BSP_Console_IRQHandler(void);

#endif /* __BSP_CONSOLE_H */
