/**
  ******************************************************************************
  * @file    uplink_port.c
  * @brief   Uplink task (T4) - M1: JSON payloads as 0xAA55 frames over USART1;
  *          M2: the same payloads will be published by MQTT-C (v0.6).
  *
  *  The JSON builders below are the SINGLE source for both forms: M1 ships
  *  them over the wire verbatim, M2 will hand the identical bytes to the
  *  MQTT client.  Every scaled value goes through fmt_fixed (docs/03 §5.2).
  ******************************************************************************
  */
#include "uplink_port.h"
#include "app_board_cfg.h"
#include "app_rtos.h"
#include "bridge.h"
#include "bsp_console.h"
#include "bsp_printf.h"
#include "can_contract.h"
#include "can_drv.h"
#include "dp_table.h"
#include "fmt.h"
#include "log_service.h"
#include "modbus_crc.h"
#include "modbus_register.h"
#include "types.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include <stdio.h>

#if ROLE_IS_GW

/*====================================================================*/
/* Wire protocol (frozen: Tools/mqtt/mqtt_topics.md)                    */
/*====================================================================*/
#define UPL_MAGIC0            0xAAU
#define UPL_MAGIC1            0x55U
#define UPL_HDR_LEN           7U     /* magic2 + type1 + seq2 + len2    */
#define UPL_CRC_LEN           2U
#define UPL_MAX_PAYLOAD       1536U  /* meta measured 1405 B (v0.5)     */

#define UPL_TYPE_TELEMETRY    1U
#define UPL_TYPE_STATUS       2U
#define UPL_TYPE_COUNTERS     3U
#define UPL_TYPE_META         4U
#define UPL_TYPE_EVENT        5U
#define UPL_TYPE_SNAPSHOT     6U

static uint8_t  s_frame[UPL_MAX_PAYLOAD + UPL_HDR_LEN + UPL_CRC_LEN];
static uint16_t s_frame_seq;
static volatile uint32_t s_frames;
static volatile uint32_t s_drops;

/* event queue: packed u32 = ev<<24 | p0<<16 | p1<<8 | p2 (multi-producer:
 * can_task forwards node events, bridge task emits link edges) */
static QueueHandle_t s_evq;

/*====================================================================*/
/* Small builder helpers                                               */
/*====================================================================*/
static char *Up_Pay(void)
{
    return (char *)&s_frame[UPL_HDR_LEN];
}

static int Up_AppendFmt(char *b, int n, uint16_t cap, const char *key,
                        uint16_t raw, uint8_t scale)
{
    if ((uint32_t)(n + 32) > cap)
    {
        return n;
    }
    n += snprintf(&b[n], (size_t)(cap - (uint16_t)n), ",\"%s\":", key);
    if ((uint32_t)n + 16U > cap)
    {
        return n;
    }
    n += fmt_fixed(&b[n], (int32_t)raw, scale);
    return n;
}

static int Up_AppendU(char *b, int n, uint16_t cap, const char *key, uint32_t v)
{
    if ((uint32_t)(n + 48) > cap)
    {
        return n;
    }
    n += snprintf(&b[n], (size_t)(cap - (uint16_t)n), ",\"%s\":%lu",
                  key, (unsigned long)v);
    return n;
}

static void Up_SendFrame(uint8_t type, uint16_t len)
{
    uint16_t crc;

    s_frame[0] = UPL_MAGIC0;
    s_frame[1] = UPL_MAGIC1;
    s_frame[2] = type;
    s_frame[3] = (uint8_t)(s_frame_seq >> 8);
    s_frame[4] = (uint8_t)(s_frame_seq & 0xFFU);
    s_frame[5] = (uint8_t)(len >> 8);
    s_frame[6] = (uint8_t)(len & 0xFFU);
    crc = MODBUS_CRC16(&s_frame[2], (uint16_t)(5U + len));
    s_frame[UPL_HDR_LEN + len]      = (uint8_t)(crc & 0xFFU);
    s_frame[UPL_HDR_LEN + len + 1U] = (uint8_t)((crc >> 8) & 0xFFU);
    s_frame_seq++;

#if (FORM_MODE == FORM_M1)
    /* shared USART1 with the text console: take the TX mutex so a console
     * line can never interleave inside a frame (docs/02 decision 4). */
    BSP_TxLock();
    BSP_Console_Send(s_frame, (uint16_t)(UPL_HDR_LEN + len + UPL_CRC_LEN));
    BSP_TxUnlock();
    s_frames++;
#else
    /* M2: the MQTT-C publish path arrives in v0.6; counted as dropped so the
     * counters never lie about "uplinked". */
    (void)type;
    s_drops++;
#endif
}

