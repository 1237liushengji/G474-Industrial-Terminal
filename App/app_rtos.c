/**
  ******************************************************************************
  * @file    app_rtos.c
  * @brief   RTOS bring-up, task set, heartbeats, fault hooks (v0.2, T3 in v0.4).
  *
  *  Owns the FreeRTOS objects and the task bodies that are not role specific.
  *  Role bodies live with their application code:
  *    Master_TaskMain()  App/mb_master_port.c   (gateway)
  *    Node_TaskMain()    App/node_app.c         (node)
  *    Node_UiInit/Step   App/node_app.c         (node UI, runs in ui_task)
  *    Bridge_TaskMain()  Gateway/bridge.c       (both roles, T3)
  ******************************************************************************
  */
#include "app_rtos.h"
#include "app_board_cfg.h"

#include "FreeRTOS.h"
#include "task.h"

#include "bsp_console.h"
#include "bsp_iwdg.h"
#include "bsp_led.h"
#include "bsp_printf.h"
#include "can_drv.h"
#include "can_contract.h"
#include "can_router.h"
#include "console.h"
#include "dp_table.h"
#include "isotp.h"
#include "uds.h"
#include "log_service.h"
#include "modbus_register.h"

#if ROLE_IS_GW
#include "mb_master_port.h"
#else
#include "node_app.h"
#endif
#include "bsp_board_assert.h"
#include "bridge.h"
#if ROLE_IS_GW
#include "gw_ui.h"
#endif
#include "uplink_port.h"

/*====================================================================*/
/* Task parameters (docs/02 §3)                                       */
/*====================================================================*/
#define STACK_WDT        256U
#define STACK_CAN        512U
#define STACK_MB         512U
#define STACK_BRIDGE     512U
#define STACK_LOG        512U
#define STACK_UI        1024U
#define STACK_UPLINK    1024U
#define STACK_CONSOLE    512U

#define PRIO_WDT           6
#define PRIO_CAN           5
#define PRIO_MB            5
#define PRIO_BRIDGE        4
#define PRIO_UPLINK        3
#define PRIO_LOG           2
#define PRIO_UI            1
#define PRIO_CONSOLE       2

/*====================================================================*/
/* Heartbeats                                                          */
/*====================================================================*/
static volatile uint32_t s_hb[APP_HB_COUNT];
static volatile uint32_t s_task_ovr = 0U;

void App_Hb_Beat(app_hb_t ch)
{
    if ((uint32_t)ch < (uint32_t)APP_HB_COUNT)
    {
        s_hb[ch]++;
    }
}

uint32_t App_Hb_Value(app_hb_t ch)
{
    return ((uint32_t)ch < (uint32_t)APP_HB_COUNT) ? s_hb[ch] : 0U;
}

uint32_t App_GetTaskOvr(void)
{
    return s_task_ovr;
}

/*====================================================================*/
/* Debug counters (console 'stats')                                    */
/*====================================================================*/
uint32_t g_dbgPoll;
uint32_t g_dbgHx;
uint32_t g_dbgCmd;
uint32_t g_dbgResp;

/*====================================================================*/
/* CAN dispatch (can_task context; ISR never parses - docs/02 §5)      */
/*====================================================================*/
static void App_CanDispatch(const CAN_RxMsg *m)
{
    g_dbgPoll++;
    g_dbgHx++;

    if (m->id == CANC_ID_CMD)
    {
        g_dbgCmd++;
    }
    if (m->id == CANC_ID_CMD_ACK)
    {
        g_dbgResp++;
    }

    if (Console_SniffEnabled() != 0U)
    {
        Console_PrintFrame(m);
    }

    /* v0.4: table-driven routing (Gateway/can_router.c).  The ISO-TP
     * channels (UDS + snapshot) are rows of the same table and self-filter
     * by rxId - all protocol handling stays in task context. */
    CanRouter_Dispatch(m);
}

/*====================================================================*/
/* T1 - can_task                                                       */
/*====================================================================*/
static void CanTask(void *arg)
{
    CAN_RxMsg msg;

    (void)arg;

    Uds_Init();          /* ISO-TP ids + role callbacks (server / client) */

    for (;;)
    {
        /* Block on the RX queue: an ISR post is the "notification" (docs/02 T1) */
        if (CAN_WaitRx(&msg, 10U) != 0U)
        {
            do
            {
                App_CanDispatch(&msg);
            } while (CAN_PollRx(&msg));
        }

        CAN_Task();      /* bus-off exponential backoff housekeeping */
        Uds_Task();      /* ISO-TP 节拍 + S3 session rollback + client timeout */

        App_Hb_Beat(APP_HB_CAN);
    }
}

