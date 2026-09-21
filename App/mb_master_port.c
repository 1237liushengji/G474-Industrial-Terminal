/**
  ******************************************************************************
  * @file    master_main.c
  * @brief   Master node (A board) application entry.
  * @note    Compiled only when MODBUS_NODE_ROLE == NODE_ROLE_MASTER.
  *
  *  v0.7: periodic poll of the slave (0x03 read of 5 measurands).
  *  v0.8: write-through demo every N polls (0x06), fault LED indication
  *        and comm statistics tracking via the master engine.
  *        LED1 (blue): pulse per successful read transaction
  *        LED2 (green): fast blink while link is down (timeouts), solid
  *                       during exception storms (unused yet)
  ******************************************************************************
  */
#include "master_main.h"

#include "led.h"
#include "bsp_rs485.h"
#include "bsp_uart.h"
#include "bsp_tick.h"
#include "bsp_board_cfg.h"
#include "bsp_w25q128.h"
#include "modbus_master.h"
#include "modbus_register.h"
#include "config.h"
#include "log.h"
#include "types.h"

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

static void RSCB_MasterRx(uint8_t byte)
{
    MB_Master_OnRxByte(byte, BSP_Tick_GetUs());
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
    LOG_WriteEvent((log_event_t)event,
                   (uint8_t)(param & 0xFFU), (uint8_t)((param >> 8) & 0xFFU), 0U);
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
        /* link down: green fast blink via main loop */
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

void Master_Main(void)
{
    config_param_t cfg;
    uint32_t baud;

    /* flash log subsystem (W25Q128) */
    (void)W25Q_Init();
    LOG_SetTickSource(HAL_GetTick);
    LOG_Init();
    LOG_WriteEvent(LOG_BOOT, 0U, 0U, 0U);

    /* load own link config (both boards carry a 24C02) */
    MB_REG_Init();
    (void)CONFIG_Init(&cfg);
    CONFIG_ApplyToRegisters(&cfg);
    MB_REG_ClearConfigDirty();

    s_target_slave = cfg.slave_id;      /* poll this slave id */
    baud = MB_REG_BaudFromIdx(cfg.baud_idx);

    RS485_Init(baud);
    RS485_SetRxCallback(RSCB_MasterRx);
#if (RS485_RX_MODE == 1)
    RS485_SetRxFrameCallback(RSCB_MasterRxFrame);
#endif

    MB_Master_Init(s_target_slave, baud, MASTER_TIMEOUT_MS, MASTER_RETRY_MAX);
    MB_Master_SetTxFunc(RS485_SendFrame);
    MB_Master_SetEventCallback(Master_LogEvent);

    s_poll_last_ms = HAL_GetTick();
    LED1_OFF;
    LED2_OFF;

    while (1)
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

        HAL_Delay(1);
    }
}
