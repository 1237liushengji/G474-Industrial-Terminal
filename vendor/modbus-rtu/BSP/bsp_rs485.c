/**
  ******************************************************************************
  * @file    bsp_rs485.c
  * @brief   RS485 half-duplex link management.
  *
  *  Transmit sequence (spec section 7):
  *      RX mode -> DE=1 -> UART send -> wait TC -> DE=0 -> RX mode
  ******************************************************************************
  */
#include "bsp_rs485.h"
#include "bsp_uart.h"
#include "bsp_board_cfg.h"

/*====================================================================*/
/* Local                                                                */
/*====================================================================*/
static rs485_rx_cb_t s_rx_cb = 0;
static rs485_rx_frame_cb_t s_rx_frame_cb = 0;
static volatile uint32_t s_rx_count = 0;
static volatile uint8_t  s_tx_active = 0U;   /* suppress RX while TX on */

static void RxByteBridge(uint8_t byte)
{
    /* Half-duplex: bytes received while we are transmitting are our own
       echo (auto-direction transceivers may keep RO enabled while DE is
       driven). Drop them so the protocol layer never sees a self frame. */
    if (s_tx_active != 0U)
    {
        return;
    }
    s_rx_count++;
    if (s_rx_cb != 0)
    {
        s_rx_cb(byte);
    }
}

#if (RS485_RX_MODE == 1)
static void RxFrameBridge(const uint8_t *frame, uint16_t len, uint32_t now_us)
{
    if (s_tx_active != 0U)
    {
        return;
    }
    s_rx_count += len;
    if (s_rx_frame_cb != 0)
    {
        s_rx_frame_cb(frame, len, now_us);
    }
}
#endif

/*====================================================================*/
/* Public API                                                          */
/*====================================================================*/
int32_t RS485_Init(uint32_t baudrate)
{
#if (RS485_USE_DE_PIN == 1)
    GPIO_InitTypeDef gpio = {0};

    RS485_DE_GPIO_CLK_ENABLE();
    gpio.Pin   = RS485_DE_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(RS485_DE_GPIO_PORT, &gpio);
    /* start in receive mode (DE low -> RO enabled) */
    HAL_GPIO_WritePin(RS485_DE_GPIO_PORT, RS485_DE_PIN, GPIO_PIN_RESET);
#endif

    BSP_UART_Init(baudrate);
    BSP_UART_SetRxCallback(RxByteBridge);
#if (RS485_RX_MODE == 1)
    BSP_UART_SetRxFrameCallback(RxFrameBridge);
#endif
    return 0;
}

void RS485_SetTxMode(void)
{
#if (RS485_USE_DE_PIN == 1)
    HAL_GPIO_WritePin(RS485_DE_GPIO_PORT, RS485_DE_PIN, GPIO_PIN_SET);
    /* give the transceiver a few us to enable the driver */
    for (volatile uint32_t i = 0; i < 200U; i++)
    {
    }
#endif
}

void RS485_SetRxMode(void)
{
#if (RS485_USE_DE_PIN == 1)
    HAL_GPIO_WritePin(RS485_DE_GPIO_PORT, RS485_DE_PIN, GPIO_PIN_RESET);
#endif
}

void RS485_SendFrame(const uint8_t *data, uint16_t len)
{
    s_tx_active = 1U;   /* start suppressing any RX echo */
    RS485_SetTxMode();
    BSP_UART_SendBytes(data, len);
    /* Critical: wait until the LAST byte (incl. stop bit) is fully shifted
       out before releasing the bus; TXE would be too early and the frame
       tail would be cut off. */
    BSP_UART_WaitTxComplete();
    RS485_SetRxMode();
    s_tx_active = 0U;   /* bus quiet again, resume listening */
}

void RS485_SetRxCallback(rs485_rx_cb_t cb)
{
    s_rx_cb = cb;
}

#if (RS485_RX_MODE == 1)
void RS485_SetRxFrameCallback(rs485_rx_frame_cb_t cb)
{
    s_rx_frame_cb = cb;
}
#endif

uint32_t RS485_GetRxCount(void)
{
    return s_rx_count;
}
