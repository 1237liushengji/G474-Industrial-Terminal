/**
  ******************************************************************************
  * @file    esp_at.c
  * @brief   ESP-12F AT link driver + connection state machine (M2, v0.6).
  *
  *  Register-level USART3 driver (same pattern as BSP/bsp_console.c) + a
  *  bounded-blocking AT command runner + the connection state machine.
  *  Single-owner: every function here is called from uplink_task only.
  ******************************************************************************
  */
#include "esp_at.h"
#include "app_board_cfg.h"
#include "at_window.h"
#include "ring_buffer.h"
#include "log_service.h"
#include "types.h"
#include "app_rtos.h"      /* App_Hb_Beat during long AT waits */

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <string.h>

/*====================================================================*/
/* USART3 driver                                                       */
/*====================================================================*/
#define ESP_RX_RING_SIZE   256U

static uint8_t       s_rx_storage[ESP_RX_RING_SIZE];
static ring_buffer_t s_rx_ring;

void Esp_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    RING_Init(&s_rx_ring, s_rx_storage, ESP_RX_RING_SIZE);

    ESP_GPIO_CLK_ENABLE();
    ESP_UART_CLK_ENABLE();

    gpio.Pin       = ESP_TX_PIN;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = ESP_GPIO_AF;
    HAL_GPIO_Init(ESP_TX_GPIO_PORT, &gpio);

    gpio.Pin  = ESP_RX_PIN;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(ESP_RX_GPIO_PORT, &gpio);

    /* USART3 is on APB1 */
    ESP_UART_PERIPH->CR1 = 0U;
    ESP_UART_PERIPH->BRR =
        (HAL_RCC_GetPCLK1Freq() + (ESP_AT_BAUDRATE / 2U)) / ESP_AT_BAUDRATE;
    ESP_UART_PERIPH->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE
                         | USART_CR1_RXNEIE;

    HAL_NVIC_SetPriority(ESP_UART_IRQn, IRQ_PRIO_ESP, 0U);
    HAL_NVIC_EnableIRQ(ESP_UART_IRQn);
}

void Esp_WriteBytes(const uint8_t *data, uint16_t len)
{
    uint16_t i;

    if (data == 0)
    {
        return;
    }
    for (i = 0U; i < len; i++)
    {
        while ((ESP_UART_PERIPH->ISR & USART_ISR_TXE_TXFNF) == 0U)
        {
        }
        ESP_UART_PERIPH->TDR = data[i];
    }
}

uint16_t Esp_Read(uint8_t *dst, uint16_t max)
{
    uint16_t n = 0U;

    if (dst == 0)
    {
        return 0U;
    }
    while ((n < max) && (RING_Pop(&s_rx_ring, &dst[n]) != 0U))
    {
        n++;
    }
    return n;
}

uint16_t Esp_RxAvailable(void)
{
    return RING_Count(&s_rx_ring);
}

void Esp_FlushRx(void)
{
    RING_Reset(&s_rx_ring);
}

/** USART3 ISR body + vector symbol (convention of the other BSP drivers). */
void Esp_IrqHandler(void)
{
    uint32_t isr = ESP_UART_PERIPH->ISR;

    if ((isr & USART_ISR_ORE) != 0U)
    {
        (void)ESP_UART_PERIPH->RDR;
        ESP_UART_PERIPH->ICR = USART_ICR_ORECF;
    }
    if ((isr & USART_ISR_RXNE) != 0U)
    {
        uint8_t b = (uint8_t)(ESP_UART_PERIPH->RDR & 0xFFU);
        (void)RING_Push(&s_rx_ring, b);
    }
}

void ESP_UART_IRQHandler(void)
{
    Esp_IrqHandler();
}

/*====================================================================*/
/* State machine                                                       */
/*====================================================================*/
typedef enum
{
    ESP_ST_DOWN = 0,
    ESP_ST_RESET,
    ESP_ST_AT,
    ESP_ST_CWMODE,
    ESP_ST_JOIN,
    ESP_ST_CIPMUX,
    ESP_ST_CIPMODE,
    ESP_ST_OPEN,
    ESP_ST_CIPSEND,
    ESP_ST_PASS
} esp_state_t;

static esp_state_t s_state = ESP_ST_DOWN;
static uint32_t    s_retry_at;
static uint32_t    s_backoff_ms;
static volatile uint32_t s_reconnects;

uint8_t Esp_IsReady(void)
{
    return (s_state == ESP_ST_PASS) ? 1U : 0U;
}

uint32_t Esp_GetReconnectCount(void)
{
    return s_reconnects;
}

static void Esp_FailStep(uint32_t now)
{
    s_state = ESP_ST_DOWN;
    s_reconnects++;
    s_backoff_ms = (s_backoff_ms == 0U) ? CAN_BACKOFF_INIT_MS : s_backoff_ms * 2U;
    if (s_backoff_ms > CAN_BACKOFF_MAX_MS)
    {
        s_backoff_ms = CAN_BACKOFF_MAX_MS;
    }
    s_retry_at = now + s_backoff_ms;
    (void)LOG_SVC_PostEvent((uint8_t)LOG_MQTT_RECONNECT, 0U, 0U, 0U);
}

