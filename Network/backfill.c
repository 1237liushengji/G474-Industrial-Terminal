/**
  ******************************************************************************
  * @file    backfill.c
  * @brief   Offline MQTT backfill - implementation (v0.6).
  *
  *  Contract (docs/03 §5.3):
   *   - strict seq-ascending replay, at most BACKFILL_BURST (8) per round so a
   *     storm never starves live telemetry;
   *   - consumption is "publish then advance": a reboot may re-send the
   *     window, duplicates are resolved by the subscriber via `seq`;
   *   - records lost to ring overwrite are reported in a LOG_BACKFILL event
   *     (p0 = newly lost count at the moment the backlog drains).
  ******************************************************************************
  */
#include "backfill.h"
#include "app_board_cfg.h"
#include "log.h"
#include "log_service.h"
#include "mqtt_client.h"
#include "uplink_fmt.h"
#include "uplink_port.h"
#include "types.h"
#include "app_rtos.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

#define BACKFILL_BURST_MAX    8U
#define BACKFILL_TICK_MS      500U    /* scan cadence while backlog exists */

static uint32_t s_last_sent_seq;      /* highest seq replayed this boot     */
static uint32_t s_newest_seq;         /* newest pending seq seen            */
static uint32_t s_replayed;
static uint32_t s_lost_reported;
static uint32_t s_next_scan_ms;

void Backfill_Init(void)
{
    s_last_sent_seq = 0U;
    s_newest_seq = 0U;
    s_replayed = 0U;
    s_lost_reported = 0U;
    s_next_scan_ms = 0U;
}

void Backfill_Force(void)
{
    s_next_scan_ms = 0U;
}

uint32_t Backfill_GetBacklog(void)
{
    return (s_newest_seq > s_last_sent_seq) ? (s_newest_seq - s_last_sent_seq) : 0U;
}

uint32_t Backfill_GetReplayedCount(void)
{
    return s_replayed;
}

uint32_t Backfill_GetLostCount(void)
{
    return LOG_PendingLostCount();
}

static uint16_t Backfill_Rebuild(const log_pending_t *p, char *b, uint16_t cap)
{
    switch (p->type)
    {
        case 1U:
            return Uplink_FormatTelemetry(b, cap, p->seq_lo,
                                          p->ts / 1000U, p->fields);
        case 2U:
            return Uplink_FormatStatus(b, cap, p->ts / 1000U, p->fields);
        case 3U:
            return Uplink_FormatCounters(b, cap, p->fields);
        default:
            return 0U;
    }
}

void Backfill_Tick(uint32_t now)
{
    log_pending_t batch[BACKFILL_BURST_MAX];
    uint32_t newest = 0U;
    uint8_t  n;
    uint8_t  i;

    if (Mqtt_IsConnected() == 0U)
    {
        return;
    }
    if ((int32_t)(now - s_next_scan_ms) < 0)
    {
        return;
    }
    s_next_scan_ms = now + BACKFILL_TICK_MS;

    n = LOG_PendingScan(s_last_sent_seq, batch, BACKFILL_BURST_MAX, &newest);
    s_newest_seq = newest;

    if (n == 0U)
    {
        /* backlog drained: report ring-overwrite losses once */
        if ((LOG_PendingLostCount() > s_lost_reported) &&
            (s_last_sent_seq >= s_newest_seq))
        {
            uint32_t lost = LOG_PendingLostCount() - s_lost_reported;

            s_lost_reported = LOG_PendingLostCount();
            (void)LOG_SVC_PostEvent((uint8_t)LOG_BACKFILL,
                                    (uint8_t)(lost & 0xFFU),
                                    (uint8_t)((lost >> 8) & 0xFFU), 0U);
        }
        return;
    }

    for (i = 0U; i < n; i++)
    {
        char     buf[MQTT_SEND_BUF];
        uint16_t len = Backfill_Rebuild(&batch[i], buf, (uint16_t)sizeof(buf));
        uint8_t  qos;
        uint8_t  retain;
        const char *topic;

        if (len == 0U)
        {
            s_last_sent_seq = batch[i].seq;   /* unknown type: skip, don't stall */
            continue;
        }
        topic = Uplink_TopicForType(batch[i].type);
        Uplink_GetQosRetain(batch[i].type, &qos, &retain);

        if ((topic == 0) ||
            (Mqtt_PublishRaw(topic, (const uint8_t *)buf, len, qos, retain) == 0U))
        {
            return;                           /* session died mid-round: resume later */
        }
        s_last_sent_seq = batch[i].seq;
        s_replayed++;
    }
}
