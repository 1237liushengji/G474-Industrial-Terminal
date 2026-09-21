/**
  ******************************************************************************
  * @file    bsp_tick.c
  * @brief   DWT cycle-counter based microsecond tick.
  ******************************************************************************
  */
#include "bsp_tick.h"
#include "main.h"

static uint32_t s_cpu_mhz = 150U;   /* 150 MHz: HSE 8M /2 *75 /2 */

void BSP_Tick_Init(void)
{
    /* Enable DWT access (TRCENA in DEMCR), then start the cycle counter */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    s_cpu_mhz = (uint32_t)(SystemCoreClock / 1000000UL);
    if (s_cpu_mhz == 0U)
    {
        s_cpu_mhz = 150U;
    }
}

uint32_t BSP_Tick_GetUs(void)
{
    /* us = cycles / MHz. NOTE: DWT->CYCCNT is a 32-bit counter wrapping
       every ~2^32/SystemCoreClock (~28.6 s at 150 MHz), so the returned
       microseconds wrap non-uniformly at that boundary. All protocol
       timers use small (< few s) deltas and compare via unsigned
       subtraction, so they are safe as long as a measured interval never
       straddles the wrap; keep any single wait well below ~20 s. */
    return (uint32_t)(DWT->CYCCNT / s_cpu_mhz);
}
