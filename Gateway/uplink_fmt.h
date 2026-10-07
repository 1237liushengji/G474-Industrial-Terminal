/**
  ******************************************************************************
  * @file    uplink_fmt.h
  * @brief   Uplink JSON formatters - the single source of the wire format.
  *
  *  Used by: M1 frame builder, M2 MQTT publisher and the offline backfill
  *  (which rebuilds a message from the compact fields stored in flash).
  *  Field layouts are frozen in Storage/log.h (matching these formatters).
  ******************************************************************************
  */
#ifndef __UPLINK_FMT_H
#define __UPLINK_FMT_H

#include <stdint.h>

/** telemetry: f[0..1] temp, [2..3] humi, [4..5] volt, [6..7] curr (BE) */
uint16_t Uplink_FormatTelemetry(char *b, uint16_t cap, uint8_t seq_lo,
                                uint32_t ts, const uint8_t f[8]);

/** status: f[0] st, [1] er, [2] lk, [3..4] ch, [5] cv, [6..7] heap */
uint16_t Uplink_FormatStatus(char *b, uint16_t cap, uint32_t ts,
                             const uint8_t f[8]);

/** counters: 10 x BE u16: rx tx crc can_ack can_ovr can_bo can_lat mb_to mb_rt mb_exc */
uint16_t Uplink_FormatCounters(char *b, uint16_t cap, const uint8_t f[20]);

#endif /* __UPLINK_FMT_H */
