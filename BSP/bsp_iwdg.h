/**
  ******************************************************************************
  * @file    bsp_iwdg.h
  * @brief   Independent watchdog.  Both roles run it; the application loops
  *          must call BSP_IWDG_Refresh() as their liveness proof.
  * @note    Carried over from CAN v3.0, which hit and fixed a real trap here:
  *          HAL reaches the IWDG registers through Instance, so forgetting
  *          hiwdg.Instance = IWDG costs a hard fault, not a silent no-op.
  ******************************************************************************
  */
#ifndef __BSP_IWDG_H
#define __BSP_IWDG_H

#include <stdint.h>

/**
  * @brief  Start the independent watchdog.
  * @note   Call this AFTER all other peripheral init, so that init time does
  *         not eat into the timeout budget.
  * @retval 0 ok, -1 HAL error
  */
int32_t BSP_IWDG_Init(void);

/** Feed the watchdog.  Must be called periodically by each role's main loop. */
void BSP_IWDG_Refresh(void);

#endif /* __BSP_IWDG_H */
