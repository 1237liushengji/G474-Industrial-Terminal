/**
  ******************************************************************************
  * @file    modbus_slave.c
  * @brief   Modbus RTU slave engine implementation.
  *
  *  Frame receive state machine (v0.4, interrupt RX + t3.5 idle detection):
  *
  *    IDLE --byte--> RECEIVING --byte(within t3.5)--> RECEIVING
  *                        | t3.5 elapsed (Poll)
  *                        v
  *                   FRAME_COMPLETE --addr?-- CRC? --FC dispatch--> response
  *
  *  Errors: CRC fail / foreign address -> frame silently dropped + counted.
  ******************************************************************************
  */
#include "modbus_slave.h"
#include "modbus.h"
#include "modbus_crc.h"
#include "modbus_register.h"
#include "types.h"

/*====================================================================*/
/* Limits                                                              */
/*====================================================================*/
#define MB_SLAVE_RX_BUF_SIZE       MB_RTU_MAX_FRAME_LEN
#define MB_SLAVE_TX_BUF_SIZE       MB_RTU_MAX_FRAME_LEN
#define MB_FC_MAX_QTY_HOLDING      125U
#define MB_FC_MAX_QTY_COILS        2000U
#define MB_FC_MAX_WRITE_REGS       123U   /* max regs per 0x10 frame */

/*====================================================================*/
/* Module state                                                        */
/*====================================================================*/
static uint8_t   s_slave_id = 1U;
static uint32_t  s_t35_us = 4000U;

static uint8_t  s_rx_buf[MB_SLAVE_RX_BUF_SIZE];
static volatile uint16_t s_rx_len = 0U;
static volatile uint32_t s_last_byte_us = 0U;

static uint8_t  s_tx_buf[MB_SLAVE_TX_BUF_SIZE];
static uint8_t  s_resp_payload[1U + 2U * MB_FC_MAX_QTY_HOLDING]; /* shared by FC01/03 */
static mb_slave_tx_func_t s_tx_func = 0;
static mb_slave_event_cb_t s_ev_cb = 0;

static comm_stats_t s_stats;   /* local mirror; copied into regs on use */

/*====================================================================*/
/* Init                                                                */
/*====================================================================*/
void MB_Slave_Init(uint8_t slave_id, uint32_t baudrate)
{
    s_slave_id = slave_id;
    MB_Slave_SetBaudrate(baudrate);
    s_rx_len = 0U;
    s_tx_func = 0;
}

void MB_Slave_SetBaudrate(uint32_t baudrate)
{
    s_t35_us = MB_T35_US(baudrate);
}

void MB_Slave_SetSlaveId(uint8_t slave_id)
{
    s_slave_id = slave_id;
}

void MB_Slave_SetTxFunc(mb_slave_tx_func_t f)
{
    s_tx_func = f;
}

void MB_Slave_SetEventCallback(mb_slave_event_cb_t cb)
{
    s_ev_cb = cb;
}

static void Slave_NotifyEvent(uint8_t event, uint16_t param)
{
    if (s_ev_cb != 0)
    {
        s_ev_cb(event, param);
    }
}

/*====================================================================*/
/* RX byte feeding (ISR context)                                       */
/*====================================================================*/
void MB_Slave_OnRxByte(uint8_t byte, uint32_t now_us)
{
    if (s_rx_len >= (uint16_t)sizeof(s_rx_buf))
    {
        /* buffer overflow: drop frame, restart */
        s_rx_len = 0U;
    }
    s_rx_buf[s_rx_len] = byte;
    s_rx_len++;
    s_last_byte_us = now_us;
}

uint16_t MB_Slave_GetRxLen(void)
{
    return (uint16_t)s_rx_len;
}

void MB_Slave_GetStats(comm_stats_t *out)
{
    if (out != 0)
    {
        *out = s_stats;
    }
}

