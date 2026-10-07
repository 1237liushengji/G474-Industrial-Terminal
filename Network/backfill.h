/**
  ******************************************************************************
  * @file    backfill.h
  * @brief   Offline MQTT backfill (v0.6, docs/03 §5.3, A-07).
  ******************************************************************************
  */
#ifndef __BACKFILL_H
#define __BACKFILL_H

#include <stdint.h>

/** Reset the replay cursor (offline records stay in flash, seq-ascending). */
void Backfill_Init(void);

/** Called from uplink_task every 100 ms; replays when the session is up. */
void Backfill_Tick(uint32_t now);

/** Backlog = newest cached seq - last replayed seq (0 = caught up). */
uint32_t Backfill_GetBacklog(void);

/** Records replayed since boot / records lost to ring overwrite. */
uint32_t Backfill_GetReplayedCount(void);
uint32_t Backfill_GetLostCount(void);

#endif /* __BACKFILL_H */
