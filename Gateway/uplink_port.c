/**
  ******************************************************************************
  * @file    uplink_port.c
  * @brief   Uplink task (T4) - M1: JSON payloads as 0xAA55 frames over USART1;
  *          M2: the same payloads published by MQTT-C over ESP-AT (v0.6).
  *
  *  The JSON builders are the SINGLE source for both forms.  Every scaled
  *  value goes through fmt_fixed (docs/03 §5.2).  On M2, when the broker
  *  session is down, telemetry/status/counters are cached to the pending log
  *  (compact fields) and replayed by Network/backfill.c.
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
#include "uplink_fmt.h"

#if (FORM_MODE == FORM_M2)
#include "backfill.h"
#include "esp_at.h"
#include "mqtt_client.h"
#endif

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include <stdio.h>

#if ROLE_IS_GW

/*====================================================================*/
/* Wire protocol constants (frozen: Tools/mqtt/mqtt_topics.md)          */
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

/* M2 topic table (uplink topic mapping; single node n1 in v1) */
#define UPL_TOPIC_TELEMETRY   MQTT_TOPIC_PREFIX "/n1/telemetry"
#define UPL_TOPIC_STATUS      MQTT_TOPIC_PREFIX "/n1/status"
#define UPL_TOPIC_COUNTERS    MQTT_TOPIC_PREFIX "/n1/counters"
#define UPL_TOPIC_META        MQTT_TOPIC_PREFIX "/meta"
#define UPL_TOPIC_EVENT       MQTT_TOPIC_PREFIX "/n1/event"
#define UPL_TOPIC_SNAPSHOT    MQTT_TOPIC_PREFIX "/n1/snapshot"

static uint8_t  s_frame[UPL_MAX_PAYLOAD + UPL_HDR_LEN + UPL_CRC_LEN];
static uint16_t s_frame_seq;
static volatile uint32_t s_frames;
static volatile uint32_t s_drops;

/* event queue: packed u32 = ev<<24 | p0<<16 | p1<<8 | p2 (multi-producer) */
static QueueHandle_t s_evq;

/*====================================================================*/
/* Topic / QoS mapping                                                 */
/*====================================================================*/
const char *Uplink_TopicForType(uint8_t type)
{
    switch (type)
    {
        case UPL_TYPE_TELEMETRY: return UPL_TOPIC_TELEMETRY;
        case UPL_TYPE_STATUS:    return UPL_TOPIC_STATUS;
        case UPL_TYPE_COUNTERS:  return UPL_TOPIC_COUNTERS;
        case UPL_TYPE_META:      return UPL_TOPIC_META;
        case UPL_TYPE_EVENT:     return UPL_TOPIC_EVENT;
        case UPL_TYPE_SNAPSHOT:  return UPL_TOPIC_SNAPSHOT;
        default:                 return 0;
    }
}

void Uplink_GetQosRetain(uint8_t type, uint8_t *qos, uint8_t *retain)
{
    /* frozen table: telemetry/counters QoS0 no-retain; status/meta QoS1
     * retain; snapshot/event QoS1 no-retain */
    switch (type)
    {
        case UPL_TYPE_STATUS:
        case UPL_TYPE_META:
            *qos = 1U; *retain = 1U;
            break;
        case UPL_TYPE_TELEMETRY:
        case UPL_TYPE_COUNTERS:
            *qos = 0U; *retain = 0U;
            break;
        default:
            *qos = 1U; *retain = 0U;
            break;
    }
}

/*====================================================================*/
/* Emit: M1 frames / M2 publish (+ offline cache)                      */
/*====================================================================*/
static char *Up_Pay(void)
{
    return (char *)&s_frame[UPL_HDR_LEN];
}

#if (FORM_MODE == FORM_M1)
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

    /* shared USART1 with the text console: take the TX mutex so a console
     * line can never interleave inside a frame (docs/02 decision 4). */
    BSP_TxLock();
    BSP_Console_Send(s_frame, (uint16_t)(UPL_HDR_LEN + len + UPL_CRC_LEN));
    BSP_TxUnlock();
    s_frames++;
}
#endif

/**
  * @brief  Hand one built payload to the transport.
  * @param  cache_fields  compact field block for the offline cache (types
  *         1-3 only; NULL = not cacheable)
  */
static void Up_Emit(uint8_t type, uint16_t len, uint8_t seq_lo,
                    const uint8_t *cache_fields)
{
#if (FORM_MODE == FORM_M1)
    (void)cache_fields;
    (void)seq_lo;
    Up_SendFrame(type, len);
#else
    uint8_t qos;
    uint8_t retain;
    const char *topic = Uplink_TopicForType(type);

    Uplink_GetQosRetain(type, &qos, &retain);
    if ((topic != 0) &&
        (Mqtt_PublishRaw(topic, (const uint8_t *)Up_Pay(), len, qos, retain) != 0U))
    {
        s_frames++;
        return;
    }

    /* session down (or send buffer full): cache what we can replay */
    if (cache_fields != 0)
    {
        (void)LOG_SVC_PostPending(type, seq_lo, cache_fields);
    }
    s_drops++;
#endif
}

/*====================================================================*/
/* Payload builders                                                    */
/*====================================================================*/
static void PutU16BE(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFU);
}

