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

/*====================================================================*/
/* Geometry (absolute flash addresses)                                 */
/*====================================================================*/
#define LOG_AREA_BASE       (W25Q_CAPACITY - LOG_AREA_SIZE)
#define LOG_DATA_SIZE       LOG_AREA_SIZE                 /* no header */
#define LOG_SECTOR_RECORDS  (LOG_SECTOR_SIZE / LOG_RECORD_SIZE)  /* 128 */
#define LOG_SECTOR_COUNT    (LOG_DATA_SIZE / LOG_SECTOR_SIZE)     /* 16  */
#define LOG_MAX_RECORDS     (LOG_DATA_SIZE / LOG_RECORD_SIZE)     /* 2048 */

/* RAM cache depth: most recent records kept for instant reads */
#define LOG_CACHE_CAP       512U

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

static void RecLoad(uint32_t off, log_record_t *rec)
{
    W25Q_Read(AbsAddr(off), (uint8_t *)rec, LOG_RECORD_SIZE);
}

static void RecStore(uint32_t off, const log_record_t *rec)
{
    W25Q_Write(AbsAddr(off), (const uint8_t *)rec, LOG_RECORD_SIZE);
}

static uint32_t RecSeqAt(uint32_t off)
{
    log_record_t r;
    RecLoad(off, &r);
    return r.seq;
}

/* erase the 4KB sector that contains area-offset off */
static void EraseSectorOf(uint32_t off)
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

    if (W25Q_ReadID() == 0U)
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
    for (off = 0U; off < LOG_DATA_SIZE; off += LOG_RECORD_SIZE)
    {
        RecLoad(off, &rec);
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
            EraseSectorOf(off);
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
        EraseSectorOf(0U);
        /* cache may hold last 512 already - good enough for reads */
    }
}

void LOG_WriteEvent(log_event_t ev, uint8_t p0, uint8_t p1, uint8_t p2)
{
    log_record_t rec;
    uint32_t next;

    if (!s_ready)
    {
        return;
    }

    next = s_w_off + LOG_RECORD_SIZE;
    if (next > LOG_DATA_SIZE)
    {
        /* wrapped past the end: back to start, ring rotates here */
        EraseSectorOf(0U);
        s_w_off = 0U;
        next = LOG_RECORD_SIZE;
    }
    else if ((s_w_off % LOG_SECTOR_SIZE) == 0U && s_w_off != 0U)
    {
        /* entering a new sector that may still hold the oldest records */
        uint32_t chk;
        RecLoad(s_w_off, &rec);
        chk = rec.seq;
        if (chk != REC_EMPTY)
        {
            EraseSectorOf(s_w_off);
        }
    }

    s_last_seq++;
    memset(&rec, 0xFF, sizeof(rec));
    rec.seq = s_last_seq;
    rec.timestamp_ms = (s_tick_ms != 0) ? s_tick_ms() : 0U;
    rec.event = (uint8_t)ev;
    rec.param[0] = p0;
    rec.param[1] = p1;
    rec.param[2] = p2;

    RecStore(s_w_off, &rec);
    Cache_Push(&rec);
    s_written_total = s_last_seq;
    s_w_off = next;
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
