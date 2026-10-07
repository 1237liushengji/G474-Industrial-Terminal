/**
  ******************************************************************************
  * @file    uplink_fmt.c
  * @brief   Uplink JSON formatters (docs/03 §5.1) - single format source.
  *
  *  Every scaled value goes through fmt_fixed (no %f, docs/03 §5.2).  The
  *  byte layouts here mirror these functions exactly, so a pending record
  *  rebuilt by the backfill is byte-identical to what the live path sent.
  ******************************************************************************
  */
#include "uplink_fmt.h"
#include "fmt.h"

#include <stdio.h>

static uint16_t GetU16BE(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

uint16_t Uplink_FormatTelemetry(char *b, uint16_t cap, uint8_t seq_lo,
                                uint32_t ts, const uint8_t f[8])
{
    int n;

    n = snprintf(b, cap, "{\"seq\":%u,\"ts\":%lu,\"temp\":",
                 (unsigned)seq_lo, (unsigned long)ts);
    if (n < 0 || (uint32_t)n + 4U > cap) { return 0U; }
    n += fmt_fixed(&b[n], (int32_t)GetU16BE(&f[0]), 1U);
    n += snprintf(&b[n], (size_t)(cap - (uint16_t)n), ",\"humi\":");
    n += fmt_fixed(&b[n], (int32_t)GetU16BE(&f[2]), 1U);
    n += snprintf(&b[n], (size_t)(cap - (uint16_t)n), ",\"volt\":");
    n += fmt_fixed(&b[n], (int32_t)GetU16BE(&f[4]), 2U);
    n += snprintf(&b[n], (size_t)(cap - (uint16_t)n), ",\"curr\":");
    n += fmt_fixed(&b[n], (int32_t)GetU16BE(&f[6]), 2U);
    n += snprintf(&b[n], (size_t)(cap - (uint16_t)n), "}");
    return (uint16_t)n;
}

uint16_t Uplink_FormatStatus(char *b, uint16_t cap, uint32_t ts,
                             const uint8_t f[8])
{
    int n;

    n = snprintf(b, cap,
                 "{\"st\":%u,\"er\":%u,\"lk\":%u,\"ch\":\"%04X\",\"cv\":%u,"
                 "\"ts\":%lu,\"heap\":%u}",
                 (unsigned)f[0], (unsigned)f[1], (unsigned)f[2],
                 (unsigned)GetU16BE(&f[3]), (unsigned)f[5],
                 (unsigned long)ts, (unsigned)GetU16BE(&f[6]));
    if (n < 0)
    {
        return 0U;
    }
    return (uint16_t)n;
}

uint16_t Uplink_FormatCounters(char *b, uint16_t cap, const uint8_t f[20])
{
    int n;

    n = snprintf(b, cap,
                 "{\"rx\":%u,\"tx\":%u,\"crc\":%u,"
                 "\"can_ack\":%u,\"can_ovr\":%u,\"can_bo\":%u,\"can_lat\":%u,"
                 "\"mb_to\":%u,\"mb_rt\":%u,\"mb_exc\":%u}",
                 (unsigned)GetU16BE(&f[0]),  (unsigned)GetU16BE(&f[2]),
                 (unsigned)GetU16BE(&f[4]),  (unsigned)GetU16BE(&f[6]),
                 (unsigned)GetU16BE(&f[8]),  (unsigned)GetU16BE(&f[10]),
                 (unsigned)GetU16BE(&f[12]), (unsigned)GetU16BE(&f[14]),
                 (unsigned)GetU16BE(&f[16]), (unsigned)GetU16BE(&f[18]));
    if (n < 0)
    {
        return 0U;
    }
    return (uint16_t)n;
}