/*====================================================================*/
/* Response builders                                                   */
/*====================================================================*/
static uint8_t Slave_SendResponse(const uint8_t *payload, uint16_t len)
{
    uint16_t i;

    if (s_tx_func == 0)
    {
        return 0U;
    }
    s_tx_buf[0] = s_slave_id;
    for (i = 0; i < len; i++)
    {
        s_tx_buf[1 + i] = payload[i];
    }
    /* append CRC, total = 1 + len + 2 */
    MODBUS_CRC_Append(s_tx_buf, (uint16_t)(1U + len));
    s_tx_func(s_tx_buf, (uint16_t)(1U + len + 2U));

    s_stats.tx_count++;
    MB_REG_IncTxCount();
    return 1U;
}

/** Send a normal response (function code + data). */
static uint8_t Slave_SendOk(uint8_t func, const uint8_t *data, uint16_t data_len)
{
    uint8_t pdu[MB_RTU_MAX_PDU_LEN];
    uint16_t n = 0U;

    pdu[n++] = func;
    if ((data != 0) && (data_len > 0U))
    {
        for (n = 1U; n <= data_len; n++)
        {
            pdu[n] = data[n - 1U];
        }
        n = (uint16_t)(data_len + 1U);
    }
    return Slave_SendResponse(pdu, n);
}

/** Send an exception response: func|0x80 + exception code. */
static uint8_t Slave_SendException(uint8_t func, uint8_t exc_code)
{
    uint8_t pdu[2];

    pdu[0] = (uint8_t)(func | 0x80U);
    pdu[1] = exc_code;
    MB_REG_SetLastError(exc_code);
    s_stats.exception_count++;
    Slave_NotifyEvent(LOG_EXCEPTION, exc_code);
    return Slave_SendResponse(pdu, 2U);
}

/*====================================================================*/
/* Function code handlers                                              */
/*====================================================================*/

/* 0x01 Read Coils: coil bits packed LSB-first, one byte per 8 coils */
static void Slave_FC01(const uint8_t *req)
{
    uint16_t start = (uint16_t)((req[0] << 8) | req[1]);
    uint16_t qty   = (uint16_t)((req[2] << 8) | req[3]);
    uint8_t  *resp = s_resp_payload;
    uint16_t byte_cnt;
    uint16_t i;

    if (qty == 0U || qty > MB_FC_MAX_QTY_COILS)
    {
        Slave_SendException(MB_FC_READ_COILS, MB_EX_ILLEGAL_VALUE);
        return;
    }
    if ((uint32_t)start + qty > MB_REG_COIL_COUNT)
    {
        Slave_SendException(MB_FC_READ_COILS, MB_EX_ILLEGAL_ADDRESS);
        return;
    }

    byte_cnt = (uint16_t)((qty + 7U) / 8U);
    resp[0] = (uint8_t)byte_cnt;
    for (i = 0; i < byte_cnt; i++)
    {
        uint8_t byte = 0U;
        uint8_t b;
        for (b = 0; b < 8U; b++)
        {
            uint16_t coil_addr = (uint16_t)(start + i * 8U + b);
            if ((coil_addr < start + qty) && (MB_REG_GetCoil(coil_addr) != 0U))
            {
                byte |= (uint8_t)(1U << b);
            }
        }
        resp[1 + i] = byte;
    }
    Slave_SendOk(MB_FC_READ_COILS, resp, (uint16_t)(1U + byte_cnt));
}

/* 0x06 Write Single Register (echo request on success) */
static void Slave_FC06(const uint8_t *req)
{
    uint16_t addr  = (uint16_t)((req[0] << 8) | req[1]);
    uint16_t value = (uint16_t)((req[2] << 8) | req[3]);

    if (addr > MB_REG_HOLD_MAX_ADDR)
    {
        Slave_SendException(MB_FC_WRITE_SINGLE_REG, MB_EX_ILLEGAL_ADDRESS);
        return;
    }
    if (MB_REG_HoldingWritable(addr) == 0U)
    {
        /* write to read-only (measured/stats) region */
        Slave_SendException(MB_FC_WRITE_SINGLE_REG, MB_EX_ILLEGAL_ADDRESS);
        return;
    }
    if (MB_REG_ValidateConfigValue(addr, value) == 0U)
    {
        /* semantic range violation (e.g. slave id 0 / baud idx >4) -
           would brick the node, reject with Illegal Data Value */
        Slave_SendException(MB_FC_WRITE_SINGLE_REG, MB_EX_ILLEGAL_VALUE);
        return;
    }
    MB_REG_SetHolding(addr, value);
    /* echo request back */
    Slave_SendOk(MB_FC_WRITE_SINGLE_REG, req, 4U);
}