/*====================================================================*/
/* JSON builders (docs/03 §5.1 payloads)                               */
/*====================================================================*/
static void Up_TxTelemetry(void)
{
    char *b = Up_Pay();
    int n;

    n = snprintf(b, UPL_MAX_PAYLOAD, "{\"seq\":%u,\"ts\":%lu",
                 (unsigned)Bridge_GetPeerSeqLo(),
                 (unsigned long)(HAL_GetTick() / 1000U));
    n = Up_AppendFmt(b, n, UPL_MAX_PAYLOAD, "temp", MB_REG_GetHolding(MB_REG_HOLD_TEMP), 1U);
    n = Up_AppendFmt(b, n, UPL_MAX_PAYLOAD, "humi", MB_REG_GetHolding(MB_REG_HOLD_HUMI), 1U);
    n = Up_AppendFmt(b, n, UPL_MAX_PAYLOAD, "volt", MB_REG_GetHolding(MB_REG_HOLD_VOLT), 2U);
    n = Up_AppendFmt(b, n, UPL_MAX_PAYLOAD, "curr", MB_REG_GetHolding(MB_REG_HOLD_CURR), 2U);
    n = Up_AppendU(b, n, UPL_MAX_PAYLOAD, "adc1", MB_REG_GetHolding(MB_REG_HOLD_ADC1_RAW));
    n = Up_AppendFmt(b, n, UPL_MAX_PAYLOAD, "mcu_t", MB_REG_GetHolding(MB_REG_HOLD_MCU_TEMP), 1U);
    n = Up_AppendU(b, n, UPL_MAX_PAYLOAD, "vref", MB_REG_GetHolding(MB_REG_HOLD_VREF_MV));
    if ((uint32_t)n + 4U <= UPL_MAX_PAYLOAD)
    {
        b[n] = '}';
        n++;
        b[n] = '\0';
    }
    Up_SendFrame(UPL_TYPE_TELEMETRY, (uint16_t)n);
}

static void Up_TxStatus(void)
{
    char *b = Up_Pay();
    int  n;

    n = snprintf(b, UPL_MAX_PAYLOAD,
                 "{\"st\":%u,\"er\":%u,\"lk\":%u,\"ch\":\"%04lX\",\"cv\":%u,"
                 "\"ts\":%lu,\"heap\":%u}",
                 (unsigned)MB_REG_GetHolding(MB_REG_HOLD_STATUS),
                 (unsigned)MB_REG_GetHolding(MB_REG_HOLD_ERRCODE),
                 (unsigned)MB_REG_GetHolding(MB_REG_HOLD_LINK_MASK),
                 (unsigned long)MB_REG_GetHolding(MB_REG_HOLD_CONTRACT_HASH),
                 (unsigned)MB_REG_GetHolding(MB_REG_HOLD_CONTRACT_VER),
                 (unsigned long)(HAL_GetTick() / 1000U),
                 (unsigned)MB_REG_GetHolding(MB_REG_HOLD_FREE_HEAP));
    Up_SendFrame(UPL_TYPE_STATUS, (uint16_t)n);
}

static void Up_TxCounters(void)
{
    char *b = Up_Pay();
    int  n;

    n = snprintf(b, UPL_MAX_PAYLOAD,
                 "{\"rx\":%u,\"tx\":%u,\"crc\":%u,"
                 "\"can_ack\":%u,\"can_ovr\":%u,\"can_bo\":%u,\"can_lat\":%u,"
                 "\"mb_to\":%u,\"mb_rt\":%u,\"mb_exc\":%u}",
                 (unsigned)MB_REG_GetHolding(MB_REG_HOLD_RXCNT),
                 (unsigned)MB_REG_GetHolding(MB_REG_HOLD_TXCNT),
                 (unsigned)MB_REG_GetHolding(MB_REG_HOLD_CRCERR),
                 (unsigned)MB_REG_GetHolding(MB_REG_HOLD_CAN_TX_ACK),
                 (unsigned)MB_REG_GetHolding(MB_REG_HOLD_CAN_RX_OVR),
                 (unsigned)MB_REG_GetHolding(MB_REG_HOLD_CAN_BUSOFF),
                 (unsigned)MB_REG_GetHolding(MB_REG_HOLD_CAN_TX_LAT),
                 (unsigned)MB_REG_GetHolding(MB_REG_HOLD_MB_TIMEOUT),
                 (unsigned)MB_REG_GetHolding(MB_REG_HOLD_MB_RETRY),
                 (unsigned)MB_REG_GetHolding(MB_REG_HOLD_MB_EXCEPTION));
    Up_SendFrame(UPL_TYPE_COUNTERS, (uint16_t)n);
}

