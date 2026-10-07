/**
  ******************************************************************************
  * @file    log_service.h
  * @brief   RTOS service wrapper around Storage/log.c + Storage/config.c (T5).
  *
  *  Why a service layer (docs/02 task table T5, decision "T5 absorbs all
  *  blocking"):
  *   - a single W25Q record write can block ~3..20 ms (page program) and a
  *     sector erase up to ~400 ms; a 24C02 save polls ACKs for up to ~50 ms;
  *   - the power-up flash scan (~0.3 s for the 256 KB area) used to run
  *     before the protocol engines even started.
  *  All of it now runs inside log_task, fed by a queue; every other module
  *  only enqueues (non-blocking).
  *
  *  Ownership rules:
  *   - W25Q128 is touched ONLY by log_task (via LOG_SVC_RunInit/Process).
  *   - CONFIG_Save runs in log_task; the caller keeps ownership of the
  *     Modbus "config dirty" flag: on done==1 it clears it, on done==2 it
  *     retries (retry cadence is the caller's policy, 20 ms as in v2.0).
  ******************************************************************************
  */
#ifndef __LOG_SERVICE_H
#define __LOG_SERVICE_H

#include <stdint.h>
#include "types.h"
#include "config.h"

/** Create the request queue.  Call once before the scheduler starts. */
void LOG_SVC_Init(void);

/**
  * @brief  Blocking one-time init, runs inside log_task:
  *         W25Q_Init -> LOG_Init (area scan) -> LOG_BOOT with reset reason.
  */
void LOG_SVC_RunInit(void);

/**
  * @brief  Enqueue one event record (any task context, non-blocking).
  * @retval 1 accepted, 0 dropped (queue full; counter in LOG_SVC_GetDropCount)
  */
uint8_t LOG_SVC_PostEvent(uint8_t ev, uint8_t p0, uint8_t p1, uint8_t p2);

/**
  * @brief  Enqueue a parameter save (any task context, non-blocking).
  * @retval 1 accepted, 0 rejected (queue full or a save is already queued)
  */
uint8_t LOG_SVC_PostConfigSave(const config_param_t *p);

/**
  * @brief  Enqueue an offline-MQTT pending record (v0.6; any task context,
  *         non-blocking).  Executed in log_task via LOG_PendingPush().
  * @param  fields  LOG_PENDING_FIELDS raw bytes (layout: Storage/log.h)
  * @retval 1 accepted, 0 dropped (queue full / no flash)
  */
uint8_t LOG_SVC_PostPending(uint8_t type, uint8_t seq_lo, const uint8_t *fields);

/**
  * @brief  Poll for a completed save attempt (mb side, task context).
  * @retval 0 nothing new, 1 save OK (caller clears dirty flag),
  *         2 save FAILED (caller keeps dirty flag -> retries)
  */
uint8_t LOG_SVC_TakeConfigSaveDone(void);

/** Process ONE queued request (blocks waiting for one).  log_task loop body. */
void LOG_SVC_Process(void);

/** Number of events dropped because the queue was full (should stay 0). */
uint32_t LOG_SVC_GetDropCount(void);

/** Reset-cause bitmask captured at LOG_SVC_RunInit (RCC->CSR snapshot). */
uint32_t LOG_SVC_GetResetFlags(void);

#endif /* __LOG_SERVICE_H */
