/**
  ******************************************************************************
  * @file    bsp_printf.c
  * @brief   Reentrant formatted output - per-task buffer pool + mutex.
  *
  *  Slot claim policy: a task keeps its slot for its lifetime (tasks never
  *  print from more than a few contexts).  Claiming is done in a critical
  *  section; if all slots are taken (or before the scheduler exists), the
  *  shared slot 0 is used - the mutex still makes the UART write safe, and
  *  the pre-scheduler phase is single-threaded by construction.
  ******************************************************************************
  */
#include "bsp_printf.h"
#include "bsp_console.h"
#include "app_board_cfg.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <stdarg.h>
#include <stdio.h>

typedef struct
{
    TaskHandle_t owner;                      /* NULL = free */
    char         buf[CONSOLE_TX_BUF_LEN];
} bsp_printf_slot_t;

static bsp_printf_slot_t s_pool[CONSOLE_TX_POOL_NUM];
static SemaphoreHandle_t s_tx_mutex;

void BSP_Printf_Init(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();
}

static bsp_printf_slot_t *ClaimSlot(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    bsp_printf_slot_t *slot = 0;
    uint32_t i;

    if (me == 0)
    {
        return &s_pool[0];                   /* pre-scheduler: single-threaded */
    }

    taskENTER_CRITICAL();
    for (i = 0U; i < CONSOLE_TX_POOL_NUM; i++)
    {
        if (s_pool[i].owner == me)
        {
            slot = &s_pool[i];
            break;
        }
    }
    if (slot == 0)
    {
        for (i = 0U; i < CONSOLE_TX_POOL_NUM; i++)
        {
            if (s_pool[i].owner == 0)
            {
                s_pool[i].owner = me;
                slot = &s_pool[i];
                break;
            }
        }
    }
    taskEXIT_CRITICAL();

    if (slot == 0)
    {
        slot = &s_pool[0];                   /* pool exhausted: shared, mutex-protected */
    }
    return slot;
}

int BSP_Printf(const char *fmt, ...)
{
    bsp_printf_slot_t *slot = ClaimSlot();
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(slot->buf, sizeof(slot->buf), fmt, ap);
    va_end(ap);

    if (n < 0)
    {
        return n;
    }
    if ((uint32_t)n >= sizeof(slot->buf))
    {
        n = (int)(sizeof(slot->buf) - 1U);   /* truncated by vsnprintf */
    }

    if (s_tx_mutex != 0)
    {
        (void)xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    }
    BSP_Console_Send((const uint8_t *)slot->buf, (uint16_t)n);
    if (s_tx_mutex != 0)
    {
        (void)xSemaphoreGive(s_tx_mutex);
    }

    return n;
}

void BSP_Puts(const char *s)
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

    if (s_tx_mutex != 0)
    {
        (void)xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    }
    BSP_Console_Send((const uint8_t *)s, n);
    if (s_tx_mutex != 0)
    {
        (void)xSemaphoreGive(s_tx_mutex);
    }
}
