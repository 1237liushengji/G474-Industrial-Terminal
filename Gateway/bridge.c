/**
  ******************************************************************************
  * @file    bridge.c
  * @brief   CAN <-> register bridge + snapshot channel - T3 body (v0.4).
  *
  *  Threading contract:
  *   - everything in this file runs in ONE task (bridge_task, T3) except the
  *     thin RX handlers which run in can_task (T1).  The RX handlers only
  *     touch volatile single words or push into the snapshot RX queue; the
  *     ISO-TP snapshot context is stepped exclusively by bridge_task (frames
  *     are handed over through s_snap_q), so no context is shared unsafely.
  *   - register mirror writes use MB_REG_SetHoldingRaw (no persist dirty).
  ******************************************************************************
  */
#include "bridge.h"
#include "can_contract.h"
#include "app_board_cfg.h"
#include "mqtt_client.h"
#include "uplink_port.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "isotp.h"
#include "modbus.h"
#include "modbus_crc.h"
#include "modbus_register.h"
#include "log_service.h"
#include "types.h"
#include "bsp_printf.h"
#include "app_rtos.h"

#if ROLE_IS_GW
#include "modbus_master.h"
#else
#include "modbus_slave.h"
#endif

/*====================================================================*/
/* Tunables                                                            */
/*====================================================================*/
#define BRIDGE_PERIOD_MS       10U
#define CANC_TELE_PERIOD_MS    1000U
#define CANC_STATUS_PERIOD_MS  5000U
#define CANC_CNTRS_PERIOD_MS   10000U
#define CANC_HB_PERIOD_MS      1000U
#define CANC_SNAP_PERIOD_MS    30000U
#define CANC_SNAP_RETRY_MS     200U
#define CANC_SNAP_TIMEOUT_MS   2000U
#define CANC_CMD_TIMEOUT_MS    500U
#define SNAP_RX_Q_LEN          8U

/*====================================================================*/
/* Shared state                                                        */
/*====================================================================*/
static isotp_ctx_t   s_snap_tp;              /* snapshot ISO-TP channel   */
static QueueHandle_t s_snap_q;               /* RX frames -> bridge task  */

static volatile uint32_t s_peer_hb_ms;       /* last peer HB (0 = never)  */
static volatile uint8_t  s_peer_up;
static volatile uint8_t  s_peer_seq_lo;      /* last seq byte seen on peer */
static uint16_t          s_hb_seq;           /* own publish seq (reg 0x1C) */
static uint32_t          s_tx_hb_last;

#if ROLE_IS_GW
static uint32_t          s_tx_snap_last;
static volatile uint8_t  s_snap_now;         /* request on next loop      */
static uint8_t           s_snap_uid;
static uint32_t          s_snap_sent_ms;
static uint8_t           s_snap_waiting;
static volatile uint32_t s_snap_ok;
static volatile uint32_t s_snap_err;
static volatile uint32_t s_snap_last_ms;
static volatile uint32_t s_snap_hash_mismatch;
static uint16_t          s_snap_regs[CANC_SNAP_REG_COUNT]; /* full shadow  */
static volatile uint8_t  s_snap_regs_valid;

static volatile uint8_t  s_cmd_req;
static volatile uint16_t s_cmd_req_reg;
static volatile uint16_t s_cmd_req_val;
static uint8_t           s_cmd_wait;
static uint8_t           s_cmd_id;
static uint8_t           s_cmd_retry;
static uint32_t          s_cmd_sent_ms;
static volatile uint8_t  s_cmd_result;
static volatile uint8_t  s_cmd_exc;
#else
static uint32_t          s_tx_tele_last;
static uint32_t          s_tx_status_last;
static uint32_t          s_tx_cntrs_last;
static volatile uint8_t  s_snap_send_pending;

/* event uplink (single slot, overwrite on burst) */
static volatile uint8_t  s_ev_pending;
static volatile uint8_t  s_ev_ev;
static volatile uint8_t  s_ev_p0;
static volatile uint8_t  s_ev_p1;
static volatile uint8_t  s_ev_p2;

/* cmd_id idempotency cache: last 8 processed commands */
static uint8_t s_seen_id[8];
static uint8_t s_seen_res[8];
static uint8_t s_seen_exc[8];
static uint8_t s_seen_cnt;
static uint8_t s_seen_pos;
static volatile uint32_t s_cmd_bad;          /* bad sentinel/short frames  */
#endif

