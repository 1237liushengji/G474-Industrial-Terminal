/**
  ******************************************************************************
  * @file    bsp_uart.h
  * @brief   Register-level USART driver for RS485 link (USART3 on PB10/PB11)
  * @note    Self-contained init (clock + GPIO + NVIC), no CubeMX dependency.
  *          RX is interrupt driven; RX byte is delivered to a callback.
  *          TX has two paths:
  *            - blocking (BSP_UART_SendBytes/WaitTxComplete): init/bring-up
  *            - interrupt driven (BSP_UART_SendBytesAsync): the operating
  *              path since v0.2 - TXE feeds bytes, TC fires the done-callback
  *              so the RS485 layer can release the bus without busy-waiting
  *              (docs/02 §4.3).
  ******************************************************************************
  */
#ifndef __BSP_UART_H
#define __BSP_UART_H

#include <stdint.h>

/* Async TX staging buffer: one Modbus RTU ADU max (256 B, docs/03). */
#define BSP_UART_TX_BUF_LEN       256U

/* RX byte callback, invoked in USART ISR context. */
typedef void (*bsp_uart_rx_cb_t)(uint8_t byte);

/* RX frame callback (DMA mode): complete raw frame delivered from the
   main-loop service routine. */
typedef void (*bsp_uart_rx_frame_cb_t)(const uint8_t *frame, uint16_t len,
                                       uint32_t now_us);

/* TX-complete callback, invoked in USART ISR context (TC flag set, i.e. the
   last stop bit has left the shift register). */
typedef void (*bsp_uart_tx_done_cb_t)(void);

/**
  * @brief  Initialize USART (8N1, baud configurable).
  * @param  baudrate: e.g. 9600/19200/38400/57600/115200/230400
  * @retval 0 ok, -1 error
  */
int32_t BSP_UART_Init(uint32_t baudrate);

/**
  * @brief  Change baudrate on the fly.
  */
void BSP_UART_SetBaudrate(uint32_t baudrate);

/**
  * @brief  Register RX byte callback (only one, called in ISR).
  */
void BSP_UART_SetRxCallback(bsp_uart_rx_cb_t cb);

/**
  * @brief  Register RX frame callback used in DMA mode (main-loop ctx).
  */
void BSP_UART_SetRxFrameCallback(bsp_uart_rx_frame_cb_t cb);

/**
  * @brief  Call frequently from the main loop (DMA mode): moves bytes out
  *         of the DMA ring and assembles frames on the t3.5 boundary.
  *         No-op in interrupt mode.
  */
void BSP_UART_RxDmaService(void);

/**
  * @brief  Register TX-done callback (ISR context, fires on TC after the last
  *         byte of an async transmission has fully left the shift register).
  */
void BSP_UART_SetTxDoneCallback(bsp_uart_tx_done_cb_t cb);

/**
  * @brief  Interrupt-driven transmit of one frame.  Copies into the staging
  *         buffer, arms TXE/TC interrupts and returns immediately.
  * @retval 0 accepted; -1 busy (previous frame still in flight) - the caller
  *         must treat this as a real error (single-writer invariant broken),
  *         not retry silently.
  */
int32_t BSP_UART_SendBytesAsync(const uint8_t *data, uint16_t len);

/**
  * @brief  Blocking transmit of len bytes (waits TXE for each byte).
  * @note   Kept for bring-up/init paths; the RS485 frame path uses
  *         BSP_UART_SendBytesAsync since v0.2.
  */
void BSP_UART_SendBytes(const uint8_t *data, uint16_t len);

/**
  * @brief  Wait until last byte fully shifted out (TC flag).
  * @note   TXE != frame sent; RS485 direction switch MUST wait TC.
  */
void BSP_UART_WaitTxComplete(void);

/**
  * @brief  USART global ISR entry (vector symbol comes from app_board_cfg.h).
  */
void BSP_UART_IRQHandler(void);

#endif /* __BSP_UART_H */