static void Up_TxMeta(void)
{
    char *b = Up_Pay();
    int   n;
    int   i;
    uint16_t fw = FW_VERSION_BCD;

    n = snprintf(b, UPL_MAX_PAYLOAD,
                 "{\"fw\":\"%u.%u.%u\",\"cv\":%u,\"ch\":\"%04lX\","
                 "\"role\":\"%s\",\"form\":\"%s\",\"can\":\"%luk\","
                 "\"mb_baud\":%lu,\"mb_sid\":%u,\"dp\":[",
                 (unsigned)((fw >> 8) & 0xFU),
                 (unsigned)((fw >> 4) & 0xFU),
                 (unsigned)(fw & 0xFU),
                 (unsigned)CONTRACT_VERSION,
                 (unsigned long)MB_REG_GetHolding(MB_REG_HOLD_CONTRACT_HASH),
                 ROLE_IS_GW ? "GW" : "node",
                 (FORM_MODE == FORM_M1) ? "M1" : "M2",
                 (unsigned long)(CAN_GetBitrate() / 1000U),
                 (unsigned long)MB_REG_BaudFromIdx((uint8_t)MB_REG_GetBaudIdx()),
                 (unsigned)MB_REG_GetSlaveId());

    for (i = 0; i < (int)g_dp_table_len; i++)
    {
        const dp_entry_t *e = &g_dp_table[i];

        if ((uint32_t)n + 48U > UPL_MAX_PAYLOAD)
        {
            break;
        }
        n += snprintf(&b[n], (size_t)(UPL_MAX_PAYLOAD - (uint16_t)n),
                      "%s[\"%s\",%u,%u,\"%s\"]",
                      (i != 0) ? "," : "", e->key,
                      (unsigned)e->pdu, (unsigned)e->scale,
                      (e->rw == DP_RW_RW) ? "RW" : "RO");
    }

    if ((uint32_t)n + 4U <= UPL_MAX_PAYLOAD)
    {
        b[n] = ']';
        n++;
        b[n] = '}';
        n++;
        b[n] = '\0';
    }
    Up_SendFrame(UPL_TYPE_META, (uint16_t)n);
}

static void Up_TxSnapshot(void)
{
    char *b = Up_Pay();
    int   n;
    int   i;

    if (Bridge_SnapValid() == 0U)
    {
        return;                     /* nothing received from the node yet */
    }

    n = snprintf(b, UPL_MAX_PAYLOAD, "{\"cv\":%u,\"ch\":\"%04lX\",\"regs\":{",
                 (unsigned)CONTRACT_VERSION,
                 (unsigned long)MB_REG_GetHolding(MB_REG_HOLD_CONTRACT_HASH));

    for (i = 0; i < (int)CANC_SNAP_REG_COUNT; i++)
    {
        if ((uint32_t)n + 32U > UPL_MAX_PAYLOAD)
        {
            break;
        }
        n += snprintf(&b[n], (size_t)(UPL_MAX_PAYLOAD - (uint16_t)n),
                      "%s\"%u\":%u",
                      (i != 0) ? "," : "",
                      40001U + (unsigned)i,
                      (unsigned)Bridge_GetSnapReg((uint16_t)i));
    }

    if ((uint32_t)n + 8U <= UPL_MAX_PAYLOAD)
    {
        b[n] = '}';
        n++;
        b[n] = '}';
        n++;
        b[n] = '\0';
    }
    Up_SendFrame(UPL_TYPE_SNAPSHOT, (uint16_t)n);
}

