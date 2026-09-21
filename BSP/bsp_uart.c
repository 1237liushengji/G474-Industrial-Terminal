/**
  ******************************************************************************
  * @file    bsp_uart.c
  * @brief   Register-level USART3 driver for RS485 link.
  *
  *  RX architecture selected by RS485_RX_MODE (bsp_board_cfg.h):
  *    0 - RXNE interrupt per byte (default, verified on hardware)
  *    1 - DMA circular + IDLE frame detection (V1.4, optional):
  *        UART --DMA--> s_dma_buf --(IDLE ISR)--> s_ring --(service)--> frame
  *
  *  DMA mode details:
  *   - DMA1 channel in circular mode continuously fills s_dma_buf
  *   - the USART IDLE interrupt fires when the line stays idle >= 1 char,
  *     i.e. at the end of every frame (Modbus inter-frame gap is >= 3.5
  *     chars, intra-frame gap <= 1.5 chars, so IDLE reliably marks a
  *     frame boundary in this protocol)
  *   - the ISR moves the newly arrived bytes into a software ring and
  *     timestamps the arrival
  *   - BSP_UART_RxDmaService() (main loop) waits t3.5 of silence and then
  *     emits the complete frame through the frame callback
  *
  *  TX is blocking with explicit TC wait (RS485 direction switching).
  ******************************************************************************
  */
#include "bsp_uart.h"
#include "bsp_board_cfg.h"
#include "bsp_tick.h"
#include "ring_buffer.h"

/*====================================================================*/
/* Local objects                                                       */
/*====================================================================*/
static bsp_uart_rx_cb_t s_rx_cb = 0;
static bsp_uart_rx_frame_cb_t s_frame_cb = 0;

#if (RS485_RX_MODE == 1)
#include "stm32g4xx_hal_dma.h"

#define RX_DMA_BUF_SIZE   512U   /* max RTU frame 256 B, margin for wrap  */
#define RX_RING_SIZE      512U
#define RX_MAX_FRAME_LEN  256U

static uint8_t  s_dma_buf[RX_DMA_BUF_SIZE];
static uint8_t  s_ring_storage[RX_RING_SIZE];
static ring_buffer_t s_ring;
static DMA_HandleTypeDef s_hdma;

static volatile uint16_t s_dma_last_pos = 0U;   /* consumed DMA position */
static volatile uint32_t s_last_byte_us = 0U;
static uint32_t s_t35_us = 4000U;   /* set from baudrate (DMA mode) */
#endif

/*====================================================================*/
/* Static helpers                                                      */
/*====================================================================*/
static void UART_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    RS485_GPIO_CLK_ENABLE();
    RS485_UART_CLK_ENABLE();

    /* TX: PB10 -> USART3_TX (AF7), push-pull, high speed */
    gpio.Pin       = RS485_TX_PIN;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = RS485_GPIO_AF;
    HAL_GPIO_Init(RS485_TX_GPIO_PORT, &gpio);

    /* RX: PB11 <- USART3_RX (AF7); no pull, transceiver drives the line */
    gpio.Pin   = RS485_RX_PIN;
    gpio.Pull  = GPIO_NOPULL;
    HAL_GPIO_Init(RS485_RX_GPIO_PORT, &gpio);
}

static uint32_t UART_ComputeBRR(uint32_t baudrate)
{
    uint32_t pclk;

    /* USART3 kernel clock = PCLK1; runtime queried so it stays valid
       if the CubeMX clock tree changes. BRR = fck / baud (16x over-sample,
       OVER8=0), rounded. */
    pclk = HAL_RCC_GetPCLK1Freq();
    return (pclk + (baudrate / 2U)) / baudrate;
}