/*====================================================================*/
/* Snapshot ISO-TP plumbing                                            */
/*====================================================================*/
#if ROLE_IS_GW
static void Gw_SnapProcess(const uint8_t *p, uint16_t len);

static void Snap_RxCallback(const uint8_t *data, uint16_t len)
{
    Gw_SnapProcess(data, len);
}

static void Snap_ErrCallback(IsoTp_Result result)
{
    (void)result;
    s_snap_err++;
    s_snap_waiting = 0U;
}
#else
static void Snap_RxCallback(const uint8_t *data, uint16_t len)
{
    if ((len == CANC_SNAPREQ_PAYLOAD_LEN) && (data[0] == CANC_SNAPREQ_SUBCMD))
    {
        s_snap_send_pending = 1U;               /* served by the loop */
    }
}

static void Snap_ErrCallback(IsoTp_Result result)
{
    (void)result;
    /* node side: a failed snapshot send is simply retried on the next
     * request; no counter of its own (keeps the RAM/state surface small). */
}
#endif

/*====================================================================*/
/* RX handlers (can_task context - keep short, no blocking)            */
/*====================================================================*/
#if ROLE_IS_GW
void Bridge_RxTelemetry(const CAN_RxMsg *m)
{
    MB_REG_SetHoldingRaw(MB_REG_HOLD_TEMP, CANC_GetU16BE(&m->data[0]));
    MB_REG_SetHoldingRaw(MB_REG_HOLD_HUMI, CANC_GetU16BE(&m->data[2]));
    MB_REG_SetHoldingRaw(MB_REG_HOLD_VOLT, CANC_GetU16BE(&m->data[4]));
    MB_REG_SetHoldingRaw(MB_REG_HOLD_CURR, CANC_GetU16BE(&m->data[6]));
}

void Bridge_RxStatus(const CAN_RxMsg *m)
{
    MB_REG_SetHoldingRaw(MB_REG_HOLD_STATUS, m->data[0]);
    MB_REG_SetHoldingRaw(MB_REG_HOLD_ERRCODE, m->data[1]);
    /* data[2] link mask / [3] hash lo / [4] cv / [5] form / [6] fw are the
     * node's board-local metadata - the gateway keeps its own values. */
    s_peer_seq_lo = m->data[7];
}

void Bridge_RxCounters(const CAN_RxMsg *m)
{
    MB_REG_SetHoldingRaw(MB_REG_HOLD_RXCNT, CANC_GetU16BE(&m->data[0]));
    MB_REG_SetHoldingRaw(MB_REG_HOLD_TXCNT, CANC_GetU16BE(&m->data[2]));
    MB_REG_SetHoldingRaw(MB_REG_HOLD_CRCERR, CANC_GetU16BE(&m->data[4]));
    MB_REG_SetHoldingRaw(MB_REG_HOLD_ADC1_RAW, CANC_GetU16BE(&m->data[6]));
}

void Bridge_RxEvent(const CAN_RxMsg *m)
{
    if (m->data[7] != CANC_EVENT_SENTINEL)
    {
        return;
    }
    (void)LOG_SVC_PostEvent(m->data[0], m->data[1], m->data[2], m->data[3]);
    /* mirror the node's event onto the uplink event topic (v0.5) */
    Uplink_NotifyEvent(m->data[0], m->data[1], m->data[2], m->data[3]);

    /* a node config change is a mirror-visible event: pull a fresh snapshot
     * instead of waiting for the 30 s tick (docs/05 v0.4 verification). */
    if (m->data[0] == (uint8_t)LOG_CONFIG_CHANGE)
    {
        s_snap_now = 1U;
    }
}

