/**
  ******************************************************************************
  * @file    stm32g4xx_hal_timebase_tim.c
  * @brief   HAL time base moved from SysTick to TIM6.
  *
  *  Why (docs/02 §5 decision 3 - "three time bases, no substitution"):
  *   - SysTick belongs to FreeRTOS (1 ms kernel tick).
  *   - HAL_GetTick() must keep advancing independently for HAL driver timeouts
  *     (FDCAN / USART / SPI).  It is driven by TIM6 at 1 ms.
  *   - BSP_Tick_GetUs() (DWT) stays the microsecond source for Modbus t3.5.
  *
  *  Implementation notes:
  *   - Register level on purpose: the HAL_TIM module stays disabled in
  *     stm32g4xx_hal_conf.h, so this file must not pull in HAL_TIM.
  *   - TIM6 clock = PCLK1 = 150 MHz; PSC computed at run time so the file
  *     survives a clock-tree change that goes through HAL_RCC_ClockConfig
  *     (the HAL re-invokes HAL_InitTick() after a clock change).
  *   - Priority is fixed at IRQ_PRIO_TIMEBASE (8,0), NOT the TickPriority
  *     argument: the NVIC table in docs/04 §5 is authoritative, and the
  *     default HAL TICK_INT_PRIORITY (15) would be "legal but wrong" here.
  *   - URS=1: a counter overflow is the only event that sets UIF, so the
  *     UG reload pulse cannot re-trigger a spurious interrupt.
  *
  *  The matching ISR (TIM6_DAC_IRQHandler -> HAL_IncTick) lives in
  *  Core/Src/stm32g4xx_it.c because the TIM6_DAC vector is shared with DAC.
  ******************************************************************************
  */
#include "main.h"
#include "app_board_cfg.h"

/** @brief (Re)configure TIM6 as the 1 ms HAL tick source. */
static void TIM6_TimeBase_Config(void)
{
  uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
  uint32_t psc   = (pclk1 / 1000000U);

  if (psc == 0U)
  {
    psc = 1U;                       /* defensive; never expected at 150 MHz */
  }

  __HAL_RCC_TIM6_CLK_ENABLE();

  TIM6->CR1  = 0U;                  /* stop while reconfiguring */
  TIM6->PSC  = (uint16_t)(psc - 1U);        /* counter clock = 1 MHz     */
  TIM6->ARR  = 1000U - 1U;                  /* 1 MHz / 1000 = 1 kHz (1 ms) */
  TIM6->EGR  = TIM_EGR_UG;                  /* load PSC/ARR immediately  */
  TIM6->SR   = 0U;                          /* clear the UG-induced UIF  */
  TIM6->DIER = TIM_DIER_UIE;
  TIM6->CR1  = TIM_CR1_URS;                 /* only overflow sets UIF    */
  TIM6->CR1 |= TIM_CR1_CEN;

  HAL_NVIC_SetPriority(TIM6_DAC_IRQn, IRQ_PRIO_TIMEBASE, 0U);
  HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
}

/**
  * @brief  Overrides the __weak HAL_InitTick.  Called from HAL_Init() and
  *         again from HAL_RCC_ClockConfig() (with uwTickPrio).  Idempotent.
  */
HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
  (void)TickPriority;               /* NVIC table wins - see file header */
  TIM6_TimeBase_Config();
  uwTickPrio = IRQ_PRIO_TIMEBASE;
  return HAL_OK;
}

/**
  * @brief  Suspend the tick (HAL low-power entry points).  Kept functional so
  *         no caller runs into a permanently frozen HAL_GetTick().
  */
void HAL_SuspendTick(void)
{
  TIM6->DIER &= ~TIM_DIER_UIE;
}

/**
  * @brief  Resume the tick.
  */
void HAL_ResumeTick(void)
{
  TIM6->DIER |= TIM_DIER_UIE;
}
