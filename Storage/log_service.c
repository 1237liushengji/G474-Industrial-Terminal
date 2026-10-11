/**
  ******************************************************************************
  * @file    log_service.c
  * @brief   Queue-driven log/config service, executed by log_task (T5).
  ******************************************************************************
  */
#include "log_service.h"
#include "log.h"
#include "bsp_w25q128.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "main.h"      /* RCC reset flags */

/*====================================================================*/
/* Request queue                                                       */
/*====================================================================*/
#define LOG_SVC_Q_LEN       24U

typedef enum
{
    LOG_REQ_EVENT = 0,
    LOG_REQ_CONFIG_SAVE = 1,
    LOG_REQ_PENDING = 2           /* v0.6: offline MQTT pending record */
} log_req_kind_t;

typedef struct
{
    uint8_t        kind;
    uint8_t        ev;          /* LOG_REQ_EVENT / LOG_REQ_PENDING: msg type */
    uint8_t        p0;
    uint8_t        p1;
    uint8_t        p2;
    config_param_t cfg;         /* LOG_REQ_CONFIG_SAVE */
    uint8_t        fields[20];  /* LOG_REQ_PENDING */
} log_req_t;

static QueueHandle_t s_q;

/* config-save completion flag shared with the requesting task:
 * 0 = nothing new, 1 = done OK, 2 = done FAILED.  Read-and-clear via
 * critical section (writer is log_task, reader is the mb task). */
static volatile uint8_t s_cfg_done = 0U;

static volatile uint32_t s_drop_count = 0U;
static volatile uint32_t s_reset_flags = 0U;

/*====================================================================*/
/* Init                                                                */
/*====================================================================*/
void LOG_SVC_Init(void)
{
    s_q = xQueueCreate(LOG_SVC_Q_LEN, sizeof(log_req_t));
}

void LOG_SVC_RunInit(void)
{
    /* Capture-and-clear the reset cause before anything else can set bits */
    s_reset_flags = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF;

    (void)W25Q_Init();
    LOG_SetTickSource(HAL_GetTick);
    LOG_Init();                                  /* blocking area scan, by design */

    /* Boot record with the reset cause: p0 bit0=pin, bit1=software,
     * bit2=IWDG, bit3=WWDG, bit4=BOR, bit5=low-power (G4 has no separate
     * POR flag - a power-on maps to BORRSTF) */
    {
        uint32_t csr = s_reset_flags;
        uint8_t  bits = 0U;

        if ((csr & RCC_CSR_PINRSTF)  != 0U) { bits |= 0x01U; }
        if ((csr & RCC_CSR_SFTRSTF)  != 0U) { bits |= 0x02U; }
        if ((csr & RCC_CSR_IWDGRSTF) != 0U) { bits |= 0x04U; }
        if ((csr & RCC_CSR_WWDGRSTF) != 0U) { bits |= 0x08U; }
        if ((csr & RCC_CSR_BORRSTF)  != 0U) { bits |= 0x10U; }
        if ((csr & RCC_CSR_LPWRRSTF) != 0U) { bits |= 0x20U; }

        LOG_WriteEvent(LOG_BOOT, bits, 0U, 0U);
    }
}

/*====================================================================*/
/* Producer side                                                       */
/*====================================================================*/
static uint8_t LogSvc_Enqueue(const log_req_t *req)
{
    if ((s_q == 0) || (xQueueSend(s_q, req, 0U) != pdTRUE))
    {
        s_drop_count++;
        return 0U;
    }
    return 1U;
}

uint8_t LOG_SVC_PostEvent(uint8_t ev, uint8_t p0, uint8_t p1, uint8_t p2)
{
    log_req_t req;

    req.kind = (uint8_t)LOG_REQ_EVENT;
    req.ev   = ev;
    req.p0   = p0;
    req.p1   = p1;
    req.p2   = p2;
    return LogSvc_Enqueue(&req);
}

uint8_t LOG_SVC_PostConfigSave(const config_param_t *p)
{
    log_req_t req;

    if (p == 0)
    {
        return 0U;
    }
    req.kind = (uint8_t)LOG_REQ_CONFIG_SAVE;
    req.ev   = 0U;
    req.p0   = 0U;
    req.p1   = 0U;
    req.p2   = 0U;
    req.cfg  = *p;
    return LogSvc_Enqueue(&req);
}

uint8_t LOG_SVC_PostPending(uint8_t type, uint8_t seq_lo, const uint8_t *fields)
{
    log_req_t req;
    uint8_t i;

    if (fields == 0)
    {
        return 0U;
    }
    req.kind = (uint8_t)LOG_REQ_PENDING;
    req.ev   = type;
    req.p0   = 0U;
    req.p1   = seq_lo;
    req.p2   = 0U;
    for (i = 0U; i < 20U; i++)
    {
        req.fields[i] = fields[i];
    }
    return LogSvc_Enqueue(&req);
}

uint8_t LOG_SVC_TakeConfigSaveDone(void)
{
    uint8_t v;

    taskENTER_CRITICAL();
    v = s_cfg_done;
    s_cfg_done = 0U;
    taskEXIT_CRITICAL();
    return v;
}

/*====================================================================*/
/* Consumer side (log_task)                                            */
/*====================================================================*/
void LOG_SVC_Process(void)
{
    log_req_t req;

    /* 1000 ms receive timeout (HW bring-up 2026-10-11, docs §18.6):
     * an IDLE log task must still tick its heartbeat every second.  With
     * portMAX_DELAY the beat stops while the queue is empty, which the
     * watchdog (LOG deadline = 4 s) misreads as a stall and then stops
     * feeding the IWDG -> a 4-5 s reset loop.  The timeout costs one
     * wake-up per second when idle and changes nothing when busy. */
    if ((s_q == 0) || (xQueueReceive(s_q, &req, pdMS_TO_TICKS(1000)) != pdTRUE))
    {
        return;
    }

    if (req.kind == (uint8_t)LOG_REQ_CONFIG_SAVE)
    {
        if (CONFIG_Save(&req.cfg) != 0U)
        {
            LOG_WriteEvent(LOG_CONFIG_CHANGE, req.cfg.slave_id,
                           req.cfg.baud_idx, 0U);
            s_cfg_done = 1U;
        }
        else
        {
            s_cfg_done = 2U;

            /* Failure is reported via TakeConfigSaveDone()==2; the dirty
             * flag stays set on the caller side -> it retries (20 ms cadence,
             * same policy as v2.0). */
        }
    }
    else if (req.kind == (uint8_t)LOG_REQ_PENDING)
    {
        (void)LOG_PendingPush(req.ev, req.p1, req.fields);
    }
    else
    {
        LOG_WriteEvent((log_event_t)req.ev, req.p0, req.p1, req.p2);
    }
}

uint32_t LOG_SVC_GetDropCount(void)
{
    return s_drop_count;
}

uint32_t LOG_SVC_GetResetFlags(void)
{
    return s_reset_flags;
}