void Bridge_RxCmdAck(const CAN_RxMsg *m)
{
    uint8_t id = m->data[CANC_ACK_B_ID];
    uint8_t result = m->data[CANC_ACK_B_RESULT];
    uint8_t exc = m->data[CANC_ACK_B_EXCCODE];

    if (m->data[CANC_ACK_B_SENTINEL] != CANC_ACK_SENTINEL)
    {
        return;
    }
    if ((s_cmd_wait != 0U) && (id == s_cmd_id))
    {
        s_cmd_wait = 0U;
        s_cmd_result = result;
        s_cmd_exc = exc;
        MB_REG_SetHoldingRaw(MB_REG_HOLD_CMD_ID, id);
        MB_REG_SetHoldingRaw(MB_REG_HOLD_CMD_RESULT, result);
        MB_REG_SetHoldingRaw(MB_REG_HOLD_CMD_EXCCODE, exc);
        BSP_Printf("cmd_ack id=%u ok=%u ex=%u\r\n",
                   (unsigned)id, (unsigned)result, (unsigned)exc);
    }
}
#else  /* ================= NODE ================= */
static void Node_SendCmdAck(uint8_t id, uint8_t result, uint8_t exc,
                            uint16_t reg, uint16_t val)
{
    uint8_t d[CAN_PAYLOAD_LEN];

    d[CANC_ACK_B_ID]       = id;
    d[CANC_ACK_B_RESULT]   = result;
    d[CANC_ACK_B_EXCCODE]  = exc;
    CANC_PutU16BE(&d[CANC_ACK_B_ADDR_H], reg);
    CANC_PutU16BE(&d[CANC_ACK_B_VAL_H], val);
    d[6] = 0U;
    d[CANC_ACK_B_SENTINEL] = CANC_ACK_SENTINEL;
    (void)CAN_Send(CANC_ID_CMD_ACK, d, CAN_PAYLOAD_LEN);
}

void Bridge_RxCmd(const CAN_RxMsg *m)
{
    uint16_t reg;
    uint16_t val;
    uint8_t  id;
    uint8_t  fc;
    uint8_t  result;
    uint8_t  exc;
    uint8_t  dup = 0U;
    uint8_t  i;

    if ((m->len < CAN_PAYLOAD_LEN) || (m->data[CANC_CMD_B_SENTINEL] != CANC_CMD_SENTINEL))
    {
        s_cmd_bad++;
        return;
    }

    fc  = m->data[CANC_CMD_B_FC];
    id  = m->data[CANC_CMD_B_ID];
    reg = CANC_GetU16BE(&m->data[CANC_CMD_B_ADDR_H]);
    val = CANC_GetU16BE(&m->data[CANC_CMD_B_VAL_H]);

    /* idempotency: a repeated cmd_id re-ACKs WITHOUT re-executing */
    for (i = 0U; i < s_seen_cnt; i++)
    {
        if (s_seen_id[i] == id)
        {
            dup = 1U;
            result = s_seen_res[i];
            exc = s_seen_exc[i];
            break;
        }
    }

    if (dup == 0U)
    {
        if (fc != 0x06U)
        {
            /* v0.4 implements the 0x06 single-write command; 0x10 multi is
             * rejected visibly instead of being ignored (audit-friendly). */
            result = 2U;
            exc = MB_EX_ILLEGAL_FUNCTION;
        }
        else if (MB_REG_HoldingWritable(reg) == 0U)
        {
            result = 2U;
            exc = MB_EX_ILLEGAL_ADDRESS;
        }
        else if (MB_REG_ValidateConfigValue(reg, val) == 0U)
        {
            result = 2U;
            exc = MB_EX_ILLEGAL_VALUE;
        }
        else
        {
            /* same path as an RS485 write: MB_REG_SetHolding raises the
             * persist-dirty flag for 0x09-0x0E, Node_ConfigSaveService then
             * saves to 24C02 - "apply + ACK now, flash write follows"
             * (docs/02 §4.2).  CAN downlink deliberately has NO second
             * config-apply logic of its own. */
            MB_REG_SetHolding(reg, val);
            result = 1U;
            exc = 0U;
        }

        s_seen_id[s_seen_pos]  = id;
        s_seen_res[s_seen_pos] = result;
        s_seen_exc[s_seen_pos] = exc;
        s_seen_pos = (uint8_t)((s_seen_pos + 1U) & 0x07U);
        if (s_seen_cnt < 8U)
        {
            s_seen_cnt++;
        }
    }

    MB_REG_SetHoldingRaw(MB_REG_HOLD_CMD_RESULT, result);
    MB_REG_SetHoldingRaw(MB_REG_HOLD_CMD_EXCCODE, exc);
    Node_SendCmdAck(id, result, exc, reg, val);
}
#endif /* ROLE_IS_GW */

void Bridge_RxHeartbeat(const CAN_RxMsg *m)
{
    s_peer_hb_ms = HAL_GetTick();
    s_peer_seq_lo = m->data[CANC_HB_B_SEQ];
}

void Bridge_RxSnapshotFrame(const CAN_RxMsg *m)
{
    /* hand over to bridge_task: the snapshot ISO-TP context is stepped by
     * exactly one task, so RX feeds go through this small queue. */
    if ((s_snap_q != 0) && (xQueueSend(s_snap_q, m, 0U) != pdTRUE))
    {
#if ROLE_IS_GW
        s_snap_err++;
#endif
    }
}