/* 0x10 Write Multiple Registers */
static void Slave_FC10(const uint8_t *req, uint16_t pdu_len)
{
    uint16_t start = (uint16_t)((req[0] << 8) | req[1]);
    uint16_t qty   = (uint16_t)((req[2] << 8) | req[3]);
    uint8_t  byte_cnt = req[4];
    uint16_t i;

    if (qty == 0U || qty > MB_FC_MAX_WRITE_REGS)
    {
        Slave_SendException(MB_FC_WRITE_MULTI_REGS, MB_EX_ILLEGAL_VALUE);
        return;
    }
    if (byte_cnt != (uint8_t)(qty * 2U))
    {
        Slave_SendException(MB_FC_WRITE_MULTI_REGS, MB_EX_ILLEGAL_VALUE);
        return;
    }
    /* pdu_len includes [start(2) qty(2) bytecnt(1) data] */
    if ((uint32_t)byte_cnt + 5U > pdu_len)
    {
        Slave_SendException(MB_FC_WRITE_MULTI_REGS, MB_EX_ILLEGAL_VALUE);
        return;
    }
    if ((uint32_t)start + qty > MB_REG_HOLD_COUNT)
    {
        Slave_SendException(MB_FC_WRITE_MULTI_REGS, MB_EX_ILLEGAL_ADDRESS);
        return;
    }
    /* write range must be fully inside config region */
    for (i = 0; i < qty; i++)
    {
        if (MB_REG_HoldingWritable((uint16_t)(start + i)) == 0U)
        {
            Slave_SendException(MB_FC_WRITE_MULTI_REGS, MB_EX_ILLEGAL_ADDRESS);
            return;
        }
        /* semantic range check on every value BEFORE writing anything */
        {
            uint16_t v = (uint16_t)((req[5 + 2U * i] << 8) | req[6 + 2U * i]);
            if (MB_REG_ValidateConfigValue((uint16_t)(start + i), v) == 0U)
            {
                Slave_SendException(MB_FC_WRITE_MULTI_REGS,
                                    MB_EX_ILLEGAL_VALUE);
                return;
            }
        }
    }

    for (i = 0; i < qty; i++)
    {
        uint16_t v = (uint16_t)((req[5 + 2U * i] << 8) | req[6 + 2U * i]);
        MB_REG_SetHolding((uint16_t)(start + i), v);
    }

    /* response: echo start + qty */
    Slave_SendOk(MB_FC_WRITE_MULTI_REGS, req, 4U);
}

/* 0x03 Read Holding Registers */
static void Slave_FC03(const uint8_t *req)
{
    uint16_t start = (uint16_t)((req[0] << 8) | req[1]);
    uint16_t qty   = (uint16_t)((req[2] << 8) | req[3]);
    uint8_t  *resp = s_resp_payload;
    uint16_t i;

    if (qty == 0U || qty > MB_FC_MAX_QTY_HOLDING)
    {
        Slave_SendException(MB_FC_READ_HOLDING_REGS, MB_EX_ILLEGAL_VALUE);
        return;
    }
    if ((uint32_t)start + qty > MB_REG_HOLD_COUNT)
    {
        Slave_SendException(MB_FC_READ_HOLDING_REGS, MB_EX_ILLEGAL_ADDRESS);
        return;
    }

    resp[0] = (uint8_t)(qty * 2U);
    for (i = 0; i < qty; i++)
    {
        uint16_t v = MB_REG_GetHolding((uint16_t)(start + i));
        resp[1 + 2U * i]     = (uint8_t)(v >> 8);
        resp[1 + 2U * i + 1] = (uint8_t)(v & 0xFF);
    }
    Slave_SendOk(MB_FC_READ_HOLDING_REGS, resp, (uint16_t)(1U + qty * 2U));
}

