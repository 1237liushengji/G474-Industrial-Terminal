/**
  ******************************************************************************
  * @file    mb_master_port.c
  * @brief   Gateway role (A board) - Modbus master engine, mb_task body (T2).
  *
  *  v0.7: periodic poll of the slave (0x03 read of 5 measurands).
  *  v0.8: write-through demo every N polls (0x06), fault LED indication
  *        and comm statistics tracking via the master engine.
  *        LED1 (blue): pulse per successful read transaction
  *        LED2 (green): fast blink while link is down (timeouts), solid
  *                       during exception storms (unused yet)
  *  v0.2 (FreeRTOS): split out of the old super-loop -
  *        - CAN/IsoTp/IWDG housekeeping moved to can_task / wdt_task
  *        - W25Q log init moved to log_task; events are posted, not written
  *        - HAL_Delay replaced by vTaskDelay
  ******************************************************************************
  */
#include "mb_master_port.h"

#include "bsp_led.h"
#include "can_drv.h"
#include "isotp.h"
#include "bsp_rs485.h"
#include "bsp_uart.h"
#include "bsp_tick.h"
#include "app_board_cfg.h"
#include "modbus_master.h"
#include "modbus_register.h"
#include "config.h"
#include "log.h"
#include "log_service.h"
#include "types.h"
#include "app_rtos.h"

#include "FreeRTOS.h"
#include "task.h"

#ifndef MASTER_SLAVE_ID_DEFAULT
#define MASTER_SLAVE_ID_DEFAULT  1U
#endif

/*====================================================================*/
/* Tunables                                                            */
/*====================================================================*/
#define POLL_PERIOD_MS       500U
#define MASTER_TIMEOUT_MS    100U
#define MASTER_RETRY_MAX     2U
#define WRITE_EVERY_N_POLLS  10U    /* demo: write TempLimit every 10th poll */

/*====================================================================*/
/* Local                                                                */
/*====================================================================*/
static volatile uint32_t s_poll_last_ms = 0U;
static volatile uint32_t s_fault_led_ms  = 0U;
static volatile uint32_t s_poll_count    = 0U;
static uint8_t s_target_slave = MASTER_SLAVE_ID_DEFAULT;
static comm_stats_t s_stats_disp;

static void Master_OnFinished(void);   /* fwd decl */

static TaskHandle_t s_mb_task = 0;      /* mb_task, for TX-done wakes */

static void RSCB_MasterRx(uint8_t byte)
{
    MB_Master_OnRxByte(byte, BSP_Tick_GetUs());
}

/* TC reached: bus back in RX mode; wake the engine loop (docs/02 §4.3). */
static void RSCB_MasterTxDone(void)
{
    BaseType_t woken = pdFALSE;

    if (s_mb_task != 0)
    {
        vTaskNotifyGiveFromISR(s_mb_task, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

#if (RS485_RX_MODE == 1)
/* DMA mode: whole response frames arrive from BSP_UART_RxDmaService() */
static void RSCB_MasterRxFrame(const uint8_t *frame, uint16_t len,
                               uint32_t now_us)
{
    (void)now_us;
    if (MB_Master_OnRxFrame(frame, len) != 0U)
    {
        Master_OnFinished();
    }
}
#endif

static void Master_LogEvent(uint8_t event, uint16_t param)
{
    /* Events are queued; log_task owns the blocking flash write (T5). */
    (void)LOG_SVC_PostEvent(event,
                            (uint8_t)(param & 0xFFU),
                            (uint8_t)((param >> 8) & 0xFFU), 0U);
}

static void Master_OnFinished(void)
{
    const mb_master_result_t *r = MB_Master_GetResult();

    if (r->ok)
    {
        /* payload: [byteCount][regHi regLo ...] - blink blue */
        LED1_OFF;
        LED2_OFF;
        LED1_ON;
        s_fault_led_ms = HAL_GetTick();
    }
    else if (r->timed_out != 0U)
    {
        /* link down: green fast blink via the loop below */
        LED1_OFF;
        LED2_ON;
        s_fault_led_ms = HAL_GetTick();
    }
    MB_Master_GetStats(&s_stats_disp);
}

/* execute one 0x06 demo write (also validates write path on the slave) */
static void Master_DoDemoWrite(void)
{
    static uint16_t s_demo_value = 300U;

    s_demo_value = (uint16_t)(300U + (s_poll_count % 20U) * 5U); /* 300..395 */
    MB_Master_WriteSingle(s_target_slave, 0x09 /* 40010 TempLimit */,
                          s_demo_value);
}

/*====================================================================*/
/* mb_task body (T2)                                                   */
/*====================================================================*/
void Master_TaskMain(void)
{
    config_param_t cfg;
    uint32_t baud;

    /* load own link config (both boards carry a 24C02).
     * MB_REG_Init() already ran in App_RtosInit(); boot logging is done by
     * log_task (LOG_BOOT with reset cause). */
    (void)CONFIG_Init(&cfg);
    CONFIG_ApplyToRegisters(&cfg);
    MB_REG_ClearConfigDirty();

    s_target_slave = cfg.slave_id;      /* poll this slave id */
    baud = MB_REG_BaudFromIdx(cfg.baud_idx);

    RS485_Init(baud);
    RS485_SetRxCallback(RSCB_MasterRx);
    RS485_SetTxDoneCallback(RSCB_MasterTxDone);
    s_mb_task = xTaskGetCurrentTaskHandle();
#if (RS485_RX_MODE == 1)
    RS485_SetRxFrameCallback(RSCB_MasterRxFrame);
#endif

    MB_Master_Init(s_target_slave, baud, MASTER_TIMEOUT_MS, MASTER_RETRY_MAX);
    MB_Master_SetTxFunc(RS485_SendFrame);
    MB_Master_SetEventCallback(Master_LogEvent);

    s_poll_last_ms = HAL_GetTick();
    LED1_OFF;
    LED2_OFF;

    for (;;)
    {
        if (!MB_Master_IsBusy())
        {
            if ((HAL_GetTick() - s_poll_last_ms) >= POLL_PERIOD_MS)
            {
                s_poll_last_ms = HAL_GetTick();
                s_poll_count++;
                if ((s_poll_count % WRITE_EVERY_N_POLLS) == 0U)
                {
                    Master_DoDemoWrite();      /* write transaction */
                }
                else
                {
                    /* read temperature..status (5 regs) */
                    MB_Master_ReadHolding(s_target_slave, 0x00, 5U);
                }
            }
        }

        /* protocol state machine: drives SEND + timeout/retry in both modes */
        if (MB_Master_Poll(BSP_Tick_GetUs()))
        {
            Master_OnFinished();
        }

#if (RS485_RX_MODE == 1)
        /* DMA mode: service delivers complete response frames */
        BSP_UART_RxDmaService();
#endif

        /* LED decay: blink blue = ok pulse, green fast blink = link down */
        if ((HAL_GetTick() - s_fault_led_ms) >= 100U)
        {
            LED1_OFF;
        }
        if (MB_Master_GetConsecutiveFails() >= 3U)
        {
            /* link considered down: green fast blink */
            if (((HAL_GetTick() / 120U) & 1U) != 0U)
            {
                LED2_ON;
            }
            else
            {
                LED2_OFF;
            }
        }

        App_Hb_Beat(APP_HB_MB);

        /* Sleep until an RX/TX event wakes us or 1 ms elapses (v0.1 used
         * HAL_Delay(1); the TC interrupt now shortens the TX turnaround). */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
    }
}
