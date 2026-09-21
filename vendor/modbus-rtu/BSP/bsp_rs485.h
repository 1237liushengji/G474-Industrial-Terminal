/**
  ******************************************************************************
  * @file    bsp_rs485.h
  * @brief   RS485 half-duplex transceiver management on top of BSP_UART
  *
  *  Two hardware modes are supported (see bsp_board_cfg.h):
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
  * @brief  Transmit one frame: TX mode -> send bytes -> wait TC -> RX mode.
  * @note   TXE alone does NOT mean the frame is on the wire; RS485 direction
  *         change MUST wait for TC (transmission complete). This is the
  *         standard interview point implemented here.
  * @param  data   frame bytes (address + PDU + CRC already appended by caller)
  * @param  len
  */
void RS485_SendFrame(const uint8_t *data, uint16_t len);

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