/*====================================================================*/
/* Node: periodic producers                                            */
/*====================================================================*/
static void Bridge_TxHb(void)
{
    uint8_t d[CANC_HB_B_ROLE + 1U];

    CANC_PutU16BE(&d[CANC_HB_B_UPTIME_H], (uint16_t)(HAL_GetTick() / 1000U));
    d[CANC_HB_B_SEQ]  = (uint8_t)(s_hb_seq & 0xFFU);
    d[CANC_HB_B_ROLE] = (uint8_t)NODE_ID;

    (void)CAN_Send(ROLE_IS_GW ? CANC_ID_HB_GW : CANC_ID_HB_NODE,
                   d, (uint8_t)sizeof(d));

    s_hb_seq++;
    MB_REG_SetHoldingRaw(MB_REG_HOLD_HB_SEQ, s_hb_seq);
}

#if !ROLE_IS_GW
static void Node_TxTelemetry(void)
{
    uint8_t d[CAN_PAYLOAD_LEN];

    CANC_PutU16BE(&d[0], MB_REG_GetHolding(MB_REG_HOLD_TEMP));
    CANC_PutU16BE(&d[2], MB_REG_GetHolding(MB_REG_HOLD_HUMI));
    CANC_PutU16BE(&d[4], MB_REG_GetHolding(MB_REG_HOLD_VOLT));
    CANC_PutU16BE(&d[6], MB_REG_GetHolding(MB_REG_HOLD_CURR));
    (void)CAN_Send(CANC_ID_TELEMETRY_FAST, d, CAN_PAYLOAD_LEN);
}

static void Node_TxStatus(void)
{
    uint8_t d[CAN_PAYLOAD_LEN];

    d[0] = (uint8_t)MB_REG_GetHolding(MB_REG_HOLD_STATUS);
    d[1] = (uint8_t)MB_REG_GetHolding(MB_REG_HOLD_ERRCODE);
    d[2] = (uint8_t)MB_REG_GetHolding(MB_REG_HOLD_LINK_MASK);
    d[3] = (uint8_t)(MB_REG_GetHolding(MB_REG_HOLD_CONTRACT_HASH) & 0xFFU);
    d[4] = (uint8_t)CONTRACT_VERSION;
    d[5] = (uint8_t)FORM_MODE;
    d[6] = (uint8_t)(FW_VERSION_BCD & 0xFFU);
    d[7] = (uint8_t)(s_hb_seq & 0xFFU);
    (void)CAN_Send(CANC_ID_STATUS, d, CAN_PAYLOAD_LEN);
}

static void Node_TxCounters(void)
{
    uint8_t d[CAN_PAYLOAD_LEN];

    CANC_PutU16BE(&d[0], MB_REG_GetHolding(MB_REG_HOLD_RXCNT));
    CANC_PutU16BE(&d[2], MB_REG_GetHolding(MB_REG_HOLD_TXCNT));
    CANC_PutU16BE(&d[4], MB_REG_GetHolding(MB_REG_HOLD_CRCERR));
    CANC_PutU16BE(&d[6], MB_REG_GetHolding(MB_REG_HOLD_ADC1_RAW));
    (void)CAN_Send(CANC_ID_COUNTERS, d, CAN_PAYLOAD_LEN);
}

static void Node_TxEvent(void)
{
    uint8_t d[CAN_PAYLOAD_LEN];

    for (uint8_t i = 0U; i < CAN_PAYLOAD_LEN; i++)
    {
        d[i] = 0U;
    }
    d[0] = s_ev_ev;
    d[1] = s_ev_p0;
    d[2] = s_ev_p1;
    d[3] = s_ev_p2;
    d[4] = (uint8_t)(s_hb_seq & 0xFFU);
    d[7] = CANC_EVENT_SENTINEL;
    (void)CAN_Send(CANC_ID_EVENT, d, CAN_PAYLOAD_LEN);
}

