/**
  ******************************************************************************
  * @file    dp_table.h
  * @brief   Data-point table - the contract (docs/03 §3) as compile-time data.
  *
  *  Single source of the three projections (docs/03 §1):
  *    Modbus register parsing  ->  CAN packing (v0.4)  ->  MQTT JSON (v0.5)
  *  and of the contract hash:
  *
  *    hash = CRC16/Modbus over the canonical string built row by row:
  *           key ':' pdu(2 upper-case hex) ':' scale ':' rw
  *           rows joined by '\n' (LF), NO trailing newline.
  *    rw is "RO" or "RW".
  *
  *  Row set: PDU 0x00..0x37 = 56 register slots minus the 0x0F sentinel hole
  *  = 55 rows.  0x38..0x3F are reserved slots that appear in NO projection
  *  (neither snapshot nor meta) - docs/03 §4.4.
  *
  *  The PC mirror is Tools/python/dp_table.json; Tools/python/contract_check.py
  *  cross-checks this C table against the JSON and both against the firmware
  *  register 0x18 (acceptance A-05).
  ******************************************************************************
  */
#ifndef __DP_TABLE_H
#define __DP_TABLE_H

#include <stdint.h>

#define DP_RW_RO          0U
#define DP_RW_RW          1U

/** One data point. */
typedef struct
{
    const char *key;        /* MQTT JSON field name, lower-case            */
    uint8_t     pdu;        /* Modbus PDU address (0x00..0x37)             */
    uint8_t     scale;      /* decimal digits: 0/1/2, value = raw / 10^s   */
    uint8_t     rw;         /* DP_RW_RO / DP_RW_RW                         */
} dp_entry_t;

/** The table (PDU ascending; the 0x0F hole is not a data point). */
extern const dp_entry_t g_dp_table[];
extern const uint16_t   g_dp_table_len;

/**
  * @brief  Contract hash (CRC16/Modbus of the canonical string above).
  * @note   Computed once, lazily, then cached.  Stored into register 0x18 by
  *         the application bring-up (App/app_rtos.c).
  */
uint16_t DP_Table_ContractHash(void);

#endif /* __DP_TABLE_H */
