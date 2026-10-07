/**
  ******************************************************************************
  * @file    can_contract.h
  * @brief   CAN contract frames (docs/03 §4): IDs, byte layouts, sentinels.
  *
  *  This header is the CAN leg of the data contract.  The Modbus leg lives in
  *  Protocol/modbus_register.h, the MQTT leg arrives with Network/ (v0.6).
  *  Whenever docs/03 changes a frame, THIS file changes first.
  *
  *  Bus: classic CAN, 500 kbit/s, 11-bit standard IDs, node number N as the
  *  ID low bits (A board = 0, B board = 1).  The v1 topology is exactly two
  *  nodes, so the concrete IDs below cover the system.
  *
  *  Mapping table (docs/03 §4.2):
  *    0x101 TELEMETRY_FAST  node→gw  1 s   temp/humi/volt/curr
  *    0x111 STATUS          node→gw  5 s   status/link/contract meta
  *    0x121 COUNTERS        node→gw 10 s   rx/tx/crc counters + adc1
  *    0x201 EVENT           node→gw  event event enum + params
  *    0x301 CMD             gw→node  on-demand
  *    0x381 CMD_ACK         node→gw  reply to CMD
  *    0x500 HB (gw)         gw→node  1 s
  *    0x501 HB (node)       node→gw  1 s
  *    0x1A0 SNAPSHOT REQ/FC gw→node  30 s + on-demand (ISO-TP)
  *    0x181 SNAPSHOT DATA   node→gw  30 s reply       (ISO-TP, 120 B)
  *    0x7E0/0x7E8 UDS       unchanged since v3.0
  *
  *  Deprecated by contract (do NOT use): 0x321 / 0x322 application
  *  request-response pair of CAN v3.0; their jobs are split into 0x500+N HB,
  *  0x300+N CMD and 0x100+N telemetry.
  ******************************************************************************
  */
#ifndef __CAN_CONTRACT_H
#define __CAN_CONTRACT_H

#include <stdint.h>

/*--------------------------------------- frame IDs ---------------------------------*/
#define CANC_ID_TELEMETRY_FAST   0x101U   /* node -> gw, 1 s, 8 B */
#define CANC_ID_STATUS           0x111U   /* node -> gw, 5 s, 8 B */
#define CANC_ID_COUNTERS         0x121U   /* node -> gw, 10 s, 8 B */
#define CANC_ID_EVENT            0x201U   /* node -> gw, event, 8 B */
#define CANC_ID_CMD              0x301U   /* gw -> node, on-demand, 8 B */
#define CANC_ID_CMD_ACK          0x381U   /* node -> gw, reply, 8 B */
#define CANC_ID_HB_GW            0x500U   /* gw -> node, 1 s, 4 B */
#define CANC_ID_HB_NODE          0x501U   /* node -> gw, 1 s, 4 B */
#define CANC_ID_SNAPSHOT_REQ     0x1A0U   /* gw -> node (ISO-TP: req + FC) */
#define CANC_ID_SNAPSHOT_DATA    0x181U   /* node -> gw (ISO-TP: FF + 5x CF) */

/*--------------------------------------- sentinels ---------------------------------*/
#define CANC_CMD_SENTINEL        0x5CU    /* 0x301 B7 */
#define CANC_EVENT_SENTINEL      0xA5U    /* 0x201 B7 */
#define CANC_ACK_SENTINEL        0xA5U    /* 0x381 B7 */

/*--------------------------------------- 0x301 CMD fields --------------------------*/
#define CANC_CMD_B_FC            0U       /* 0x06 single / 0x10 multi */
#define CANC_CMD_B_ID            1U       /* cmd_id (transaction)     */
#define CANC_CMD_B_ADDR_H        2U       /* reg PDU address BE       */
#define CANC_CMD_B_VAL_H         4U       /* value BE                 */
#define CANC_CMD_B_SUBQTY        6U       /* qty for fc=0x10          */
#define CANC_CMD_B_SENTINEL      7U

/*--------------------------------------- 0x381 CMD_ACK fields ----------------------*/
#define CANC_ACK_B_ID            0U       /* cmd_id echo              */
#define CANC_ACK_B_RESULT        1U       /* 0=none 1=ok 2=exception  */
#define CANC_ACK_B_EXCCODE       2U       /* exception code if result=2 */
#define CANC_ACK_B_ADDR_H        3U       /* reg PDU address BE       */
#define CANC_ACK_B_VAL_H         5U       /* value echo BE            */
#define CANC_ACK_B_SENTINEL      7U

/*--------------------------------------- 0x501 HB fields ---------------------------*/
#define CANC_HB_B_UPTIME_H       0U       /* uptime seconds BE (lo 16) */
#define CANC_HB_B_SEQ            2U       /* publish seq u8            */
#define CANC_HB_B_ROLE           3U       /* node role 0=gw 1=node     */

/*--------------------------------------- snapshot payload (docs/03 §4.4) -----------*/
#define CANC_SNAP_VER            0x01U    /* payload[0]                */
#define CANC_SNAP_NODE           0x01U    /* payload[1]: B board id    */
#define CANC_SNAP_REG_FIRST      0x00U    /* regs[] starts at PDU 0x00 */
#define CANC_SNAP_REG_COUNT      56U      /* 0x00..0x37, hole slot included */
#define CANC_SNAP_PAYLOAD_LEN    120U     /* 1+1+2+112+2+2             */
#define CANC_SNAP_HDR_LEN        4U       /* ver+node+count BE         */
#define CANC_SNAP_RESERVED_LEN   2U
#define CANC_SNAP_CRC_LEN        2U       /* MODBUS_CRC16, LE          */
#define CANC_SNAP_CRC_SPAN       (CANC_SNAP_PAYLOAD_LEN - CANC_SNAP_CRC_LEN)

/* snapshot request payload: SF carrying {0x03, cmd_id} (docs/03 §4.4) */
#define CANC_SNAPREQ_SUBCMD      0x03U
#define CANC_SNAPREQ_PAYLOAD_LEN 2U

/*--------------------------------------- link_mask bits (reg 0x17) -----------------*/
#define CANC_LINK_BIT_RS485      0x01U
#define CANC_LINK_BIT_CAN        0x02U
#define CANC_LINK_BIT_WIFI       0x04U
#define CANC_LINK_BIT_MQTT       0x08U

/*--------------------------------------- small helpers -----------------------------*/
static inline void CANC_PutU16BE(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFU);
}

static inline uint16_t CANC_GetU16BE(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

#endif /* __CAN_CONTRACT_H */