/**
  * @brief  Bounded-blocking AT command: send cmd+"\r\n", wait for `expect`
  *         (substring, rolling 160-byte window).  Beats the T4 heartbeat on
  *         every poll so long waits never look like a stall.
  * @retval 0 = expect found, -1 = timeout, -2 = "ERROR" response
  */
static int Esp_AtCmd(const char *cmd, const char *expect, uint32_t timeout_ms)
{
    char     rsp[160];
    uint16_t rlen = 0U;
    uint32_t start = HAL_GetTick();

    Esp_FlushRx();
    Esp_WriteBytes((const uint8_t *)cmd, (uint16_t)strlen(cmd));
    Esp_WriteBytes((const uint8_t *)"\r\n", 2U);

    while ((HAL_GetTick() - start) < timeout_ms)
    {
        uint8_t b;

        App_Hb_Beat(APP_HB_UPLINK);        /* waiting IS progress */

        while (Esp_Read(&b, 1U) != 0U)
        {
            if (rlen >= (uint16_t)(sizeof(rsp) - 1U))
            {
                (void)memmove(rsp, &rsp[64], sizeof(rsp) - 65U);   /* keep tail */
                rlen = (uint16_t)(sizeof(rsp) - 65U);
            }
            rsp[rlen] = (char)b;
            rlen++;
            rsp[rlen] = '\0';

            if (strstr(rsp, expect) != NULL)
            {
                return 0;
            }
            if ((strcmp(expect, "ERROR") != 0) && (strstr(rsp, "ERROR") != NULL))
            {
                return -2;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return -1;
}

int Esp_Service(uint32_t now)
{
    int r = 0;

    switch (s_state)
    {
    case ESP_ST_DOWN:
        if ((int32_t)(now - s_retry_at) >= 0)
        {
            s_state = ESP_ST_RESET;
        }
        break;

    case ESP_ST_RESET:
        /* hard reset; the "ready" banner is the boot marker */
        r = Esp_AtCmd("AT+RST", "ready", 3000U);
        if (r == 0) { s_state = ESP_ST_AT; } else { Esp_FailStep(now); }
        break;

    case ESP_ST_AT:
        r = Esp_AtCmd("AT", "OK", 1000U);
        if (r == 0) { s_state = ESP_ST_CWMODE; } else { Esp_FailStep(now); }
        break;

    case ESP_ST_CWMODE:
        r = Esp_AtCmd("AT+CWMODE=1", "OK", 1000U);
        if (r == 0) { s_state = ESP_ST_JOIN; } else { Esp_FailStep(now); }
        break;

    case ESP_ST_JOIN:
    {
        char cmd[128];

        (void)snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"",
                       ESP_WIFI_SSID, ESP_WIFI_PASSWORD);
        r = Esp_AtCmd(cmd, "OK", 20000U);      /* DHCP can take a while */
        if (r == 0) { s_state = ESP_ST_CIPMUX; } else { Esp_FailStep(now); }
        break;
    }

    case ESP_ST_CIPMUX:
        r = Esp_AtCmd("AT+CIPMUX=0", "OK", 1000U);
        if (r == 0) { s_state = ESP_ST_CIPMODE; } else { Esp_FailStep(now); }
        break;

    case ESP_ST_CIPMODE:
        /* transparent mode: after CIPSEND'>' the socket is a raw byte pipe */
        r = Esp_AtCmd("AT+CIPMODE=1", "OK", 1000U);
        if (r == 0) { s_state = ESP_ST_OPEN; } else { Esp_FailStep(now); }
        break;

    case ESP_ST_OPEN:
    {
        char cmd[128];

        (void)snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u",
                       MQTT_BROKER_HOST, (unsigned)MQTT_BROKER_PORT);
        r = Esp_AtCmd(cmd, "OK", 15000U);      /* "CONNECT\r\n\r\nOK" */
        if (r == 0) { s_state = ESP_ST_CIPSEND; } else { Esp_FailStep(now); }
        break;
    }

    case ESP_ST_CIPSEND:
        r = Esp_AtCmd("AT+CIPSEND", ">", 3000U);
        if (r == 0)
        {
            s_state = ESP_ST_PASS;
            s_backoff_ms = 0U;                 /* healthy: reset the backoff */
            (void)LOG_SVC_PostEvent((uint8_t)LOG_LINK_UP, 2U, 0U, 0U); /* p0=2: wifi */
        }
        else
        {
            Esp_FailStep(now);
        }
        break;

    case ESP_ST_PASS:
        return 1;

    default:
        s_state = ESP_ST_DOWN;
        break;
    }

    return 0;
}

void Esp_LinkLost(void)
{
    s_state = ESP_ST_DOWN;
    s_backoff_ms = (s_backoff_ms == 0U) ? CAN_BACKOFF_INIT_MS : s_backoff_ms * 2U;
    if (s_backoff_ms > CAN_BACKOFF_MAX_MS)
    {
        s_backoff_ms = CAN_BACKOFF_MAX_MS;
    }
    s_retry_at = HAL_GetTick() + s_backoff_ms;
    s_reconnects++;
}