static void Node_SnapBuildSend(void)
{
    uint8_t  p[CANC_SNAP_PAYLOAD_LEN];
    uint16_t crc;
    uint16_t i;

    p[0] = CANC_SNAP_VER;
    p[1] = CANC_SNAP_NODE;
    CANC_PutU16BE(&p[2], CANC_SNAP_REG_COUNT);
    for (i = 0U; i < CANC_SNAP_REG_COUNT; i++)
    {
        CANC_PutU16BE(&p[CANC_SNAP_HDR_LEN + 2U * i], MB_REG_GetHolding(i));
    }
    CANC_PutU16BE(&p[CANC_SNAP_HDR_LEN + 2U * CANC_SNAP_REG_COUNT], 0U);

    crc = MODBUS_CRC16(p, CANC_SNAP_CRC_SPAN);
    p[CANC_SNAP_CRC_SPAN]      = (uint8_t)(crc & 0xFFU);
    p[CANC_SNAP_CRC_SPAN + 1U] = (uint8_t)((crc >> 8) & 0xFFU);

    if (IsoTp_Send(&s_snap_tp, p, CANC_SNAP_PAYLOAD_LEN) == ISOTP_OK)
    {
        s_snap_send_pending = 0U;      /* else: retried by the next loop */
    }
}
#else  /* ================= GW ================= */
static void Gw_SnapProcess(const uint8_t *p, uint16_t len)
{
    uint16_t crc;
    uint16_t rx_crc;
    uint16_t i;

    if ((len != CANC_SNAP_PAYLOAD_LEN) ||
        (p[0] != CANC_SNAP_VER) ||
        (CANC_GetU16BE(&p[2]) != CANC_SNAP_REG_COUNT))
    {
        s_snap_err++;
        return;
    }

    rx_crc = (uint16_t)(p[CANC_SNAP_CRC_SPAN] |
                        ((uint16_t)p[CANC_SNAP_CRC_SPAN + 1U] << 8));
    crc = MODBUS_CRC16(p, CANC_SNAP_CRC_SPAN);
    if (crc != rx_crc)
    {
        s_snap_err++;
        (void)LOG_SVC_PostEvent(LOG_CRC_ERROR, 1U, 0U, 0U);   /* p0=1: snapshot */
        return;
    }

    /* mirror domain: 0x00-0x0F (minus the 0x0F hole) + 0x20-0x22.
     * 0x10-0x1F (board-local stats/meta) and 0x30+ (gw cmd shadow) stay local. */
    for (i = 0U; i < 0x10U; i++)
    {
        if (i != MB_REG_HOLD_HOLE)
        {
            MB_REG_SetHoldingRaw(i, CANC_GetU16BE(&p[CANC_SNAP_HDR_LEN + 2U * i]));
        }
    }
    for (i = MB_REG_HOLD_ADC1_RAW; i <= MB_REG_HOLD_VREF_MV; i++)
    {
        MB_REG_SetHoldingRaw(i, CANC_GetU16BE(&p[CANC_SNAP_HDR_LEN + 2U * i]));
    }

    /* full 56-register shadow: feeds the `snapshot` uplink topic (v0.5) -
     * the mirror domain above is a subset and stays the fast path. */
    for (i = 0U; i < CANC_SNAP_REG_COUNT; i++)
    {
        s_snap_regs[i] = CANC_GetU16BE(&p[CANC_SNAP_HDR_LEN + 2U * i]);
    }
    s_snap_regs_valid = 1U;

    /* A-05 pre-check: the snapshot must carry the same contract hash as the
     * gateway's own (same firmware, same dp_table). */
    if (CANC_GetU16BE(&p[CANC_SNAP_HDR_LEN + 2U * MB_REG_HOLD_CONTRACT_HASH]) !=
        MB_REG_GetHolding(MB_REG_HOLD_CONTRACT_HASH))
    {
        s_snap_hash_mismatch++;
    }

    s_snap_ok++;
    s_snap_last_ms = HAL_GetTick();
    s_snap_waiting = 0U;
}

static void Gw_SendCmdFrame(uint8_t id, uint16_t reg, uint16_t val)
{
    uint8_t d[CAN_PAYLOAD_LEN];

    d[CANC_CMD_B_FC]        = 0x06U;
    d[CANC_CMD_B_ID]        = id;
    CANC_PutU16BE(&d[CANC_CMD_B_ADDR_H], reg);
    CANC_PutU16BE(&d[CANC_CMD_B_VAL_H], val);
    d[CANC_CMD_B_SUBQTY]    = 1U;
    d[CANC_CMD_B_SENTINEL]  = CANC_CMD_SENTINEL;
    (void)CAN_Send(CANC_ID_CMD, d, CAN_PAYLOAD_LEN);
}