/*====================================================================*/
/* Public API                                                          */
/*====================================================================*/
int32_t BSP_UART_Init(uint32_t baudrate)
{
    UART_GPIO_Init();

    /* Disable USART first while configuring */
    RS485_UART_PERIPH->CR1 = 0U;

    /* Oversampling 16 (OVER8=0), 8 data bits, no parity (default after reset) */
    RS485_UART_PERIPH->BRR = UART_ComputeBRR(baudrate);

    /* UE=1, TE=1, RE=1; 1 stop bit is default */
    RS485_UART_PERIPH->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;

#if (RS485_RX_MODE == 1)
    /* ---- DMA circular RX + IDLE frame detection ---- */
    s_t35_us = (baudrate == 0U) ? 10000U : (uint32_t)(38500000UL / baudrate);
    RING_Init(&s_ring, s_ring_storage, sizeof(s_ring_storage));

    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_DMAMUX1_CLK_ENABLE();

    s_hdma.Instance = DMA1_Channel2;
    s_hdma.Init.Request = DMA_REQUEST_USART3_RX;   /* DMAMUX req 28 */
    s_hdma.Init.Direction = DMA_PERIPH_TO_MEMORY;
    s_hdma.Init.PeriphInc = DMA_PINC_DISABLE;
    s_hdma.Init.MemInc = DMA_MINC_ENABLE;
    s_hdma.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    s_hdma.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    s_hdma.Init.Mode = DMA_CIRCULAR;
    s_hdma.Init.Priority = DMA_PRIORITY_HIGH;
    if (HAL_DMA_Init(&s_hdma) == HAL_OK)
    {
        HAL_DMA_Start(&s_hdma, (uint32_t)&RS485_UART_PERIPH->RDR,
                      (uint32_t)s_dma_buf, RX_DMA_BUF_SIZE);
        /* ask the USART to drive the DMA receiver */
        RS485_UART_PERIPH->CR3 |= USART_CR3_DMAR;
        /* IDLE line interrupt */
        RS485_UART_PERIPH->CR1 |= USART_CR1_IDLEIE;
    }
#else
    /* ---- RXNE interrupt mode ---- */
    RS485_UART_PERIPH->CR1 |= USART_CR1_RXNEIE;
#endif

    /* NVIC: medium priority, enable USART3 IRQ */
    HAL_NVIC_SetPriority(RS485_UART_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(RS485_UART_IRQn);

    return 0;
}

void BSP_UART_SetBaudrate(uint32_t baudrate)
{
    /* Recompute BRR; UART is kept enabled, byte in flight may be corrupted,
       caller must change baud only in idle state. */
    RS485_UART_PERIPH->BRR = UART_ComputeBRR(baudrate);
#if (RS485_RX_MODE == 1)
    s_t35_us = (baudrate == 0U) ? 10000U : (uint32_t)(38500000UL / baudrate);
#endif
}

void BSP_UART_SetRxCallback(bsp_uart_rx_cb_t cb)
{
    s_rx_cb = cb;
}

void BSP_UART_SetRxFrameCallback(bsp_uart_rx_frame_cb_t cb)
{
#if (RS485_RX_MODE == 1)
    s_frame_cb = cb;
#else
    (void)cb;
#endif
}

void BSP_UART_SendBytes(const uint8_t *data, uint16_t len)
{
    /* Clear TC so the TC-wait at frame end refers to THIS frame. */
    RS485_UART_PERIPH->ICR = USART_ICR_TCCF;

    while (len > 0U)
    {
        /* Wait TXE (transmit data register empty) */
        while ((RS485_UART_PERIPH->ISR & USART_ISR_TXE_TXFNF) == 0U)
        {
        }
        RS485_UART_PERIPH->TDR = *data;
        data++;
        len--;
    }
}

void BSP_UART_WaitTxComplete(void)
{
    /* TC = last byte fully shifted out including stop bit.
       TXE alone is NOT enough before releasing the RS485 bus. */
    while ((RS485_UART_PERIPH->ISR & USART_ISR_TC) == 0U)
    {
    }
    RS485_UART_PERIPH->ICR = USART_ICR_TCCF;
}

/*====================================================================*/
/* DMA mode: ISR moves bytes DMA->ring; main loop assembles frames     */
/*====================================================================*/
#if (RS485_RX_MODE == 1)
static void DMA_RingPull(void)
{
    uint16_t ndtr = (uint16_t)s_hdma.Instance->CNDTR;
    uint16_t cur = (uint16_t)(RX_DMA_BUF_SIZE - ndtr);  /* DMA write pos */
    uint16_t produced;
    uint16_t i;

    /* Wrap-safe byte count: DMA circular pointer may have wrapped around
       the end of s_dma_buf since the last pull. */
    if (cur >= s_dma_last_pos)
    {
        produced = (uint16_t)(cur - s_dma_last_pos);
    }
    else
    {
        produced = (uint16_t)(cur + RX_DMA_BUF_SIZE - s_dma_last_pos);
    }

    if (produced == 0U)
    {
        return;
    }
    /* Defensive: a frame can never exceed the ring, but guard anyway. */
    if (produced > RX_RING_SIZE)
    {
        produced = RX_RING_SIZE;
    }
    for (i = 0U; i < produced; i++)
    {
        uint16_t idx = (uint16_t)((s_dma_last_pos + i) % RX_DMA_BUF_SIZE);
        if (RING_Push(&s_ring, s_dma_buf[idx]) == 0U)
        {
            break;   /* ring full: drop overflow (should not happen) */
        }
    }
    s_dma_last_pos = (uint16_t)((s_dma_last_pos + produced) % RX_DMA_BUF_SIZE);
    s_last_byte_us = BSP_Tick_GetUs();
}
#endif

void BSP_UART_RxDmaService(void)
{
#if (RS485_RX_MODE == 1)
    uint32_t now_us = BSP_Tick_GetUs();

    if (!RING_Empty(&s_ring) &&
        ((uint32_t)(now_us - s_last_byte_us) >= s_t35_us))
    {
        /* idle long enough: the ring holds exactly ONE complete frame
           (the IDLE interrupt fired once per received frame); pop it all */
        uint8_t frame[RX_MAX_FRAME_LEN];
        uint16_t len = 0U;
        uint8_t b;

        while ((len < sizeof(frame)) && RING_Pop(&s_ring, &b))
        {
            frame[len++] = b;
        }
        if ((len > 0U) && (s_frame_cb != 0))
        {
            s_frame_cb(frame, len, now_us);
        }
    }
#endif
}

/*====================================================================*/
/* USART ISR                                                           */
/*====================================================================*/
void BSP_UART_IRQHandler(void)
{
    uint32_t isr = RS485_UART_PERIPH->ISR;

#if (RS485_RX_MODE == 1)
    /* IDLE line: one frame arrived completely -> pull into ring */
    if ((isr & USART_ISR_IDLE) != 0U)
    {
        RS485_UART_PERIPH->ICR = USART_ICR_IDLECF;
        DMA_RingPull();
    }
    /* ORE possible when DMA is slow to drain - discard and continue */
    if ((isr & USART_ISR_ORE) != 0U)
    {
        (void)RS485_UART_PERIPH->RDR;
        RS485_UART_PERIPH->ICR = USART_ICR_ORECF;
    }
#else
    /* Overrun error: read RDR to discard, keep receiver alive */
    if ((isr & USART_ISR_ORE) != 0U)
    {
        (void)RS485_UART_PERIPH->RDR;      /* clears ORE */
        RS485_UART_PERIPH->ICR = USART_ICR_ORECF;
    }

    if ((isr & USART_ISR_RXNE) != 0U)
    {
        uint8_t byte = (uint8_t)(RS485_UART_PERIPH->RDR & 0xFFU); /* clears RXNE */
        if (s_rx_cb != 0)
        {
            s_rx_cb(byte);
        }
    }
#endif
}

/*====================================================================*/
/* ISR symbol wired into the startup vector table                      */
/*====================================================================*/
void RS485_UART_IRQHandler(void)
{
    BSP_UART_IRQHandler();
}
