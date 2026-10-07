/**
  ******************************************************************************
  * @file    bsp_rs485.h
  * @brief   RS485 half-duplex transceiver management on top of BSP_UART
  *
  *  Two hardware modes are supported (see app_board_cfg.h):
  *   - RS485_USE_DE_PIN = 1 : classic DE/RE GPIO controlled direction
  *   - RS485_USE_DE_PIN = 0 : transceiver with automatic direction (default
  *                             on the Genbotter board); DE calls are no-ops
  ******************************************************************************
  */
#ifndef __BSP_RS485_H
#define __BSP_RS485_H

#include <stdint.h>

/* RX byte callback (ISR context), wired to BSP_UART callback. */
typedef void (*rs485_rx_cb_t)(uint8_t byte);

/* RX frame callback (DMA mode, main-loop context). */
typedef void (*rs485_rx_frame_cb_t)(const uint8_t *frame, uint16_t len,
                                    uint32_t now_us);

/* TX-done callback: invoked from the USART ISR when the last stop bit of a
   frame has left the shift register (TC).  The bus has already been released
   back to RX mode at this point.  RTOS callers use this to wake a task with
   vTaskNotifyGiveFromISR (docs/02 §4.3). */
typedef void (*rs485_tx_done_cb_t)(void);

/**
  * @brief  Initialize RS485 link (UART + optional DE GPIO). Default: RX mode.
  * @param  baudrate
  * @retval 0 ok
  */
int32_t RS485_Init(uint32_t baudrate);

/**
  * @brief  Switch transceiver to transmit mode (DE=1 when GPIO controlled).
  */
void RS485_SetTxMode(void);

/**
  * @brief  Switch transceiver back to receive mode (DE=0).
  */
void RS485_SetRxMode(void);

/**
  * @brief  Transmit one frame, non-blocking (v0.2).
  *
  *         Sequence: TX mode -> stage the frame -> the USART ISR feeds bytes
  *         on TXE -> on TC the bus returns to RX mode and the TX-done
  *         callback fires.  The caller is free immediately; the protocol
  *         engines are lock-step (one frame per request), so a second call
  *         while a frame is in flight is a bug and is dropped + counted
  *         instead of corrupting the wire (see RS485_GetTxDropCount).
  *
  * @param  data   frame bytes (address + PDU + CRC already appended by caller)
  * @param  len
  */
void RS485_SendFrame(const uint8_t *data, uint16_t len);

/**
  * @brief  Register TX-done callback (ISR context; bus already back in RX).
  */
void RS485_SetTxDoneCallback(rs485_tx_done_cb_t cb);

/**
  * @brief  Number of frames dropped because a previous frame was still in
  *         flight (must stay 0; non-zero means the single-writer invariant
  *         of the half-duplex bus was violated).
  */
uint32_t RS485_GetTxDropCount(void);

/**
  * @brief  Register RX byte callback (delivered in USART ISR).
  */
void RS485_SetRxCallback(rs485_rx_cb_t cb);

/**
  * @brief  Register RX frame callback (DMA mode; main-loop context).
  */
void RS485_SetRxFrameCallback(rs485_rx_frame_cb_t cb);

/**
  * @brief  RX statistics hook used by tests: returns bytes received so far.
  */
uint32_t RS485_GetRxCount(void);

#endif /* __BSP_RS485_H */
