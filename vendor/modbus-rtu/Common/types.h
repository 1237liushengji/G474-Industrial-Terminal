/**
  ******************************************************************************
  * @file    types.h
  * @brief   Platform-independent common types & enumerations
  * @note    This header must NOT include any HAL/CMSIS header,
  *          so that Common/Protocol code can be unit-tested on PC.
  ******************************************************************************
  */
#ifndef __TYPES_H
#define __TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/*====================================================================*/
/*  Node role                                                          */
/*====================================================================*/
#ifndef NODE_ROLE_MASTER
#define NODE_ROLE_MASTER   0
#endif
#ifndef NODE_ROLE_SLAVE
#define NODE_ROLE_SLAVE    1
#endif

/*====================================================================*/
/*  Device top-level state (spec section 6)                            */
/*====================================================================*/
typedef enum {
    DEV_STATE_BOOT = 0,
    DEV_STATE_INIT,
    DEV_STATE_READY,
    DEV_STATE_RUNNING,
    DEV_STATE_CONFIG,
    DEV_STATE_ERROR
} dev_state_t;

/*====================================================================*/
/*  Modbus slave frame receive states                                  */
/*====================================================================*/
typedef enum {
    MB_RX_IDLE = 0,
    MB_RX_RECEIVING,        /* bytes arriving, frame not finished      */
    MB_RX_FRAME_COMPLETE,   /* t3.5 elapsed -> frame ready for parse    */
    MB_RX_PARSE_DONE        /* frame consumed by protocol layer         */
} mb_rx_state_t;

/*====================================================================*/
/*  Modbus master transaction states                                   */
/*====================================================================*/
typedef enum {
    MB_MASTER_IDLE = 0,
    MB_MASTER_POLL,         /* pick next task from queue               */
    MB_MASTER_SEND,         /* frame built, RS485 TX                   */
    MB_MASTER_WAIT,         /* waiting response / timeout timer        */
    MB_MASTER_SUCCESS,      /* response ok, data updated               */
    MB_MASTER_RETRY,        /* will re-send same request               */
    MB_MASTER_TIMEOUT       /* retries exhausted -> comm fault         */
} mb_master_state_t;

/*====================================================================*/
/*  Comm statistics (master & slave)                                   */
/*====================================================================*/
typedef struct {
    uint32_t tx_count;          /* frames transmitted                  */
    uint32_t rx_count;          /* valid frames received               */
    uint32_t crc_error_count;   /* CRC failures                        */
    uint32_t timeout_count;     /* master: no response                 */
    uint32_t retry_count;       /* master: retransmissions             */
    uint32_t exception_count;   /* exception responses received        */
} comm_stats_t;

/*====================================================================*/
/*  Log events (W25Q128 ring log, spec section 13)                     */
/*====================================================================*/
typedef enum {
    LOG_BOOT = 0,
    LOG_RX_OK,
    LOG_TX_OK,
    LOG_TIMEOUT,
    LOG_CRC_ERROR,
    LOG_EXCEPTION,
    LOG_CONFIG_CHANGE,
    LOG_SYSTEM_ERROR
} log_event_t;

#endif /* __TYPES_H */