static void Gw_Loop(uint32_t now)
{
    /* heartbeat */
    if ((now - s_tx_hb_last) >= CANC_HB_PERIOD_MS)
    {
        s_tx_hb_last = now;
        Bridge_TxHb();
    }

    /* snapshot: 30 s tick or on-demand (console / node config event) */
    if ((now - s_tx_snap_last) >= CANC_SNAP_PERIOD_MS)
    {
        s_snap_now = 1U;
    }
    if ((s_snap_now != 0U) && (IsoTp_IsBusy(&s_snap_tp) == 0U) &&
        ((now - s_snap_sent_ms) >= CANC_SNAP_RETRY_MS))
    {
        uint8_t req[CANC_SNAPREQ_PAYLOAD_LEN];

        req[0] = CANC_SNAPREQ_SUBCMD;
        req[1] = ++s_snap_uid;
        if (IsoTp_Send(&s_snap_tp, req, CANC_SNAPREQ_PAYLOAD_LEN) == ISOTP_OK)
        {
            s_snap_now = 0U;
            s_snap_sent_ms = now;
            s_tx_snap_last = now;
            s_snap_waiting = 1U;
        }
    }
    if ((s_snap_waiting != 0U) && ((now - s_snap_sent_ms) > CANC_SNAP_TIMEOUT_MS))
    {
        s_snap_err++;
        s_snap_waiting = 0U;
    }

    /* CMD from the console: send, 500 ms timeout, one retry */
    if ((s_cmd_req != 0U) && (s_cmd_wait == 0U))
    {
        s_cmd_req = 0U;
        s_cmd_id++;
        Gw_SendCmdFrame(s_cmd_id, s_cmd_req_reg, s_cmd_req_val);
        s_cmd_wait = 1U;
        s_cmd_retry = 0U;
        s_cmd_sent_ms = now;
        MB_REG_SetHoldingRaw(MB_REG_HOLD_CMD_WRITE_REG, s_cmd_req_reg);
        MB_REG_SetHoldingRaw(MB_REG_HOLD_CMD_WRITE_VAL, s_cmd_req_val);
        MB_REG_SetHoldingRaw(MB_REG_HOLD_CMD_ID, s_cmd_id);
    }
    if ((s_cmd_wait != 0U) && ((now - s_cmd_sent_ms) > CANC_CMD_TIMEOUT_MS))
    {
        if (s_cmd_retry == 0U)
        {
            s_cmd_retry = 1U;
            Gw_SendCmdFrame(s_cmd_id, s_cmd_req_reg, s_cmd_req_val);
            s_cmd_sent_ms = now;
            BSP_Puts("cmd: no ack, retrying once\r\n");
        }
        else
        {
            s_cmd_wait = 0U;
            s_cmd_result = 0U;
            BSP_Puts("cmd: FAILED (no ack after retry)\r\n");
            (void)LOG_SVC_PostEvent(LOG_TIMEOUT, 3U, 0U, 0U);   /* p0=3: cmd */
        }
    }
}
#endif /* ROLE_IS_GW */

/*====================================================================*/
/* Common periodic work                                                */
/*====================================================================*/
static void Bridge_UpdateStats(void)
{
    const CAN_Stats *cs = CAN_GetStats();
    comm_stats_t ms;

#if ROLE_IS_GW
    MB_Master_GetStats(&ms);
#else
    MB_Slave_GetStats(&ms);
#endif

    /* registers are u16 - long-run counters wrap (u16 环绕); the console
     * 'stats' command shows the full u32 values for acceptance work. */
    MB_REG_SetHoldingRaw(MB_REG_HOLD_CAN_TX_ACK, (uint16_t)(cs->txAckCount & 0xFFFFU));
    MB_REG_SetHoldingRaw(MB_REG_HOLD_CAN_RX_OVR, (uint16_t)(cs->rxOverruns & 0xFFFFU));
    MB_REG_SetHoldingRaw(MB_REG_HOLD_CAN_BUSOFF, (uint16_t)(cs->busOffCount & 0xFFFFU));
    MB_REG_SetHoldingRaw(MB_REG_HOLD_CAN_TX_LAT, (uint16_t)(cs->txMaxLatencyMs & 0xFFFFU));
    MB_REG_SetHoldingRaw(MB_REG_HOLD_MB_TIMEOUT, (uint16_t)(ms.timeout_count & 0xFFFFU));
    MB_REG_SetHoldingRaw(MB_REG_HOLD_MB_RETRY, (uint16_t)(ms.retry_count & 0xFFFFU));
    MB_REG_SetHoldingRaw(MB_REG_HOLD_MB_EXCEPTION, (uint16_t)(ms.exception_count & 0xFFFFU));

    MB_REG_SetHoldingRaw(MB_REG_HOLD_UPTIME_S, (uint16_t)((HAL_GetTick() / 1000U) & 0xFFFFU));
    MB_REG_SetHoldingRaw(MB_REG_HOLD_FREE_HEAP,
                         (uint16_t)((uint32_t)xPortGetFreeHeapSize() & 0xFFFFU));
    MB_REG_SetHoldingRaw(MB_REG_HOLD_TASK_OVR, (uint16_t)(App_GetTaskOvr() & 0xFFFFU));
}

