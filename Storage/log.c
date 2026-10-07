/**
  ******************************************************************************
  * @file    log.c
  * @brief   Ring log on W25Q128 - sector rotating, crash recoverable.
  *
  *  Design:
  *   - fixed log area at the END of the 16 MB flash (LOG_AREA_SIZE)
  *   - 32-byte records; 4 KB sector holds 128 records
  *   - records carry a monotonic 32-bit seq; no separate header -> the
  *     flash itself is the state (torn writes can only lose one record)
  *   - write position always points at the hole: when the area is full,
  *     the oldest sector (the one the write pointer is about to enter) is
  *     erased first -> simple rotating ring, even wear-out
  *   - power-up: scan for the first erased record (or the seq discontinuity
  *     when 100% full) to relocate the write pointer
  *   - a RAM cache keeps the most recent LOG_CACHE_CAP records for fast
  *     random access (LCD / tools); the whole flash area can be dumped by
  *     the PC tool directly for full history
  ******************************************************************************
  */
#include "log.h"
#include "bsp_w25q128.h"
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"

/*====================================================================*/
/* Geometry (absolute flash addresses)                                 */
/*====================================================================*/
#define LOG_AREA_BASE       (W25Q_CAPACITY - LOG_AREA_SIZE)
#define LOG_DATA_SIZE       LOG_AREA_SIZE                 /* no header */
#define LOG_SECTOR_RECORDS  (LOG_SECTOR_SIZE / LOG_RECORD_SIZE)  /* 128 */
#define LOG_SECTOR_COUNT    (LOG_DATA_SIZE / LOG_SECTOR_SIZE)     /* 16  */
#define LOG_MAX_RECORDS     (LOG_DATA_SIZE / LOG_RECORD_SIZE)     /* 2048 */

/* RAM cache depth: most recent records kept for instant reads.
 * v0.2: 512 -> 128, reclaiming 12 KB of SRAM (512x32 B = 16 KB before) to
 * pay for the FreeRTOS heap - see docs/02 decision 6 and docs/04 §6.
 * NOTE for v0.6 backfill: LOG_Read() can only reach back 128 records from
 * now on; the offline backfill MUST use the per-seq direct flash read API
 * (to be added in v0.6), never LOG_Read(). */
#define LOG_CACHE_CAP       128U

#define REC_EMPTY           0xFFFFFFFFU

/*====================================================================*/
/* State                                                               */
/*====================================================================*/
static uint8_t  s_ready = 0U;
static uint32_t s_w_off = 0U;          /* offset (area-relative) of next write */
static uint32_t s_last_seq = 0U;       /* highest seq seen                    */
static uint32_t s_written_total = 0U;  /* total records ever written          */
static uint32_t (*s_tick_ms)(void) = 0;

static log_record_t s_cache[LOG_CACHE_CAP];
static uint16_t s_cache_first = 0U;    /* ring index of oldest cached record  */
static uint16_t s_cache_len = 0U;      /* valid entries                        */

/* v0.6: flash access mutex.  Writers live in log_task, but the MQTT
 * backfill scans pending records from uplink_task - every physical access
 * goes through the lock.  Created in LOG_Init (single writer at boot). */
static SemaphoreHandle_t s_flash_lock;
static uint32_t s_pending_lost;

static void Log_Lock(void)
{
    if (s_flash_lock != 0)
    {
        (void)xSemaphoreTake(s_flash_lock, portMAX_DELAY);
    }
}

static void Log_Unlock(void)
{
    if (s_flash_lock != 0)
    {
        (void)xSemaphoreGive(s_flash_lock);
    }
}

void LOG_SetTickSource(uint32_t (*tick_ms)(void))
{
    s_tick_ms = tick_ms;
}

/*====================================================================*/
/* Record IO (area-relative offsets)                                   */
/*====================================================================*/
static uint32_t AbsAddr(uint32_t off)
{
    return LOG_AREA_BASE + off;
}

/* unlocked primitives - callers hold Log_Lock */
static void RecLoadU(uint32_t off, log_record_t *rec)
{
    W25Q_Read(AbsAddr(off), (uint8_t *)rec, LOG_RECORD_SIZE);
}

