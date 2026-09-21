/**
  ******************************************************************************
  * @file    modbus_master.h
  * @brief   Modbus RTU master engine (platform independent).
  *
  *  One transaction at a time:
  *    IDLE --request--> SEND --frame out--> WAIT --response--> DONE
  *                        |                   | timeout
  *                        |                   v
  *                        |               RETRY (max N) --still no--> FAIL
  *
  *  The app calls MB_Master_Poll(now_us) often; a finished transaction is
  *  reported by Poll returning 1, then MB_Master_GetResult() gives details.
  ******************************************************************************
  */
#ifndef __MODBUS_MASTER_H
#define __MODBUS_MASTER_H

#include <stdint.h>
#include "types.h"   /* comm_stats_t */

/** TX function (RS485_SendFrame from BSP). */
typedef void (*mb_master_tx_func_t)(const uint8_t *data, uint16_t len);

/** Protocol event callback (timeout after retries), engine context. */
typedef void (*mb_master_event_cb_t)(uint8_t event, uint16_t param);

/** Transaction result as parsed by the master engine. */
typedef struct {
    uint8_t  active;        /* 1 while transaction pending            */
    uint8_t  ok;            /* 1 = valid response received            */
    uint8_t  exception;     /* exception code when slave answered exc  */
    uint8_t  timed_out;     /* 1 = retries exhausted                  */
    uint8_t  func;          /* request function code                  */
    uint8_t  slave_id;      /* addressed slave                        */
    uint16_t payload_len;   /* data bytes in payload (no addr/func)    */
    uint8_t  payload[250];  /* response payload (regs/coils/...)       */
} mb_master_result_t;

/**
  * @brief  Initialize the master engine.
  * @param  slave_id    default slave to poll
  * @param  baudrate    used for t3.5 idle detection
  * @param  timeout_ms  per-attempt response timeout (ms)
  * @param  retry_max   retries after first attempt (0 = single try)
  */
void MB_Master_Init(uint8_t slave_id, uint32_t baudrate,
                    uint32_t timeout_ms, uint8_t retry_max);

/** Set TX function. */
void MB_Master_SetTxFunc(mb_master_tx_func_t f);

/** Set protocol event callback (may be NULL). */
void MB_Master_SetEventCallback(mb_master_event_cb_t cb);

/** Feed received bytes (call from UART RX ISR). */
void MB_Master_OnRxByte(uint8_t byte, uint32_t now_us);

/**
  * @brief  Poll engine. Call very often.
  * @param  now_us
  * @retval 1 when a transaction just finished (inspect GetResult),
  *         0 otherwise
  */
uint8_t MB_Master_Poll(uint32_t now_us);

/** Feed received bytes (call from UART RX ISR, interrupt mode). */
void MB_Master_OnRxByte(uint8_t byte, uint32_t now_us);

/**
  * @brief  Feed one complete response frame (DMA mode; main-loop context).
  * @param  frame  complete RTU frame incl. CRC
  * @param  len
  * @retval 1 when a response was processed
  */
uint8_t MB_Master_OnRxFrame(const uint8_t *frame, uint16_t len);

/** Last finished result. */
const mb_master_result_t *MB_Master_GetResult(void);

/** True while a transaction is in flight. */
uint8_t MB_Master_IsBusy(void);

/** 1 when the last transaction succeeded, 0 otherwise (comm fault flag). */
uint8_t MB_Master_CommOk(void);

/** Number of consecutive failures (reset on success). */
uint16_t MB_Master_GetConsecutiveFails(void);

/** Runtime tuning of timeout / retry. */
void MB_Master_SetTiming(uint32_t timeout_ms, uint8_t retry_max);

/*====================================================================*/
/* Request builders (async; results come via Poll/GetResult)           */
/*====================================================================*/

/** 0x01 Read Coils. start/qty in coil addresses. */
uint8_t MB_Master_ReadCoils(uint8_t slave, uint16_t start, uint16_t qty);

/** 0x03 Read Holding Registers. */
uint8_t MB_Master_ReadHolding(uint8_t slave, uint16_t start, uint16_t qty);

/** 0x06 Write Single Register. */
uint8_t MB_Master_WriteSingle(uint8_t slave, uint16_t addr, uint16_t value);

/** 0x10 Write Multiple Registers (values big-endian pairs). */
uint8_t MB_Master_WriteMulti(uint8_t slave, uint16_t start,
                             const uint16_t *values, uint16_t qty);

/** Communication statistics. */
void MB_Master_GetStats(comm_stats_t *out);

#endif /* __MODBUS_MASTER_H */