static void Bridge_UpdateLink(uint32_t now)
{
    uint8_t mask = 0U;
    uint8_t peer_now;

    peer_now = (uint8_t)((s_peer_hb_ms != 0U) &&
                         ((now - s_peer_hb_ms) <= CAN_LINK_TIMEOUT_MS));
    if (peer_now != s_peer_up)
    {
        s_peer_up = peer_now;
        (void)LOG_SVC_PostEvent(peer_now ? (uint8_t)LOG_LINK_UP
                                         : (uint8_t)LOG_LINK_DOWN, 0U, 0U, 0U);
        /* link edges also go out on the uplink event topic (both directions:
         * the down edge is local-only at the node, but the gateway saw it) */
        Uplink_NotifyEvent(peer_now ? (uint8_t)LOG_LINK_UP
                                    : (uint8_t)LOG_LINK_DOWN, 0U, 0U, 0U);
    }

#if ROLE_IS_GW
    if (MB_Master_GetConsecutiveFails() < 3U)
    {
        mask |= CANC_LINK_BIT_RS485;
    }
#else
    if (MB_REG_GetHolding(MB_REG_HOLD_STATUS) == 1U)
    {
        mask |= CANC_LINK_BIT_RS485;
    }
#endif
    if (s_peer_up != 0U)
    {
        mask |= CANC_LINK_BIT_CAN;
    }
    /* bit2 WiFi / bit3 MQTT come from the network layer (0 in M1: the
     * shadow gateway at the PC owns the broker connection, not the board) */
    mask |= Mqtt_LinkBits();

    MB_REG_SetHoldingRaw(MB_REG_HOLD_LINK_MASK, mask);
}

/*====================================================================*/
/* Hooks                                                               */
/*====================================================================*/
void Bridge_NotifyEvent(uint8_t ev, uint8_t p0, uint8_t p1, uint8_t p2)
{
#if !ROLE_IS_GW
    /* 0x201 is the alarm/exception channel (docs/03 §4.3): routine per-frame
     * RX_OK/TX_OK events stay in the local flash log only - forwarding them
     * would triple the gateway log for zero information. */
    if ((ev == (uint8_t)LOG_RX_OK) || (ev == (uint8_t)LOG_TX_OK))
    {
        return;
    }
    s_ev_ev = ev;
    s_ev_p0 = p0;
    s_ev_p1 = p1;
    s_ev_p2 = p2;
    s_ev_pending = 1U;
#else
    (void)ev; (void)p0; (void)p1; (void)p2;
#endif
}

uint8_t Bridge_RequestCmd(uint16_t reg, uint16_t val)
{
#if ROLE_IS_GW
    if ((s_cmd_req != 0U) || (s_cmd_wait != 0U))
    {
        return 0U;
    }
    s_cmd_req_reg = reg;
    s_cmd_req_val = val;
    s_cmd_req = 1U;
    return 1U;
#else
    (void)reg; (void)val;
    return 0U;
#endif
}

void Bridge_RequestSnapshot(void)
{
#if ROLE_IS_GW
    s_snap_now = 1U;
#endif
}

uint32_t Bridge_GetHbAgeMs(void)
{
    if (s_peer_hb_ms == 0U)
    {
        return 0xFFFFFFFFU;                     /* never seen */
    }
    return HAL_GetTick() - s_peer_hb_ms;
}

uint8_t Bridge_PeerUp(void)
{
    return s_peer_up;
}

uint32_t Bridge_GetSnapOkCount(void)
{
#if ROLE_IS_GW
    return s_snap_ok;
#else
    return 0U;
#endif
}

uint32_t Bridge_GetSnapErrCount(void)
{
#if ROLE_IS_GW
    return s_snap_err;
#else
    return 0U;
#endif
}

uint32_t Bridge_GetSnapAgeMs(void)
{
#if ROLE_IS_GW
    if (s_snap_last_ms == 0U)
    {
        return 0xFFFFFFFFU;
    }
    return HAL_GetTick() - s_snap_last_ms;
#else
    return 0U;
#endif
}

