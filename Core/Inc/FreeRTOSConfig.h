/**
  ******************************************************************************
  * @file    FreeRTOSConfig.h
  * @brief   FreeRTOS kernel configuration - the single RTOS configuration point.
  *
  *  Targets: A_GW_M1 / A_GW_M2 / B_NODE_M1 / B_NODE_M2  (armclang V6.14)
  *  Kernel : FreeRTOS-Kernel V10.5.1, port portable/GCC/ARM_CM4F
  *
  *  Key decisions (docs/02 §5):
  *   - configTICK_RATE_HZ 1000; SysTick belongs to the kernel
  *   - configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5: any ISR that calls
  *     a *FromISR API must have a preemption priority numerically >= 5.
  *     NVIC table: FDCAN1_IT0=(6,0) USART1/2/3=(7,0) TIM6=(8,0) - see
  *     App/app_board_cfg.h §13 and docs/04 §5.
  *   - HAL time base runs on TIM6 (stm32g4xx_hal_timebase_tim.c), NOT SysTick,
  *     so HAL_GetTick() and xTaskGetTickCount() stay independent (acceptance A-02).
  *   - heap_4 with a per-role size: 20 KB gateway / 18 KB node (docs/04 §6).
  ******************************************************************************
  */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/*---------------------------------------------------------------------------
 * Scheduler
 *-------------------------------------------------------------------------*/
#define configUSE_PREEMPTION                     1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  0
#define configUSE_TICKLESS_IDLE                  0
#define configCPU_CLOCK_HZ                       ( 150000000UL )
#define configTICK_RATE_HZ                       ( 1000U )
#define configMAX_PRIORITIES                     ( 8 )
#define configMINIMAL_STACK_SIZE                 ( 128U )
#define configMAX_TASK_NAME_LEN                  ( 12U )
#define configUSE_16_BIT_TICKS                   0
#define configIDLE_SHOULD_YIELD                  1
#define configUSE_TIME_SLICING                   1
#define configUSE_NEWLIB_REENTRANT               0
#define configENABLE_BACKWARD_COMPATIBILITY      0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS  0
#define configSTACK_DEPTH_TYPE                   uint16_t
#define configMESSAGE_BUFFER_LENGTH_TYPE         size_t

/*---------------------------------------------------------------------------
 * Memory allocation (heap_4)
 *-------------------------------------------------------------------------*/
#define configSUPPORT_STATIC_ALLOCATION          0
#define configSUPPORT_DYNAMIC_ALLOCATION         1
#if defined( NODE_ROLE ) && ( NODE_ROLE == 1 )
  #define configTOTAL_HEAP_SIZE                  ( 18U * 1024U )  /* node */
#else
  #define configTOTAL_HEAP_SIZE                  ( 20U * 1024U )  /* gateway */
#endif
#define configAPPLICATION_ALLOCATED_HEAP         0

/*---------------------------------------------------------------------------
 * Hooks
 *-------------------------------------------------------------------------*/
#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configCHECK_FOR_STACK_OVERFLOW           2
#define configUSE_MALLOC_FAILED_HOOK             1
#define configUSE_DAEMON_TASK_STARTUP_HOOK       0

/*---------------------------------------------------------------------------
 * Runtime stats / trace (off: RAM and flash are kept for the application)
 *-------------------------------------------------------------------------*/
#define configGENERATE_RUN_TIME_STATS            0
#define configUSE_TRACE_FACILITY                 0
#define configUSE_STATS_FORMATTING_FUNCTIONS     0

/*---------------------------------------------------------------------------
 * Synchronisation primitives actually used (keep the rest out to save space)
 *-------------------------------------------------------------------------*/
#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              0
#define configUSE_COUNTING_SEMAPHORES            0
#define configUSE_QUEUE_SETS                     0
#define configUSE_TASK_NOTIFICATIONS             1
#define configQUEUE_REGISTRY_SIZE                0
#define configUSE_TIMERS                         0

/*---------------------------------------------------------------------------
 * API inclusion (only what the application calls)
 *-------------------------------------------------------------------------*/
#define INCLUDE_vTaskPrioritySet                 0
#define INCLUDE_uxTaskPriorityGet                0
#define INCLUDE_vTaskDelete                      0
#define INCLUDE_vTaskSuspend                     0
#define INCLUDE_vTaskDelay                       1
#define INCLUDE_vTaskDelayUntil                  1
#define INCLUDE_xTaskGetCurrentTaskHandle        1
#define INCLUDE_uxTaskGetStackHighWaterMark      1
#define INCLUDE_xTaskGetSchedulerState           1
#define INCLUDE_xTaskGetIdleTaskHandle           0
#define INCLUDE_eTaskGetState                    0
#define INCLUDE_xTaskAbortDelay                  0

/*---------------------------------------------------------------------------
 * Cortex-M interrupt priority configuration (4 priority bits, group 4)
 *-------------------------------------------------------------------------*/
#define configPRIO_BITS                          4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY          15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY     5

#define configKERNEL_INTERRUPT_PRIORITY \
    ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )

/*---------------------------------------------------------------------------
 * The port owns SVC/PendSV/SysTick; map them onto the CMSIS vector names.
 * With these three defines present, Core/Src/stm32g4xx_it.c must NOT define
 * those handlers (port.c does, under the renamed symbols).
 *-------------------------------------------------------------------------*/
#define vPortSVCHandler                          SVC_Handler
#define xPortPendSVHandler                       PendSV_Handler
#define xPortSysTickHandler                      SysTick_Handler

/*---------------------------------------------------------------------------
 * Assert: record file/line for the debugger, then stop.  With IWDG running
 * (fed only by wdt_task) a stopped scheduler turns into a controlled reset,
 * and the recorded location survives in RAM until the debugger reads it.
 *-------------------------------------------------------------------------*/
void vAssertCalled( const char * const file, int line );
#define configASSERT( x ) \
    if( ( x ) == 0 ) { vAssertCalled( __FILE__, __LINE__ ); }

#endif /* FREERTOS_CONFIG_H */
