/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32g4xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32g4xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "app_board_cfg.h"
#include "bsp_console.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/*============================================================================
 * Fault forensics (v0.2)
 *
 * The stacked register values and fault status words are kept in RAM so a
 * debugger session can read them after the fault; a compact hex line is also
 * pushed to the console when USART1 is already up.  Keeping the core halted
 * hands the reset to the IWDG (wdt_task stops feeding) -> controlled reset,
 * see docs/04 §5 "HardFault" row.
 *==========================================================================*/
volatile uint32_t g_fault_cfsr;
volatile uint32_t g_fault_hfsr;
volatile uint32_t g_fault_pc;
volatile uint32_t g_fault_lr;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
static void Fault_Dump(const char *tag, uint32_t cfsr, uint32_t hfsr,
                       uint32_t pc, uint32_t lr);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* LR register read: CMSIS header used here does not expose __get_LR under
 * armclang, so the few faults that need EXC_RETURN read it directly. */
static uint32_t Fault_ReadLR(void)
{
  uint32_t lr;
  __asm volatile ("MOV %0, LR" : "=r" (lr));
  return lr;
}

static uint32_t HexStr(uint32_t v, char *out)
{
  static const char digits[] = "0123456789ABCDEF";
  int i;
  int n = 0;

  out[n++] = '0';
  out[n++] = 'x';
  for (i = 7; i >= 0; i--)
  {
    out[n++] = digits[(v >> (uint32_t)(i * 4)) & 0xFU];
  }
  return (uint32_t)n;
}

static void Fault_Dump(const char *tag, uint32_t cfsr, uint32_t hfsr,
                       uint32_t pc, uint32_t lr)
{
  char buf[96];
  uint32_t n = 0;
  uint32_t len = 0;

  /* Console TX polls TXE - if USART1 was never initialised (fault during
   * early boot) that poll would never finish.  Only print once it is up;
   * the RAM words above stay readable in the debugger regardless. */
  if (BSP_Console_IsReady() == 0U)
  {
    return;
  }

  while (tag[len] != '\0')
  {
    buf[n++] = tag[len++];
  }
  buf[n++] = ' ';
  n += HexStr(pc, &buf[n]);
  buf[n++] = ' '; buf[n++] = 'L'; buf[n++] = 'R'; buf[n++] = '=';
  n += HexStr(lr, &buf[n]);
  buf[n++] = ' '; buf[n++] = 'C'; buf[n++] = 'F';
  buf[n++] = 'S'; buf[n++] = 'R'; buf[n++] = '=';
  n += HexStr(cfsr, &buf[n]);
  buf[n++] = ' '; buf[n++] = 'H';
  buf[n++] = 'F'; buf[n++] = 'S'; buf[n++] = 'R'; buf[n++] = '=';
  n += HexStr(hfsr, &buf[n]);
  buf[n++] = '\r'; buf[n++] = '\n';

  BSP_Console_Send((const uint8_t *)buf, (uint16_t)n);
}

/* USER CODE END 0 */

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/*                                                                            */
/*  NOTE: SVC_Handler / PendSV_Handler / SysTick_Handler are NOT defined      */
/*  here.  FreeRTOSConfig.h maps vPortSVCHandler / xPortPendSVHandler /       */
/*  xPortSysTickHandler onto those vector names and the FreeRTOS port         */
/*  (Middlewares/FreeRTOS-Kernel/portable/GCC/ARM_CM4F/port.c) provides the   */
/*  actual bodies.  Defining them here as well would be a duplicate symbol.   */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */
  /* Recover the stack frame that was active when the fault hit:
   * EXC_RETURN bit2 = 0 -> MSP was in use, 1 -> PSP (RTOS task context).
   * Frame layout: r0 r1 r2 r3 r12 lr pc xpsr.                        */
  {
    uint32_t lr = Fault_ReadLR();
    uint32_t *sp = ((lr & 0x4U) != 0U) ? (uint32_t *)__get_PSP()
                                       : (uint32_t *)__get_MSP();

    g_fault_pc   = sp[6];
    g_fault_lr   = sp[5];
  }
  g_fault_cfsr = SCB->CFSR;
  g_fault_hfsr = SCB->HFSR;

  Fault_Dump("HF", g_fault_cfsr, g_fault_hfsr, g_fault_pc, g_fault_lr);
  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */
  g_fault_cfsr = SCB->CFSR;
  g_fault_hfsr = SCB->HFSR;
  g_fault_pc   = 0U;
  g_fault_lr   = Fault_ReadLR();
  Fault_Dump("MMF", g_fault_cfsr, g_fault_hfsr, g_fault_pc, g_fault_lr);
  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Prefetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */
  g_fault_cfsr = SCB->CFSR;
  g_fault_hfsr = SCB->HFSR;
  g_fault_pc   = 0U;
  g_fault_lr   = Fault_ReadLR();
  Fault_Dump("BF", g_fault_cfsr, g_fault_hfsr, g_fault_pc, g_fault_lr);
  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */
  g_fault_cfsr = SCB->CFSR;
  g_fault_hfsr = SCB->HFSR;
  g_fault_pc   = 0U;
  g_fault_lr   = Fault_ReadLR();
  Fault_Dump("UF", g_fault_cfsr, g_fault_hfsr, g_fault_pc, g_fault_lr);
  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/******************************************************************************/
/* STM32G4xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32g4xx.s).                    */
/******************************************************************************/

/* USER CODE BEGIN 1 */

/**
  * @brief TIM6 global interrupt: the HAL 1 ms time base (stm32g4xx_hal_timebase_tim.c).
  * @note  Shared vector with DAC - only TIM6 is enabled on this project.
  *        URS=1 makes an overflow the only UIF source; write-0-clears-it
  *        semantics: clearing by writing ~UIF is the RM-recommended pattern.
  */
void TIM6_DAC_IRQHandler(void)
{
  if ((TIM6->SR & TIM_SR_UIF) != 0U)
  {
    TIM6->SR = ~TIM_SR_UIF;
    HAL_IncTick();
  }
}

/* USER CODE END 1 */
