/**
  ******************************************************************************
  * @file    bsp_console.c
  * @brief   Register-level USART console driver (USART1).
  *
  *  Kept independent of HAL UART on purpose (HAL_UART_MODULE_ENABLED stays
  *  off): the project convention is register-level BSP drivers, and the TX
  *  path must work from fault handlers with nothing but the UART clocked.
  ******************************************************************************
  */
#include "bsp_console.h"
#include "app_board_cfg.h"
#include "ring_buffer.h"

/*====================================================================*/
/* Local state                                                         */
/*====================================================================*/
static uint8_t      s_rx_storage[BSP_CONSOLE_RX_RING];
static ring_buffer_t s_rx_ring;
static volatile uint8_t s_ready = 0U;

/*====================================================================*/
/* Init                                                                */
/*====================================================================*/
void BSP_Console_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    RING_Init(&s_rx_ring, s_rx_storage, BSP_CONSOLE_RX_RING);

    CONSOLE_GPIO_CLK_ENABLE();
    CONSOLE_UART_CLK_ENABLE();

    /* TX: PA9 -> USART1_TX (AF7); RX: PA10 <- USART1_RX (AF7) */
    gpio.Pin       = CONSOLE_TX_PIN;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = CONSOLE_GPIO_AF;
    HAL_GPIO_Init(CONSOLE_TX_GPIO_PORT, &gpio);

    gpio.Pin  = CONSOLE_RX_PIN;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(CONSOLE_RX_GPIO_PORT, &gpio);

    /* USART1 is on APB2 - BRR from PCLK2, 16x oversampling, 8N1 */
    CONSOLE_UART_PERIPH->CR1 = 0U;
    CONSOLE_UART_PERIPH->BRR =
        (HAL_RCC_GetPCLK2Freq() + (CONSOLE_BAUDRATE / 2U)) / CONSOLE_BAUDRATE;
    CONSOLE_UART_PERIPH->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE
                             | USART_CR1_RXNEIE;

    HAL_NVIC_SetPriority(CONSOLE_UART_IRQn, IRQ_PRIO_CONSOLE, 0U);
    HAL_NVIC_EnableIRQ(CONSOLE_UART_IRQn);

    s_ready = 1U;
}

uint8_t BSP_Console_IsReady(void)
{
    return s_ready;
}

/*====================================================================*/
/* TX (blocking, poll TXE)                                             */
/*====================================================================*/
void BSP_Console_Send(const uint8_t *data, uint16_t len)
{
    uint16_t i;

    if ((data == 0) || (len == 0U))
    {
        return;
    }

    for (i = 0U; i < len; i++)
    {
        while ((CONSOLE_UART_PERIPH->ISR & USART_ISR_TXE_TXFNF) == 0U)
        {
        }
        CONSOLE_UART_PERIPH->TDR = data[i];
    }
    while ((CONSOLE_UART_PERIPH->ISR & USART_ISR_TC) == 0U)
    {
    }
    CONSOLE_UART_PERIPH->ICR = USART_ICR_TCCF;
}

void BSP_Console_Puts(const char *s)
{
    uint16_t n = 0U;

    if (s == 0)
    {
        return;
    }
    while (s[n] != '\0')
    {
        n++;
    }
    BSP_Console_Send((const uint8_t *)s, n);
}

/*====================================================================*/
/* RX                                                                  */
/*====================================================================*/
uint16_t BSP_Console_Available(void)
{
    return RING_Count(&s_rx_ring);
}

int32_t BSP_Console_GetChar(uint8_t *c)
{
    if (c == 0)
    {
        return 0;
    }
    return (RING_Pop(&s_rx_ring, c) != 0U) ? 1 : 0;
}

/*====================================================================*/
/* ISR                                                                 */
/*====================================================================*/
void BSP_Console_IRQHandler(void)
{
    uint32_t isr = CONSOLE_UART_PERIPH->ISR;

    /* Overrun: read RDR to release it, keep the receiver alive */
    if ((isr & USART_ISR_ORE) != 0U)
    {
        (void)CONSOLE_UART_PERIPH->RDR;
        CONSOLE_UART_PERIPH->ICR = USART_ICR_ORECF;
    }

    if ((isr & USART_ISR_RXNE) != 0U)
    {
        uint8_t byte = (uint8_t)(CONSOLE_UART_PERIPH->RDR & 0xFFU);
        (void)RING_Push(&s_rx_ring, byte);   /* ring full: byte lost, keep going */
    }
}

/*====================================================================*/
/* ISR symbol wired into the startup vector table                      */
/*====================================================================*/
void CONSOLE_UART_IRQHandler(void)
{
    BSP_Console_IRQHandler();
}
