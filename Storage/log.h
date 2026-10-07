/**
  ******************************************************************************
  * @file    log.h
  * @brief   Ring log on W25Q128 (Storage layer).
  *
  *  Design (docs/08-Storage设计.md):
  *   - dedicated log area at the END of the 16 MB flash, LOG_AREA_SIZE bytes
  *   - fixed-size records (LOG_RECORD_SIZE), one sector holds many records
  *   - headerless on-flash format: every record carries a monotonic 32-bit
  *     sequence number, so the flash itself is the state - torn writes can
  *     only lose one record (recovery = scan for first erased record or a
  *     sequence discontinuity on boot)
  *   - when full, the sector the write pointer is about to enter is erased
  *     first (simple rotating ring, even wear-out)
  *   - a RAM cache keeps the most recent records for fast reads (full
  *     history can be dumped by the PC tool directly from flash)
  ******************************************************************************
  */
#ifndef __LOG_H
#define __LOG_H

#include <stdint.h>
#include "types.h"   /* log_event_t */

/* Configuration: 256 KB log area = 64 sectors, record 32 B -> 8192 recs.
 * v0.2: grown from 64 KB now that the on-board W25Q128 is actually driven on
 * the correct SPI1 bus (docs/07 §3/§6, 迁移说明 §17).  The power-up area scan is no longer
 * a boot blocker - it runs inside log_task (Storage/log_service.c). */
#define LOG_AREA_SIZE      (256UL * 1024UL)
#define LOG_RECORD_SIZE    32U
#define LOG_SECTOR_SIZE    4096U

typedef struct {
    uint32_t seq;           /* monotonically increasing                */
    uint32_t timestamp_ms;  /* HAL_GetTick() at event time             */
    uint8_t  event;         /* log_event_t                             */
    uint8_t  param[3];      /* extra context (e.g. exception code)     */
    uint8_t  reserved[LOG_RECORD_SIZE - 12U];
} log_record_t;

/** Init log subsystem (scans flash to locate the write position). */
void LOG_Init(void);

/** Provide the ms tick source (e.g. HAL_GetTick). Call before LOG_Init. */
void LOG_SetTickSource(uint32_t (*tick_ms)(void));

/**
  * @brief  Append one event to the ring log (blocking flash write,
  *         call from the main loop context only, not from ISR).
  */
void LOG_WriteEvent(log_event_t ev, uint8_t p0, uint8_t p1, uint8_t p2);

/** Total number of records currently stored (approximate after wrap). */
uint32_t LOG_GetCount(void);

/** Last sequence number written. */
uint32_t LOG_GetLastSeq(void);

/**
  * @brief  Read a stored record by index (0 = oldest, count-1 = newest).
  * @retval 1 ok, 0 index out of range
  */
uint8_t LOG_Read(uint32_t index, log_record_t *rec);

/*====================================================================*/
/* MQTT offline pending records (v0.6, docs/03 §5.3)                    */
/*====================================================================*/
/* A pending record is a normal 32 B flash record with event =
 * LOG_MQTT_PENDING.  Payload layout (frozen here, mirrored by the
 * formatters in Gateway/uplink_port.c):
 *
 *   seq(4) | timestamp_ms(4) | event=12(1) | param[0]=msg type(1)
 *   | param[1..2]=0(2) | fields[20]
 *
 *   type 1 telemetry : f[0..1] temp  f[2..3] humi  f[4..5] volt  f[6..7] curr   (BE u16)
 *   type 2 status    : f[0] st  f[1] er  f[2] lk  f[3..4] ch  f[5] cv  f[6..7] heap
 *   type 3 counters  : 10 x BE u16: rx tx crc can_ack can_ovr can_bo can_lat mb_to mb_rt mb_exc
 *   (meta/snapshot/event are NOT cached: they are periodic/regenerated -
 *    same trade-off the capacity calculation in docs/03 §5.3 assumes.)
 *
 * Only telemetry/status/counters are cached, and consumption is "publish
 * then advance": duplicates after a reboot are allowed by design - the
 * subscriber de-duplicates on `seq` (docs/03 §5.3).
 */
#define LOG_PENDING_FIELDS   20U

typedef struct
{
    uint32_t seq;
    uint32_t ts;
    uint8_t  seq_lo;    /* payload "seq" byte (telemetry): peer seq at capture */
    uint8_t  type;
    uint8_t  fields[LOG_PENDING_FIELDS];
} log_pending_t;

/**
  * @brief  Append one pending record (log_task context).  Returns its seq.
  * @param  seq_lo  payload seq byte carried in param[1] (telemetry only; 0
  *                 for status/counters which have no seq field)
  */
uint32_t LOG_PendingPush(uint8_t type, uint8_t seq_lo,
                         const uint8_t fields[LOG_PENDING_FIELDS]);

/**
  * @brief  Collect up to max_n pending records with seq > after_seq,
  *         ascending (single pass; safe from any task - flash access is
  *         mutex-protected).
  * @param  newest_seq receives the highest pending seq existing (0 = none)
  * @retval number of records written to out[]
  */
uint8_t LOG_PendingScan(uint32_t after_seq, log_pending_t *out, uint8_t max_n,
                        uint32_t *newest_seq);

/** Pending records overwritten by the ring (data loss; A-07 watches this). */
uint32_t LOG_PendingLostCount(void);

#endif /* __LOG_H */