static void Up_TxEvent(uint8_t ev, uint8_t p0, uint8_t p1, uint8_t p2)
{
    char *b = Up_Pay();
    int   n;

    n = snprintf(b, UPL_MAX_PAYLOAD,
                 "{\"ev\":%u,\"p\":[%u,%u,%u],\"seq\":%u,\"ts\":%lu}",
                 (unsigned)ev, (unsigned)p0, (unsigned)p1, (unsigned)p2,
                 (unsigned)(s_frame_seq & 0xFFU),
                 (unsigned long)(HAL_GetTick() / 1000U));
    Up_SendFrame(UPL_TYPE_EVENT, (uint16_t)n);
}

/*====================================================================*/
/* Public API                                                          */
/*====================================================================*/
void Uplink_Init(void)
{
    s_evq = xQueueCreate(8U, sizeof(uint32_t));
}

void Uplink_NotifyEvent(uint8_t ev, uint8_t p0, uint8_t p1, uint8_t p2)
{
    uint32_t packed = ((uint32_t)ev << 24) | ((uint32_t)p0 << 16) |
                      ((uint32_t)p1 << 8) | (uint32_t)p2;

    /* routine per-frame events never reach the uplink (same filter as the
     * CAN event channel, docs/03 §4.3 alarm semantics) */
    if ((ev == (uint8_t)LOG_RX_OK) || (ev == (uint8_t)LOG_TX_OK))
    {
        return;
    }
    if ((s_evq == 0) || (xQueueSend(s_evq, &packed, 0U) != pdTRUE))
    {
        s_drops++;
    }
}

uint32_t Uplink_GetFrameCount(void)
{
    return s_frames;
}

uint32_t Uplink_GetDropCount(void)
{
    return s_drops;
}

/*====================================================================*/
/* T4 body (gateway)                                                   */
/*====================================================================*/
void Uplink_TaskMain(void)
{
    uint32_t t_tele  = HAL_GetTick();
    uint32_t t_stat  = t_tele;
    uint32_t t_cntr  = t_tele;
    uint32_t t_snap  = t_tele;
    uint8_t  meta_due = 1U;

    for (;;)
    {
        uint32_t now = HAL_GetTick();
        uint32_t packed;

#if (FORM_MODE == FORM_M1)
        if (meta_due != 0U)
        {
            meta_due = 0U;
            Up_TxMeta();                      /* boot + on node cfg change */
        }
        if ((now - t_tele) >= UPLINK_TELEMETRY_MS)
        {
            t_tele = now;
            Up_TxTelemetry();
        }
        if ((now - t_stat) >= UPLINK_STATUS_MS)
        {
            t_stat = now;
            Up_TxStatus();
        }
        if ((now - t_cntr) >= UPLINK_COUNTERS_MS)
        {
            t_cntr = now;
            Up_TxCounters();
        }
        if ((now - t_snap) >= UPLINK_SNAPSHOT_MS)
        {
            t_snap = now;
            Up_TxSnapshot();
        }
        while ((s_evq != 0) && (xQueueReceive(s_evq, &packed, 0U) == pdTRUE))
        {
            uint8_t ev = (uint8_t)(packed >> 24);
            Up_TxEvent((uint8_t)(packed >> 24),
                       (uint8_t)((packed >> 16) & 0xFFU),
                       (uint8_t)((packed >> 8) & 0xFFU),
                       (uint8_t)(packed & 0xFFU));
            if (ev == (uint8_t)LOG_CONFIG_CHANGE)
            {
                meta_due = 1U;                /* target sid/baud may differ */
            }
        }
#else
        /* M2: MQTT-C publish path (v0.6).  Until then, drain the event
         * queue so it cannot overflow silently. */
        while ((s_evq != 0) && (xQueueReceive(s_evq, &packed, 0U) == pdTRUE))
        {
            s_drops++;
        }
        (void)meta_due;
#endif

        App_Hb_Beat(APP_HB_UPLINK);
        vTaskDelay(pdMS_TO_TICKS(100));       /* 100 ms tick (docs/02 T4) */
    }
}

#else  /* ================= NODE: uplink is gateway-only in v1 ============ */

void Uplink_Init(void) { }

void Uplink_TaskMain(void) { }

void Uplink_NotifyEvent(uint8_t ev, uint8_t p0, uint8_t p1, uint8_t p2)
{
    (void)ev; (void)p0; (void)p1; (void)p2;
}

uint32_t Uplink_GetFrameCount(void) { return 0U; }
uint32_t Uplink_GetDropCount(void)  { return 0U; }

#endif /* ROLE_IS_GW */