static void RecStoreU(uint32_t off, const log_record_t *rec)
{
    W25Q_Write(AbsAddr(off), (const uint8_t *)rec, LOG_RECORD_SIZE);
}

static void RecLoad(uint32_t off, log_record_t *rec)
{
    Log_Lock();
    RecLoadU(off, rec);
    Log_Unlock();
}

/* erase the 4KB sector that contains area-offset off (caller holds lock) */
static void EraseSectorOfU(uint32_t off)
{
    uint32_t sector_area_off = off & ~(uint32_t)(LOG_SECTOR_SIZE - 1U);
    W25Q_EraseSector(AbsAddr(sector_area_off));
}

/*====================================================================*/
/* Cache helpers                                                       */
/*====================================================================*/
static void Cache_Push(const log_record_t *rec)
{
    if (s_cache_len < LOG_CACHE_CAP)
    {
        uint16_t idx = (uint16_t)((s_cache_first + s_cache_len) % LOG_CACHE_CAP);
        s_cache[idx] = *rec;
        s_cache_len++;
    }
    else
    {
        /* overwrite oldest */
        s_cache[s_cache_first] = *rec;
        s_cache_first = (uint16_t)((s_cache_first + 1U) % LOG_CACHE_CAP);
    }
}

static uint8_t Cache_Get(uint32_t index, log_record_t *rec) /* 0 = oldest */
{
    if (index >= s_cache_len)
    {
        return 0U;
    }
    {
        uint16_t idx = (uint16_t)((s_cache_first + index) % LOG_CACHE_CAP);
        *rec = s_cache[idx];
    }
    return 1U;
}

/*====================================================================*/
/* Init / recovery scan                                                */
/*====================================================================*/
void LOG_Init(void)
{
    uint32_t off = 0U;
    log_record_t rec;

    if (s_flash_lock == 0)
    {
        s_flash_lock = xSemaphoreCreateMutex();
    }

    if ((W25Q_ReadID() == 0U) || (W25Q_IsPresent() == 0U))
    {
        s_ready = 0U;
        return;
    }
    s_ready = 1U;
    s_cache_first = 0U;
    s_cache_len = 0U;
    s_last_seq = 0U;
    s_written_total = 0U;
    s_w_off = 0U;

    /* scan the whole area to find the write hole */
    Log_Lock();
    for (off = 0U; off < LOG_DATA_SIZE; off += LOG_RECORD_SIZE)
    {
        RecLoadU(off, &rec);
        if (rec.seq == REC_EMPTY)
        {
            s_w_off = off;              /* first empty record = write pos */
            break;
        }
        if (s_last_seq != 0U && rec.seq != (s_last_seq + 1U))
        {
            /* discontinuity (100% full ring): the next write overwrites
               the oldest record at this offset - we must erase its sector */
            s_w_off = off;
            s_written_total = s_last_seq;
            EraseSectorOfU(off);
            break;
        }
        s_last_seq = rec.seq;
        s_written_total = rec.seq;
        Cache_Push(&rec);
    }
    if (off >= LOG_DATA_SIZE)
    {
        /* area completely full and contiguous: wrap to start, erase it */
        s_w_off = 0U;
        EraseSectorOfU(0U);
        /* cache may hold the newest records already - good enough for reads */
    }
    Log_Unlock();
}

/**
  * @brief  Append one prepared record (seq/timestamp filled here).
  * @note   Core of every write path.  Handles ring rotation, sector
  *         pre-erase and the pending-overwrite accounting (v0.6).
  */