/*====================================================================*/
/* Frame processing                                                    */
/*====================================================================*/
static void Slave_ProcessFrame(void)
{
    uint16_t len = (uint16_t)s_rx_len;
    uint8_t  func;
    uint16_t pdu_len;   /* bytes after func */

    if (len < 4U)
    {
        return;         /* too short: drop silently */
    }

    /* 1. address check (own id only; broadcast = no response) */
    if (s_rx_buf[0] != s_slave_id)
    {
        return;
    }

    /* 2. CRC check */
    if (MODBUS_CRC_Check(s_rx_buf, len) == 0U)
    {
        s_stats.crc_error_count++;
        MB_REG_IncCrcError();
        MB_REG_SetDeviceStatus(2U);   /* comm warning */
        Slave_NotifyEvent(LOG_CRC_ERROR, 0U);
        return;
    }

    /* valid request addressed to us */
    s_stats.rx_count++;
    MB_REG_IncRxCount();
    MB_REG_SetDeviceStatus(1U);

    func = s_rx_buf[1];
    pdu_len = (uint16_t)(len - 2U - 2U);   /* minus addr+func+CRC */

    switch (func)
    {
        case MB_FC_READ_COILS:
            if (pdu_len == 4U)
            {
                Slave_FC01(&s_rx_buf[2]);
            }
            else
            {
                Slave_SendException(func, MB_EX_ILLEGAL_VALUE);
            }
            break;

        case MB_FC_READ_HOLDING_REGS:
            if (pdu_len == 4U)
            {
                Slave_FC03(&s_rx_buf[2]);
            }
            else
            {
                Slave_SendException(func, MB_EX_ILLEGAL_VALUE);
            }
            break;

        case MB_FC_WRITE_SINGLE_REG:
            if (pdu_len == 4U)
            {
                Slave_FC06(&s_rx_buf[2]);
            }
            else
            {
                Slave_SendException(func, MB_EX_ILLEGAL_VALUE);
            }
            break;

        case MB_FC_WRITE_MULTI_REGS:
            if (pdu_len >= 5U)
            {
                Slave_FC10(&s_rx_buf[2], pdu_len);
            }
            else
            {
                Slave_SendException(func, MB_EX_ILLEGAL_VALUE);
            }
            break;

        default:
            /* illegal function */
            Slave_SendException(func, MB_EX_ILLEGAL_FUNCTION);
            break;
    }
}

/*====================================================================*/
/* Poll (main loop)                                                    */
/*====================================================================*/
uint8_t MB_Slave_Poll(uint32_t now_us)
{
    uint8_t sent = 0U;

    /* frame complete when no byte arrived within t3.5 */
    if ((s_rx_len > 0U) &&
        ((uint32_t)(now_us - s_last_byte_us) >= s_t35_us))
    {
        Slave_ProcessFrame();
        s_rx_len = 0U;
        sent = 1U;
    }
    return sent;
}

/*====================================================================*/
/* Whole-frame entry (DMA receive mode)                                */
/*====================================================================*/
uint8_t MB_Slave_OnRxFrame(const uint8_t *frame, uint16_t len)
{
    uint16_t i;

    if ((frame == 0) || (len == 0U) || (len > (uint16_t)sizeof(s_rx_buf)))
    {
        return 0U;      /* invalid: drop */
    }
    /* the frame boundary is already known (t3.5 in the DMA service),
       so parse it directly in the main-loop context */
    for (i = 0U; i < len; i++)
    {
        s_rx_buf[i] = frame[i];
    }
    s_rx_len = len;
    Slave_ProcessFrame();
    s_rx_len = 0U;
    return 1U;
}
