/**
  ******************************************************************************
  * @file    bsp_tick.h
  * @brief   Microsecond time base using the Cortex-M4 DWT cycle counter.
  * @note    Gives the protocol layer a stable us-resolution clock without
  *          occupying a timer peripheral (TIM6 stays free for other tasks).
  ******************************************************************************
  */
#ifndef __BSP_TICK_H
#define __BSP_TICK_H

#include <stdint.h>

/**
  * @brief  Enable DWT cycle counter (call once after clock config).
  */
void BSP_Tick_Init(void);

/**
  * @brief  Current time in microseconds since init.
  * @note   Backed by the 32-bit DWT cycle counter; do NOT measure any
  *         single interval longer than ~20 s (the counter wraps every
  *         ~28.6 s at 150 MHz). Fine for Modbus t3.5/timeouts.
  */
uint32_t BSP_Tick_GetUs(void);

#endif /* __BSP_TICK_H */
