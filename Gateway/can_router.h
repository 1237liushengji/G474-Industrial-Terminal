/**
  ******************************************************************************
  * @file    can_router.h
  * @brief   Table-driven CAN frame routing (docs/03 §4.6).
  *
  *  New frame type = one table row + one handler; the dispatch switch never
  *  changes.  Handlers run in the can_task context (T1); they must stay
  *  short and non-blocking (register mirror, link bookkeeping, event queue).
  ******************************************************************************
  */
#ifndef __CAN_ROUTER_H
#define __CAN_ROUTER_H

#include <stdint.h>
#include "can_drv.h"

/** One route: (id & id_mask) == id_value and len >= dlc_min -> handler. */
typedef struct
{
    uint32_t id_mask;
    uint32_t id_value;
    uint8_t  dlc_min;
    void   (*handler)(const CAN_RxMsg *msg);
} can_route_t;

/** Dispatch one received frame over the role's route table. */
void CanRouter_Dispatch(const CAN_RxMsg *msg);

/** Frames that matched no route (debug counter; expect only 0x1F5 selftest). */
uint32_t CanRouter_GetUnmatched(void);

#endif /* __CAN_ROUTER_H */
