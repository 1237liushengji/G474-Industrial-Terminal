/**
  ******************************************************************************
  * @file    bridge.h
  * @brief   CAN <-> register bridge + snapshot channel (T3, v0.4).
  *
  *  Node (B board) - producers:
  *    0x101 TELEMETRY_FAST 1 s, 0x111 STATUS 5 s, 0x121 COUNTERS 10 s,
  *    0x501 HB 1 s, 0x201 EVENT (on log events), 0x181 SNAPSHOT on request.
  *  Node - consumer:
  *    0x301 CMD -> MB_REG_SetHolding (config writes reuse the v2.0 dirty ->
  *    persist path; "apply now, ACK now, flash write follows"), 0x381 ACK out;
  *    duplicate cmd_id (last 8) re-ACKs WITHOUT re-executing (idempotent).
  *
  *  Gateway (A board) - producers:
  *    0x500 HB 1 s, 0x1A0 snapshot request 30 s + on-demand (console `snap`,
  *    and automatically after a node LOG_CONFIG_CHANGE event), 0x301 CMD from
  *    the console with 500 ms timeout + one retry.
  *  Gateway - mirror (the MQTT source of truth, v0.5):
  *    0x101 -> 0x00-0x03, 0x111 -> 0x04/0x05, 0x121 -> 0x06-0x08/0x20,
  *    snapshot -> full mirror domain (0x00-0x0F minus hole + 0x20-0x22).
  *    Writes use MB_REG_SetHoldingRaw (no persist-dirty on the gateway).
  *
  *  Both roles maintain: 0x10-0x17 stats/link regs, 0x1D uptime, 0x1E heap,
  *  0x1F task_ovr, and their own 0x1C hb_seq (++ on every HB send).
  ******************************************************************************
  */
#ifndef __BRIDGE_H
#define __BRIDGE_H

#include <stdint.h>
#include "can_drv.h"

/** T3 body: periodic producers + mirror + link watchdog + snapshot engine. */
void Bridge_TaskMain(void);

/*--------------------------------------------------------------------*/
/* RX handlers (called from can_router, can_task context)              */
/*--------------------------------------------------------------------*/
void Bridge_RxTelemetry(const CAN_RxMsg *m);
void Bridge_RxStatus(const CAN_RxMsg *m);
void Bridge_RxCounters(const CAN_RxMsg *m);
void Bridge_RxEvent(const CAN_RxMsg *m);
void Bridge_RxCmd(const CAN_RxMsg *m);
void Bridge_RxCmdAck(const CAN_RxMsg *m);
void Bridge_RxHeartbeat(const CAN_RxMsg *m);
void Bridge_RxSnapshotFrame(const CAN_RxMsg *m);

/*--------------------------------------------------------------------*/
/* Hooks from the role application                                     */
/*--------------------------------------------------------------------*/
/** Node: forward one log event onto 0x201 (single-slot, overwrite on burst). */
void Bridge_NotifyEvent(uint8_t ev, uint8_t p0, uint8_t p1, uint8_t p2);

/*--------------------------------------------------------------------*/
/* Console / diagnostics (gateway)                                     */
/*--------------------------------------------------------------------*/
/** Queue one 0x06 CMD (reg = PDU address). 1 accepted, 0 busy. */
uint8_t Bridge_RequestCmd(uint16_t reg, uint16_t val);

/** Force one snapshot request on the next loop. */
void Bridge_RequestSnapshot(void);

/** Milliseconds since the last peer heartbeat (0 = never seen). */
uint32_t Bridge_GetHbAgeMs(void);

/** Peer link state as tracked by the HB watchdog (1 = up). */
uint8_t Bridge_PeerUp(void);

/** Snapshot channel counters. */
uint32_t Bridge_GetSnapOkCount(void);
uint32_t Bridge_GetSnapErrCount(void);
uint32_t Bridge_GetSnapAgeMs(void);

/** Last CMD result echo (gateway side): 0=none,1=ok,2=exception. */
uint8_t Bridge_GetLastCmdResult(void);
uint8_t Bridge_GetLastCmdExc(void);

/** Node publish-seq low byte as seen on 0x111 B7 / 0x501 (uplink "seq"). */
uint8_t Bridge_GetPeerSeqLo(void);

/**
  * @brief  Full 56-register snapshot shadow (PDU 0x00..0x37) as received on
  *         0x181 - the source of the `snapshot` uplink topic.
  * @retval register value, or 0 while no valid snapshot arrived yet
  *         (check Bridge_SnapValid()).
  */
uint16_t Bridge_GetSnapReg(uint16_t pdu);
uint8_t  Bridge_SnapValid(void);
uint32_t Bridge_GetSnapHashMismatch(void);

#endif /* __BRIDGE_H */
