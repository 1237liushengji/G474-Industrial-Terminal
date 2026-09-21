/**
  ******************************************************************************
  * @file    modbus_master.c
  * @brief   Modbus RTU master engine (v0.7: poll + one-shot requests;
  *          v0.8: full timeout/retry/statistics).
  *
  *  State machine:
  *   IDLE --request queued--> SEND (frame built, TX)
  *   SEND --> WAIT (response window, timeout timer running)
  *   WAIT --valid response--> DONE(result) --> IDLE
  *   WAIT --timeout--> retry? SEND again : DONE(timeout) --> IDLE
  *   WAIT --exception frame--> DONE(exception) --> IDLE
  ******************************************************************************
  */
#include "modbus_master.h"
#include "modbus.h"
#include "modbus_crc.h"

/*====================================================================*/
/* States                                                              */
/*====================================================================*/
typedef enum {
    MASTER_IDLE = 0,
    MASTER_SEND,
    MASTER_WAIT
} master_state_t;

/*====================================================================*/
/* Module state                                                        */
/*====================================================================*/
static uint8_t  s_slave_id = 1U;
static uint32_t s_t35_us = 4000U;
static uint32_t s_timeout_us = 100000U;   /* 100 ms per attempt @115200 */
static uint32_t s_baudrate = 115200U;
static uint8_t  s_retry_max = 2U;         /* total tries = retry_max+1 */

static mb_master_tx_func_t s_tx_func = 0;
static mb_master_event_cb_t s_ev_cb = 0;

static master_state_t s_state = MASTER_IDLE;
static uint8_t  s_retry_left = 0U;
static uint32_t s_sent_us = 0U;

static uint8_t  s_req[MB_RTU_MAX_FRAME_LEN];
static uint16_t s_req_len = 0U;

static uint8_t  s_rx_buf[MB_RTU_MAX_FRAME_LEN];
static volatile uint16_t s_rx_len = 0U;
static volatile uint32_t s_last_byte_us = 0U;
static uint32_t s_first_byte_us = 0U;   /* set on first response byte */

static comm_stats_t s_stats;
static mb_master_result_t s_result;
static uint16_t s_consec_fails = 0U;

/*====================================================================*/
/* Init / accessors                                                    */
/*====================================================================*/
void MB_Master_Init(uint8_t slave_id, uint32_t baudrate,
                    uint32_t timeout_ms, uint8_t retry_max)
{
    uint32_t scale;

    s_slave_id = slave_id;
    s_baudrate = baudrate;
    s_t35_us = MB_T35_US(baudrate);

    /* Timeout is specified for 115200. Scale it by baud ratio so that a
       large response (e.g. 125 regs = 253 bytes) at 9600 baud (~292 ms on
       the wire) is not misjudged as a timeout. */
    if (baudrate == 0U)
    {
        scale = 12U;      /* worst case: assume 9600 */
    }
    else
    {
        scale = 115200U / baudrate;
        if (scale < 1U)
        {
            scale = 1U;
        }
        if (scale > 20U)
        {
            scale = 20U;  /* clamp: never exceed ~2 s */
        }
    }
    s_timeout_us = timeout_ms * 1000U * scale;

    s_retry_max = retry_max;
    s_state = MASTER_IDLE;
    s_rx_len = 0U;
    s_tx_func = 0;
    s_first_byte_us = 0U;
}

void MB_Master_SetTxFunc(mb_master_tx_func_t f)
{
    s_tx_func = f;
}

void MB_Master_SetEventCallback(mb_master_event_cb_t cb)
{
    s_ev_cb = cb;
}

static void Master_NotifyEvent(uint8_t event, uint16_t param)
{
    if (s_ev_cb != 0)
    {
        s_ev_cb(event, param);
    }
}

void MB_Master_OnRxByte(uint8_t byte, uint32_t now_us)
{
    if (s_state == MASTER_WAIT)
    {
        if (s_rx_len >= (uint16_t)sizeof(s_rx_buf))
        {
            s_rx_len = 0U;      /* overflow -> restart capture */
            s_first_byte_us = now_us;
        }
        else if (s_rx_len == 0U)
        {
            s_first_byte_us = now_us;   /* first byte of the response */
        }
        s_rx_buf[s_rx_len] = byte;
        s_rx_len++;
        s_last_byte_us = now_us;
    }
    /* bytes outside WAIT are ignored (we are not listening) */
}

uint8_t MB_Master_IsBusy(void)
{
    return (s_state != MASTER_IDLE) ? 1U : 0U;
}

uint8_t MB_Master_CommOk(void)
{
    return (s_consec_fails == 0U) ? 1U : 0U;
}

uint16_t MB_Master_GetConsecutiveFails(void)
{
    return s_consec_fails;
}

void MB_Master_SetTiming(uint32_t timeout_ms, uint8_t retry_max)
{
    s_timeout_us = timeout_ms * 1000U;
    s_retry_max = retry_max;
}

const mb_master_result_t *MB_Master_GetResult(void)
{
    return &s_result;
}