static uint32_t Log_Append(log_record_t *rec)
{
    uint32_t next;
    log_record_t old;

    if (!s_ready)
    {
        return 0U;
    }

    Log_Lock();

    next = s_w_off + LOG_RECORD_SIZE;
    if (next > LOG_DATA_SIZE)
    {
        /* wrapped past the end: back to start, ring rotates here.
         * (records in sector 0 are dropped wholesale - the pending-lost
         * count can only approximate that case, hence ">=" semantics) */
        EraseSectorOfU(0U);
        s_w_off = 0U;
        next = LOG_RECORD_SIZE;
    }
    else if ((s_w_off % LOG_SECTOR_SIZE) == 0U && s_w_off != 0U)
    {
        /* entering a new sector that may still hold the oldest records */
        RecLoadU(s_w_off, &old);
        if (old.seq != REC_EMPTY)
        {
            EraseSectorOfU(s_w_off);
        }
    }

    /* pending-overwrite accounting: if the slot we are about to write holds
     * an unconsumed offline MQTT record, it is being lost right now */
    RecLoadU(s_w_off, &old);
    if ((old.seq != REC_EMPTY) && (old.event == (uint8_t)LOG_MQTT_PENDING))
    {
        s_pending_lost++;
    }

    s_last_seq++;
    memset(rec, 0xFF, sizeof(*rec));
    rec->seq = s_last_seq;
    rec->timestamp_ms = (s_tick_ms != 0) ? s_tick_ms() : 0U;

    RecStoreU(s_w_off, rec);
    Log_Unlock();

    Cache_Push(rec);
    s_written_total = s_last_seq;
    s_w_off = next;
    return s_last_seq;
}

void LOG_WriteEvent(log_event_t ev, uint8_t p0, uint8_t p1, uint8_t p2)
{
    log_record_t rec;

    if (!s_ready)
    {
        return;
    }
    rec.event = (uint8_t)ev;
    rec.param[0] = p0;
    rec.param[1] = p1;
    rec.param[2] = p2;
    (void)Log_Append(&rec);
}

uint32_t LOG_PendingPush(uint8_t type, uint8_t seq_lo,
                         const uint8_t fields[LOG_PENDING_FIELDS])
{
    log_record_t rec;

    if (!s_ready || (fields == 0))
    {
        return 0U;
    }
    rec.event = (uint8_t)LOG_MQTT_PENDING;
    rec.param[0] = type;
    rec.param[1] = seq_lo;
    rec.param[2] = 0U;
    memcpy(rec.reserved, fields, LOG_PENDING_FIELDS);
    return Log_Append(&rec);
}

uint8_t LOG_PendingScan(uint32_t after_seq, log_pending_t *out, uint8_t max_n,
                        uint32_t *newest_seq)
{
    uint32_t off;
    uint32_t newest = 0U;
    uint8_t  n = 0U;

    if ((out == 0) || (newest_seq == 0) || (max_n == 0U))
    {
        return 0U;
    }
    *newest_seq = 0U;
    if (!s_ready)
    {
        return 0U;
    }

    Log_Lock();
    for (off = 0U; off < LOG_DATA_SIZE; off += LOG_RECORD_SIZE)
    {
        log_record_t rec;

        RecLoadU(off, &rec);
        if ((rec.seq == REC_EMPTY) || (rec.event != (uint8_t)LOG_MQTT_PENDING))
        {
            continue;
        }
        if (rec.seq > newest)
        {
            newest = rec.seq;
        }
        if (rec.seq <= after_seq)
        {
            continue;
        }

        /* keep the SMALLEST max_n seqs (publish must be seq-ascending):
         * insert sorted; a full array drops its largest entry */
        {
            log_pending_t cand;
            uint8_t i;

            if ((n >= max_n) && (rec.seq >= out[max_n - 1U].seq))
            {
                continue;                  /* newer than everything we hold */
            }
            cand.seq = rec.seq;
            cand.ts = rec.timestamp_ms;
            cand.seq_lo = rec.param[1];
            cand.type = rec.param[0];
            memcpy(cand.fields, rec.reserved, LOG_PENDING_FIELDS);

            /* shift to make room (ascending) */
            i = n;
            while ((i > 0U) && (out[i - 1U].seq > cand.seq))
            {
                if (i < max_n)
                {
                    out[i] = out[i - 1U];
                }
                i--;
            }
            if (i < max_n)
            {
                out[i] = cand;
            }
            if (n < max_n)
            {
                n++;
            }
        }
    }
    Log_Unlock();

    *newest_seq = newest;
    return n;
}

uint32_t LOG_PendingLostCount(void)
{
    return s_pending_lost;
}

uint32_t LOG_GetCount(void)
{
    return s_written_total;   /* total written since format */
}

uint32_t LOG_GetLastSeq(void)
{
    return s_last_seq;
}

uint8_t LOG_Read(uint32_t index, log_record_t *rec)
{
    if (!s_ready || (rec == 0))
    {
        return 0U;
    }
    return Cache_Get(index, rec);
}