uint8_t Bridge_GetLastCmdResult(void)
{
#if ROLE_IS_GW
    return s_cmd_result;
#else
    return 0U;
#endif
}

uint8_t Bridge_GetLastCmdExc(void)
{
#if ROLE_IS_GW
    return s_cmd_exc;
#else
    return 0U;
#endif
}

uint8_t Bridge_GetPeerSeqLo(void)
{
    return s_peer_seq_lo;
}

uint16_t Bridge_GetSnapReg(uint16_t pdu)
{
#if ROLE_IS_GW
    if ((pdu < CANC_SNAP_REG_COUNT) && (s_snap_regs_valid != 0U))
    {
        return s_snap_regs[pdu];
    }
#else
    (void)pdu;
#endif
    return 0U;
}

uint8_t Bridge_SnapValid(void)
{
#if ROLE_IS_GW
    return s_snap_regs_valid;
#else
    return 0U;
#endif
}

uint32_t Bridge_GetSnapHashMismatch(void)
{
#if ROLE_IS_GW
    return s_snap_hash_mismatch;
#else
    return 0U;
#endif
}

/*====================================================================*/
/* T3 body                                                             */
/*====================================================================*/
void Bridge_TaskMain(void)
{
#if ROLE_IS_GW
    IsoTp_Init(&s_snap_tp, CANC_ID_SNAPSHOT_DATA, CANC_ID_SNAPSHOT_REQ,
               Snap_RxCallback, Snap_ErrCallback);
    s_tx_hb_last = HAL_GetTick() - CANC_HB_PERIOD_MS;   /* first HB now */
    s_tx_snap_last = HAL_GetTick();
    s_cmd_id = 0U;
#else
    IsoTp_Init(&s_snap_tp, CANC_ID_SNAPSHOT_REQ, CANC_ID_SNAPSHOT_DATA,
               Snap_RxCallback, Snap_ErrCallback);
    s_tx_tele_last = s_tx_status_last = s_tx_cntrs_last = HAL_GetTick();
    /* v0.4->HW bring-up 2026-10-11 (docs §18.6): the node also sends its
     * periodic heartbeat - this initialisation was missing on the node
     * path, so Bridge_TxHb (0x501) was never called there and the gateway
     * never saw its peer (peer=0 / LINK bit stayed dark). */
    s_tx_hb_last = HAL_GetTick() - CANC_HB_PERIOD_MS;   /* first HB now */
#endif

    s_snap_q = xQueueCreate(SNAP_RX_Q_LEN, sizeof(CAN_RxMsg));
    s_peer_hb_ms = 0U;

    for (;;)
    {
        CAN_RxMsg m;
        uint32_t  now = HAL_GetTick();

        /* hand over queued snapshot-related frames to this task's ctx */
        while ((s_snap_q != 0) && (xQueueReceive(s_snap_q, &m, 0U) == pdTRUE))
        {
            (void)IsoTp_OnCanFrame(&s_snap_tp, m.id, m.data, m.len);
        }

        Bridge_UpdateStats();
        Bridge_UpdateLink(now);

#if ROLE_IS_GW
        Gw_Loop(now);
#else
        /* heartbeat first (same 1 s cadence as the gateway side) */
        if ((now - s_tx_hb_last) >= CANC_HB_PERIOD_MS)
        {
            s_tx_hb_last = now;
            Bridge_TxHb();
        }
        if ((now - s_tx_tele_last) >= CANC_TELE_PERIOD_MS)
        {
            s_tx_tele_last = now;
            Node_TxTelemetry();
        }
        if ((now - s_tx_status_last) >= CANC_STATUS_PERIOD_MS)
        {
            s_tx_status_last = now;
            Node_TxStatus();
        }
        if ((now - s_tx_cntrs_last) >= CANC_CNTRS_PERIOD_MS)
        {
            s_tx_cntrs_last = now;
            Node_TxCounters();
        }
        if (s_ev_pending != 0U)
        {
            s_ev_pending = 0U;
            Node_TxEvent();
        }
        if (s_snap_send_pending != 0U)
        {
            Node_SnapBuildSend();
        }
#endif

        IsoTp_Task(&s_snap_tp);

        App_Hb_Beat(APP_HB_BRIDGE);
        vTaskDelay(pdMS_TO_TICKS(BRIDGE_PERIOD_MS));
    }
}