void MB_Master_GetStats(comm_stats_t *out)
{
    if (out != 0)
    {
        *out = s_stats;
    }
}

/*====================================================================*/
/* Request builders                                                    */
/*====================================================================*/
static uint8_t Master_QueueRequest(const uint8_t *frame, uint16_t len,
                                   uint8_t func, uint8_t slave)
{
    if (s_state != MASTER_IDLE)
    {
        return 0U;      /* busy */
    }
    if (len > sizeof(s_req))
    {
        return 0U;
    }

    /* keep frame without CRC; CRC appended at send time (retries reuse) */
    memcpy(s_req, frame, len);
    s_req_len = len;
    s_result.active = 1U;
    s_result.ok = 0U;
    s_result.exception = 0U;
    s_result.timed_out = 0U;
    s_result.func = func;
    s_result.slave_id = slave;
    s_result.payload_len = 0U;
    s_retry_left = s_retry_max;
    s_state = MASTER_SEND;
    return 1U;
}

uint8_t MB_Master_ReadCoils(uint8_t slave, uint16_t start, uint16_t qty)
{
    uint8_t f[6];
    uint16_t i = 0;

    if (slave == MB_ADDR_BROADCAST || slave > MB_ADDR_MAX) return 0U;
    f[i++] = slave;
    f[i++] = MB_FC_READ_COILS;
    f[i++] = (uint8_t)(start >> 8);
    f[i++] = (uint8_t)(start & 0xFF);
    f[i++] = (uint8_t)(qty >> 8);
    f[i++] = (uint8_t)(qty & 0xFF);
    return Master_QueueRequest(f, i, MB_FC_READ_COILS, slave);
}

uint8_t MB_Master_ReadHolding(uint8_t slave, uint16_t start, uint16_t qty)
{
    uint8_t f[6];
    uint16_t i = 0;

    if (slave == MB_ADDR_BROADCAST || slave > MB_ADDR_MAX) return 0U;
    f[i++] = slave;
    f[i++] = MB_FC_READ_HOLDING_REGS;
    f[i++] = (uint8_t)(start >> 8);
    f[i++] = (uint8_t)(start & 0xFF);
    f[i++] = (uint8_t)(qty >> 8);
    f[i++] = (uint8_t)(qty & 0xFF);
    return Master_QueueRequest(f, i, MB_FC_READ_HOLDING_REGS, slave);
}

uint8_t MB_Master_WriteSingle(uint8_t slave, uint16_t addr, uint16_t value)
{
    uint8_t f[6];
    uint16_t i = 0;

    if (slave == MB_ADDR_BROADCAST || slave > MB_ADDR_MAX) return 0U;
    f[i++] = slave;
    f[i++] = MB_FC_WRITE_SINGLE_REG;
    f[i++] = (uint8_t)(addr >> 8);
    f[i++] = (uint8_t)(addr & 0xFF);
    f[i++] = (uint8_t)(value >> 8);
    f[i++] = (uint8_t)(value & 0xFF);
    return Master_QueueRequest(f, i, MB_FC_WRITE_SINGLE_REG, slave);
}

uint8_t MB_Master_WriteMulti(uint8_t slave, uint16_t start,
                             const uint16_t *values, uint16_t qty)
{
    uint8_t f[6 + 2U * 123U];
    uint16_t i = 0;
    uint16_t n;

    if (slave == MB_ADDR_BROADCAST || slave > MB_ADDR_MAX) return 0U;
    if (qty == 0U || qty > 123U) return 0U;

    f[i++] = slave;
    f[i++] = MB_FC_WRITE_MULTI_REGS;
    f[i++] = (uint8_t)(start >> 8);
    f[i++] = (uint8_t)(start & 0xFF);
    f[i++] = (uint8_t)(qty >> 8);
    f[i++] = (uint8_t)(qty & 0xFF);
    f[i++] = (uint8_t)(qty * 2U);
    for (n = 0; n < qty; n++)
    {
        f[i++] = (uint8_t)(values[n] >> 8);
        f[i++] = (uint8_t)(values[n] & 0xFF);
    }
    return Master_QueueRequest(f, i, MB_FC_WRITE_MULTI_REGS, slave);
}

/*====================================================================*/
/* Send                                                                 */
/*====================================================================*/
static void Master_Send(void)
{
    uint8_t frame[MB_RTU_MAX_FRAME_LEN];
    uint16_t len;

    memcpy(frame, s_req, s_req_len);
    len = MODBUS_CRC_Append(frame, s_req_len);

    if (s_tx_func != 0)
    {
        s_tx_func(frame, len);
    }
    s_stats.tx_count++;
    s_rx_len = 0U;
    s_first_byte_us = 0U;
    s_state = MASTER_WAIT;
    s_sent_us = 0U;     /* set by caller Poll with current time */
}

