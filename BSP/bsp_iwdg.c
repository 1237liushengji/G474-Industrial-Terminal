/**
  ******************************************************************************
  * @file    bsp_iwdg.c
  * @brief   Independent watchdog on the LSI: 32 kHz / 32 = 1 kHz timebase.
  ******************************************************************************
  */
#include "bsp_iwdg.h"
#include "app_board_cfg.h"

static IWDG_HandleTypeDef s_hiwdg;
static uint8_t            s_started = 0U;

int32_t BSP_IWDG_Init(void)
{
    /* Keep the watchdog running while the core is halted at a breakpoint,
     * otherwise every debug session resets the board. */
    DBGMCU->APB1FZR1 |= DBGMCU_APB1FZR1_DBG_IWDG_STOP;

    s_hiwdg.Instance       = IWDG;              /* must be set: HAL uses Instance */
    s_hiwdg.Init.Prescaler = IWDG_PRESCALER_32; /* LSI 32 kHz / 32 = 1 kHz */
    s_hiwdg.Init.Reload    = IWDG_RELOAD;       /* from app_board_cfg.h */
    s_hiwdg.Init.Window    = IWDG_WINDOW_DISABLE;

    if (HAL_IWDG_Init(&s_hiwdg) != HAL_OK)
    {
        s_started = 0U;
        return -1;
    }

    s_started = 1U;
    return 0;
}

void BSP_IWDG_Refresh(void)
{
    if (s_started != 0U)
    {
        HAL_IWDG_Refresh(&s_hiwdg);
    }
}
