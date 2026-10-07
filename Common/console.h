/**
  ******************************************************************************
  * @file    console.h
  * @brief   Serial command console (debug entry point: stats / send / bitrate
  *          switch / bus monitor / UDS diagnostics).
  *
  *  Runs as a low-priority FreeRTOS task since v0.2; polls the USART1 RX ring
  *  through BSP_Console_GetChar().
  *
  *  Frame output format (bus monitor mode, parsed by Tools/python/can_console.py):
  *    RX <ms timestamp> <ID hex 3 digits> <DLC> <data hex 2 digits xN>
  *    e.g.  RX 12345 321 8 01 A2 03 ...
  ******************************************************************************
  */
#ifndef __CONSOLE_H
#define __CONSOLE_H

#include <stdint.h>
#include "can_drv.h"

/** Print the banner (call once the console UART is up). */
void Console_Init(void);

/** Poll RX and execute complete lines; call periodically from console_task. */
void Console_Task(void);

/** 1 while bus monitor mode is armed (can_task prints every frame then). */
uint8_t Console_SniffEnabled(void);

/** Print one received CAN frame in the RX line format above. */
void Console_PrintFrame(const CAN_RxMsg *msg);

/* Debug counters maintained by can_task (kept for the 'stats' command and
 * for locating protocol-dispatch problems under sniff load). */
extern uint32_t g_dbgPoll;   /* frames taken from the RX queue        */
extern uint32_t g_dbgHx;     /* frames dispatched to the protocol     */
extern uint32_t g_dbgCmd;    /* CMD frames seen (0x321)               */
extern uint32_t g_dbgResp;   /* RESP frames seen (0x322)              */

#endif /* __CONSOLE_H */