/*====================================================================*/
/* T2 - mb_task (role body owns the loop; never returns)               */
/*====================================================================*/
static void MbTask(void *arg)
{
    (void)arg;

#if ROLE_IS_GW
    Master_TaskMain();
#else
    Node_TaskMain();
#endif
}

/*====================================================================*/
/* T3 - bridge_task (role body owns the loop; never returns)           */
/*====================================================================*/
static void BridgeTask(void *arg)
{
    (void)arg;

    /* CAN <-> register bridge + snapshot channel (Gateway/bridge.c) */
    Bridge_TaskMain();
}

/*====================================================================*/
/* T4 - uplink_task (gateway only in v1: M1 frames / M2 MQTT-C)        */
/*====================================================================*/
static void UplinkTask(void *arg)
{
    (void)arg;

    Uplink_TaskMain();   /* Gateway/uplink_port.c; never returns */
}

/*====================================================================*/
/* T5 - log_task                                                       */
/*====================================================================*/
static void LogTask(void *arg)
{
    (void)arg;

    LOG_SVC_RunInit();                  /* blocking flash scan, by design */
    App_Hb_Beat(APP_HB_LOG);

    for (;;)
    {
        LOG_SVC_Process();
        App_Hb_Beat(APP_HB_LOG);
    }
}

/*====================================================================*/
/* T6 - ui_task                                                        */
/*====================================================================*/
static void UiTask(void *arg)
{
    (void)arg;

#if ROLE_IS_NODE
    Node_UiInit();

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(5));   /* key scan cadence (v0.1 was ~2 ms) */
        Node_UiStep();
        App_Hb_Beat(APP_HB_UI);
    }
#else
    /* Gateway monitoring page (v0.7): link bits, CAN/MB/MQTT counters,
     * backfill depth, snapshot age.  K0 = manual snapshot, K1 = manual
     * backfill sweep. */
    Gw_UiInit();

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(5));
        Gw_UiStep();
        App_Hb_Beat(APP_HB_UI);
    }
#endif
}

/*====================================================================*/
/* console_task (transitional; v0.5 folds it into the uplink task)     */
/*====================================================================*/
static void ConsoleTask(void *arg)
{
    (void)arg;

    BSP_Console_Init();
    Console_Init();

    for (;;)
    {
        Console_Task();
        vTaskDelay(pdMS_TO_TICKS(10));
        App_Hb_Beat(APP_HB_CONSOLE);
    }
}

/*====================================================================*/
/* T7 - wdt_task                                                       */
/*====================================================================*/
typedef struct
{
    app_hb_t ch;
    uint32_t deadline_ms;   /* stale after this long without a beat */
} hb_watch_t;

static const hb_watch_t s_watch[] =
{
    { APP_HB_CAN,     300U },   /* 10 ms loop                     */
    { APP_HB_MB,      300U },   /* 1 ms loop                      */
    { APP_HB_BRIDGE,  300U },   /* 10 ms loop                     */
#if ROLE_IS_GW
    { APP_HB_UPLINK, 3000U },   /* 100 ms loop; M2 will block in AT/MQTT */
#endif
    { APP_HB_LOG,    4000U },   /* flash erase worst case ~400 ms */
    { APP_HB_UI,     2500U },   /* 500 ms refresh + full clear    */
    { APP_HB_CONSOLE,3000U },   /* 10 ms loop                     */
};