/*====================================================================*/
/* Response processing                                                 */
/*====================================================================*/
static void Master_HandleResponse(void)
{
    uint16_t len = (uint16_t)s_rx_len;

    s_rx_len = 0U;

    /* minimum valid: addr + func + crc(2) */
    if (len < 4U)
    {
        /* garbage: treat as no response -> retry path */
        s_state = MASTER_WAIT;   /* keep waiting until timeout */
        return;
    }
    /* address must match the slave we asked */
    if (s_rx_buf[0] != s_result.slave_id)
    {
        s_state = MASTER_WAIT;
        return;
    }
    /* CRC */
    if (MODBUS_CRC_Check(s_rx_buf, len) == 0U)
    {
        s_stats.crc_error_count++;
        s_state = MASTER_WAIT;
        return;
    }

    /* valid frame */
    if ((s_rx_buf[1] & 0x80U) != 0U)
    {
        /* exception response: link works, task failed */
        s_result.ok = 0U;
        s_result.exception = (uint8_t)(s_rx_buf[2] & 0xFFU);
        s_result.timed_out = 0U;
        s_result.payload_len = 0U;
        s_stats.exception_count++;
        s_consec_fails = 0U;   /* slave answered -> link is healthy */
        s_state = MASTER_IDLE;
        return;
    }

    if (s_rx_buf[1] != s_result.func)
    {
        /* unexpected function code: not our response */
        s_state = MASTER_WAIT;
        return;
    }

    /* normal response: copy payload (after func byte) */
    {
        uint16_t payload_len = (uint16_t)(len - 2U - 2U); /* minus addr+func+crc */
        uint16_t n;

        if (payload_len > sizeof(s_result.payload))
        {
            payload_len = sizeof(s_result.payload);
        }
        for (n = 0; n < payload_len; n++)
        {
            s_result.payload[n] = s_rx_buf[2 + n];
        }
        s_result.payload_len = payload_len;
    }
    s_result.ok = 1U;
    s_result.exception = 0U;
    s_result.timed_out = 0U;
    s_stats.rx_count++;
    s_consec_fails = 0U;
    s_state = MASTER_IDLE;
}

/*====================================================================*/
/* Timeout handling: retry or give up (shared by Poll and OnRxFrame)   */
/*====================================================================*/
static uint8_t Master_TimeoutAction(void)
{
    uint8_t finished = 0U;

    s_rx_len = 0U;
    s_stats.timeout_count++;
    if (s_retry_left > 0U)
    {
        s_retry_left--;
        s_stats.retry_count++;
        s_state = MASTER_SEND;      /* retransmit next poll */
    }
    else
    {
        /* give up */
        s_result.ok = 0U;
        s_result.exception = 0U;
        s_result.timed_out = 1U;
        s_result.payload_len = 0U;
        s_consec_fails++;
        s_state = MASTER_IDLE;
        finished = 1U;
        Master_NotifyEvent(LOG_TIMEOUT, (uint16_t)s_result.slave_id);
    }
    return finished;
}

/*====================================================================*/
/* Whole-frame entry (DMA receive mode)                                */
/*====================================================================*/
uint8_t MB_Master_OnRxFrame(const uint8_t *frame, uint16_t len)
{
    uint16_t i;

    if (s_state != MASTER_WAIT)
    {
        return 0U;      /* not expecting a response */
    }
    if ((frame == 0) || (len < 4U) || (len > (uint16_t)sizeof(s_rx_buf)))
    {
        /* garbage -> retry immediately */
        return Master_TimeoutAction();
    }
    for (i = 0U; i < len; i++)
    {
        s_rx_buf[i] = frame[i];
    }
    s_rx_len = len;
    Master_HandleResponse();
    if (s_state == MASTER_IDLE)
    {
        return 1U;      /* valid response processed */
    }
    /* invalid frame (bad addr/crc/func): do not wait for timeout */
    return Master_TimeoutAction();
}

/*====================================================================*/
/* Poll (main loop)                                                    */
/*====================================================================*/
uint8_t MB_Master_Poll(uint32_t now_us)
{
    uint8_t finished = 0U;

    switch (s_state)
    {
        case MASTER_SEND:
            Master_Send();
            s_sent_us = now_us;
            break;

        case MASTER_WAIT:
            /* response complete? (t3.5 idle after last byte) */
            if ((s_rx_len > 0U) &&
                ((uint32_t)(now_us - s_last_byte_us) >= s_t35_us))
            {
                Master_HandleResponse();
                if (s_state == MASTER_IDLE)
                {
                    finished = 1U;
                }
                /* else: invalid -> keep waiting for timeout/retry */
            }

            /* timeout check: measured from send, or from the first response
               byte once a response has started arriving (so a slow big
               response at low baud is not cut off mid-frame) */
            if (s_state == MASTER_WAIT)
            {
                uint32_t elapsed;
                if (s_rx_len > 0U)
                {
                    /* response in flight: allow full max frame duration */
                    elapsed = (uint32_t)(now_us - s_first_byte_us);
                    elapsed += MB_T35_US(s_baudrate);
                }
                else
                {
                    elapsed = (uint32_t)(now_us - s_sent_us);
                }
                if (elapsed >= s_timeout_us)
                {
                    if (Master_TimeoutAction())
                    {
                        finished = 1U;
                    }
                }
            }
            break;

        default:
            break;
    }
    return finished;
}
