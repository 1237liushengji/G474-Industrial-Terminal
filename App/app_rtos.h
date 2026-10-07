/**
  ******************************************************************************
  * @file    app_rtos.h
  * @brief   RTOS bring-up: task set, heartbeats, fault hooks (v0.2).
  *
  *  Task map (docs/02 §3; T3 bridge / T4 uplink arrive in later milestones):
  *    T7 wdt_task    prio 6  256 w  feeds IWDG only while all heartbeats move
  *    T1 can_task    prio 5  512 w  RX queue -> dispatch, bus-off backoff, ISO-TP
  *    T2 mb_task     prio 5  512 w  Modbus master (GW) / slave (node) engine
  *    T5 log_task    prio 2  512 w  flash log + CONFIG_Save, blocks internally
  *    T6 ui_task     prio 1 1024 w  keys + LCD (node) / sleep (gateway)
  *    console_task   prio 2  512 w  debug console (transitional; v0.5 folds
  *                                  console + uplink into T4, docs/02 decision 4)
  ******************************************************************************
  */
#ifndef __APP_RTOS_H
#define __APP_RTOS_H

#include <stdint.h>

/* Heartbeat channels monitored by wdt_task. */
typedef enum
{
    APP_HB_CAN = 0,
    APP_HB_MB,
    APP_HB_LOG,
    APP_HB_UI,
    APP_HB_CONSOLE,
    APP_HB_COUNT
} app_hb_t;

/** Increment one heartbeat channel (each task, every loop). */
void App_Hb_Beat(app_hb_t ch);

/** Current heartbeat counter (wdt_task / diagnostics). */
uint32_t App_Hb_Value(app_hb_t ch);

/** Number of times a task heartbeat went stale (A-10 companion counter). */
uint32_t App_GetTaskOvr(void);

/**
  * @brief  Create queues + tasks.  Call after HAL/peripheral bring-up and
  *         before vTaskStartScheduler().  Touches no hardware itself.
  */
void App_RtosInit(void);

/* Kernel fault hooks (vApplicationStackOverflowHook / vApplicationMallocFailedHook)
 * and vAssertCalled are defined in app_rtos.c; their prototypes come from the
 * kernel (task.h) and FreeRTOSConfig.h respectively - do not redeclare here. */

/* ---- UDS application hooks (see Protocol/uds.h) ---- */
void    App_SetLed(uint8_t on);
uint8_t App_GetLed(void);

/* Last fault location, readable in the debugger after a halt. */
extern volatile const char *g_fault_file;
extern volatile uint32_t    g_fault_line;
extern volatile const char *g_stack_ovr_task;

#endif /* __APP_RTOS_H */