static void Up_TxTelemetry(void)
{
    uint8_t f[8];

    PutU16BE(&f[0], MB_REG_GetHolding(MB_REG_HOLD_TEMP));
    PutU16BE(&f[2], MB_REG_GetHolding(MB_REG_HOLD_HUMI));
    PutU16BE(&f[4], MB_REG_GetHolding(MB_REG_HOLD_VOLT));
    PutU16BE(&f[6], MB_REG_GetHolding(MB_REG_HOLD_CURR));

    /* NOTE: the live payload appends adc1/mcu_t/vref after curr; the cache
     * only holds the four measured values (docs/03 §5.3 pace calculation).
     * The live text is built in one shot below to stay byte-identical with
     * the contract example ordering. */
    {
        char *b = Up_Pay();
        uint8_t seq_lo = Bridge_GetPeerSeqLo();
        int   n = (int)Uplink_FormatTelemetry(b, UPL_MAX_PAYLOAD, seq_lo,
                                              HAL_GetTick() / 1000U, f);

        /* append the extended measurements (live-only fields) */
        n += snprintf(&b[n], (size_t)(UPL_MAX_PAYLOAD - (uint16_t)n),
                      ",\"adc1\":%u,\"mcu_t\":",
                      (unsigned)MB_REG_GetHolding(MB_REG_HOLD_ADC1_RAW));
        n += fmt_fixed(&b[n], (int32_t)MB_REG_GetHolding(MB_REG_HOLD_MCU_TEMP), 1U);
        n += snprintf(&b[n], (size_t)(UPL_MAX_PAYLOAD - (uint16_t)n),
                      ",\"vref\":%u}",
                      (unsigned)MB_REG_GetHolding(MB_REG_HOLD_VREF_MV));

        Up_Emit(UPL_TYPE_TELEMETRY, (uint16_t)n, seq_lo, f);
    }
}

static void Up_TxStatus(void)
{
    uint8_t f[8];
    char   *b = Up_Pay();
    uint16_t n;

    f[0] = (uint8_t)MB_REG_GetHolding(MB_REG_HOLD_STATUS);
    f[1] = (uint8_t)MB_REG_GetHolding(MB_REG_HOLD_ERRCODE);
    f[2] = (uint8_t)MB_REG_GetHolding(MB_REG_HOLD_LINK_MASK);
    PutU16BE(&f[3], MB_REG_GetHolding(MB_REG_HOLD_CONTRACT_HASH));
    f[5] = (uint8_t)MB_REG_GetHolding(MB_REG_HOLD_CONTRACT_VER);
    PutU16BE(&f[6], MB_REG_GetHolding(MB_REG_HOLD_FREE_HEAP));

    n = Uplink_FormatStatus(b, UPL_MAX_PAYLOAD, HAL_GetTick() / 1000U, f);
    Up_Emit(UPL_TYPE_STATUS, n, 0U, f);
}

static void Up_TxCounters(void)
{
    uint8_t f[20];

    PutU16BE(&f[0],  MB_REG_GetHolding(MB_REG_HOLD_RXCNT));
    PutU16BE(&f[2],  MB_REG_GetHolding(MB_REG_HOLD_TXCNT));
    PutU16BE(&f[4],  MB_REG_GetHolding(MB_REG_HOLD_CRCERR));
    PutU16BE(&f[6],  MB_REG_GetHolding(MB_REG_HOLD_CAN_TX_ACK));
    PutU16BE(&f[8],  MB_REG_GetHolding(MB_REG_HOLD_CAN_RX_OVR));
    PutU16BE(&f[10], MB_REG_GetHolding(MB_REG_HOLD_CAN_BUSOFF));
    PutU16BE(&f[12], MB_REG_GetHolding(MB_REG_HOLD_CAN_TX_LAT));
    PutU16BE(&f[14], MB_REG_GetHolding(MB_REG_HOLD_MB_TIMEOUT));
    PutU16BE(&f[16], MB_REG_GetHolding(MB_REG_HOLD_MB_RETRY));
    PutU16BE(&f[18], MB_REG_GetHolding(MB_REG_HOLD_MB_EXCEPTION));

    {
        uint16_t n = Uplink_FormatCounters(Up_Pay(), UPL_MAX_PAYLOAD, f);
        Up_Emit(UPL_TYPE_COUNTERS, n, 0U, f);
    }
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
    Up_Emit(UPL_TYPE_META, (uint16_t)n, 0U, 0);
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
    Up_Emit(UPL_TYPE_SNAPSHOT, (uint16_t)n, 0U, 0);
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
    Up_Emit(UPL_TYPE_EVENT, (uint16_t)n, 0U, 0);
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

    /* routine per-frame events never reach the uplink (alarm semantics,
     * same filter as the CAN event channel docs/03 §4.3) */
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

#if (FORM_MODE == FORM_M2)
    Esp_Init();          /* USART3 + RX ring; SM advances via Mqtt_Service */
    Mqtt_Init();
    Backfill_Init();
#endif

    for (;;)
    {
        uint32_t now = HAL_GetTick();
        uint32_t packed;

#if (FORM_MODE == FORM_M2)
        Mqtt_Service(now);             /* ESP SM + MQTT sync + reconnect  */
        Backfill_Tick(now);            /* replay pending, <=8 per round   */
#endif

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

            Up_TxEvent(ev,
                       (uint8_t)((packed >> 16) & 0xFFU),
                       (uint8_t)((packed >> 8) & 0xFFU),
                       (uint8_t)(packed & 0xFFU));
            if (ev == (uint8_t)LOG_CONFIG_CHANGE)
            {
                meta_due = 1U;                /* target sid/baud may differ */
            }
        }

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

const char *Uplink_TopicForType(uint8_t type) { (void)type; return 0; }
void Uplink_GetQosRetain(uint8_t type, uint8_t *qos, uint8_t *retain)
{
    (void)type; *qos = 1U; *retain = 0U;
}

#endif /* ROLE_IS_GW */