static void WdtTask(void *arg)
{
    uint32_t prev[APP_HB_COUNT];
    uint32_t last_seen[APP_HB_COUNT];
    uint32_t now;
    uint32_t i;

    (void)arg;

    {
        uint8_t asserted = 0U;

        now = xTaskGetTickCount();
        for (i = 0U; i < (uint32_t)APP_HB_COUNT; i++)
        {
            prev[i] = s_hb[i];
            last_seen[i] = now;
        }

    for (;;)
    {
        uint32_t all_ok = 1U;

        vTaskDelay(pdMS_TO_TICKS(20));
        now = xTaskGetTickCount();

        /* one-shot pin-ownership assertion (A-09): ~1.2 s after start every
         * driver has configured its pins (RS485 in mb task, LCD in ui task,
         * ESP in uplink task) - checking later avoids false positives. */
        if ((asserted == 0U) && (now > 1200U))
        {
            asserted = 1U;
            Board_Assert_Check();
        }

        for (i = 0U; i < (uint32_t)(sizeof(s_watch) / sizeof(s_watch[0])); i++)
        {
            app_hb_t ch = s_watch[i].ch;

            if (s_hb[ch] != prev[ch])
            {
                prev[ch] = s_hb[ch];
                last_seen[ch] = now;
            }
            else if ((now - last_seen[ch]) > s_watch[i].deadline_ms)
            {
                all_ok = 0U;
                s_task_ovr++;
            }
        }

        if (all_ok != 0U)
        {
            BSP_IWDG_Refresh();
        }
        /* else: deliberately stop feeding -> IWDG resets in <= 3 s and the
         * next boot records the cause in LOG_BOOT (docs/05 fault matrix:
         * blocking a task 5 s must reset the board, not hang it). */
    }
    }
}

/*====================================================================*/
/* Fault hooks                                                         */
/*====================================================================*/
volatile const char *g_fault_file;
volatile uint32_t    g_fault_line;
volatile const char *g_stack_ovr_task;

void vAssertCalled(const char *const file, int line)
{
    g_fault_file = file;
    g_fault_line = (uint32_t)line;
    taskDISABLE_INTERRUPTS();
    for (;;)
    {
    }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    g_stack_ovr_task = pcTaskName;
    taskDISABLE_INTERRUPTS();
    for (;;)
    {
    }
}

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    for (;;)
    {
    }
}

/*====================================================================*/
/* UDS application hooks (LED)                                         */
/*====================================================================*/
/* Debug-only LED output for the UDS server (0x2E F010).  It shares LED1
 * with the master's link pulse; the LED semantics are reworked in v0.7
 * together with the UI layer, so no arbitration is attempted here. */
static uint8_t s_uds_led = 0U;

void App_SetLed(uint8_t on)
{
    s_uds_led = (on != 0U) ? 1U : 0U;
    if (s_uds_led != 0U)
    {
        LED1_ON;
    }
    else
    {
        LED1_OFF;
    }
}

uint8_t App_GetLed(void)
{
    return s_uds_led;
}

/*====================================================================*/
/* Bring-up                                                            */
/*====================================================================*/
void App_RtosInit(void)
{
    /* RAM-only pieces first: the register bank must be consistent before any
     * task reads it (UI, console stats).  Hardware init stays in the owning
     * task (mb task: 24C02/RS485; log task: W25Q; ui task: LCD/keys). */
    MB_REG_Init();

    /* Contract metadata (docs/03 §3.2, 0x18-0x1B): the hash is computed from
     * Common/dp_table.c - the compile-time table that IS the contract.  The
     * same hash is emitted in MQTT meta."ch" and the ISO-TP snapshot header;
     * acceptance A-05 compares all three against Tools/python/dp_table.json. */
    MB_REG_SetHolding(MB_REG_HOLD_CONTRACT_HASH, DP_Table_ContractHash());
    MB_REG_SetHolding(MB_REG_HOLD_CONTRACT_VER,  CONTRACT_VERSION);
    MB_REG_SetHolding(MB_REG_HOLD_FORM_MODE,     FORM_MODE);
    MB_REG_SetHolding(MB_REG_HOLD_FW_VER,        FW_VERSION_BCD);

    LOG_SVC_Init();
    BSP_Printf_Init();
    Uplink_Init();

    (void)xTaskCreate(WdtTask,     "wdt", STACK_WDT,     0, PRIO_WDT,     0);
    (void)xTaskCreate(CanTask,     "can", STACK_CAN,     0, PRIO_CAN,     0);
    (void)xTaskCreate(MbTask,      "mb",  STACK_MB,      0, PRIO_MB,      0);
    (void)xTaskCreate(BridgeTask,  "brg", STACK_BRIDGE,  0, PRIO_BRIDGE,  0);
#if ROLE_IS_GW
    (void)xTaskCreate(UplinkTask,  "upl", STACK_UPLINK,  0, PRIO_UPLINK,  0);
#endif
    (void)xTaskCreate(LogTask,     "log", STACK_LOG,     0, PRIO_LOG,     0);
    (void)xTaskCreate(UiTask,      "ui",  STACK_UI,      0, PRIO_UI,      0);
    (void)xTaskCreate(ConsoleTask, "con", STACK_CONSOLE, 0, PRIO_CONSOLE, 0);
}
