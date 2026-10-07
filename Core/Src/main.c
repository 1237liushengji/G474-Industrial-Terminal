/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body - single firmware image for both roles
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
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "app_board_cfg.h"
#include "app_rtos.h"
#include "bsp_led.h"
#include "bsp_iwdg.h"
#include "bsp_tick.h"
#include "can_drv.h"

#include "FreeRTOS.h"
#include "task.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  *
  * Boot sequence (v0.2 - FreeRTOS):
  *   1. HAL_Init()        -> time base is TIM6 (stm32g4xx_hal_timebase_tim.c)
  *   2. clock + GPIO + DWT microsecond tick
  *   3. App_RtosInit()    -> queues + task set (T1/T2/T5/T6/T7 + console)
  *   4. CAN_Init()        -> the RX queue must exist before IRQs are unmasked
  *   5. IWDG last         -> wdt_task takes over feeding right after the
  *                           scheduler starts (never returns afterwards)
  *
  * Both roles are built from this one file.  The role, the form factor and
  * every pin come from App/app_board_cfg.h via the Keil target's Define field:
  *   A_GW_M1 / A_GW_M2    : NODE_ROLE=0  (Modbus master + uplink)
  *   B_NODE_M1 / B_NODE_M2: NODE_ROLE=1  (Modbus slave + local HMI)
  */
int main(void)
{
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the TIM6
   * HAL time base (SysTick belongs to the kernel once the scheduler runs). */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  /* USER CODE BEGIN 2 */
  LED_Init();
  BSP_Tick_Init();      /* DWT microsecond source (Modbus t3.5, CAN latency) */

  /* RTOS objects first: the CAN RX queue must exist before CAN interrupts
   * can fire, and the log/config queues are used by every task. */
  App_RtosInit();

  /* CAN backbone: FDCAN1 on PD0/PD1.  Its low-level init lives in
   * Core/Src/stm32g4xx_hal_msp.c (HAL_FDCAN_MspInit) because HAL_FDCAN_Init
   * depends on that callback - it is the one peripheral that does not follow
   * the "BSP self-init" convention used by the USART/SPI/I2C drivers. */
  if (CAN_Init() != HAL_OK)
  {
    Error_Handler();
  }

  /* Watchdog last, so that init time does not eat into the timeout budget.
   * From here on wdt_task owns BSP_IWDG_Refresh(). */
  if (BSP_IWDG_Init() != 0)
  {
    Error_Handler();
  }

  /* Hand over to the kernel - role bodies run inside their tasks:
   *   mb_task  -> Master_TaskMain() / Node_TaskMain()
   *   ui_task  -> Node_UiInit/Step()  (monitoring page on the gateway in v0.7)
   *   log_task -> flash log + CONFIG_Save
   *   can_task -> RX queue drain + ISO-TP/UDS; wdt_task feeds the IWDG      */
  vTaskStartScheduler();
  /* USER CODE END 2 */

  /* If the scheduler cannot start (heap exhausted), fail loudly. */
  Error_Handler();

  /* Infinite loop - unreachable */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */
    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  *
  * HSE 8 MHz -> PLL /M=2 -> 4 MHz -> xN=75 -> 300 MHz -> /R=2 -> 150 MHz.
  * Both APB1 and APB2 run at 150 MHz.  This clock tree is what every derived
  * figure in app_board_cfg.h and docs/04 §7 is based on - changing it
  * invalidates the FDCAN bit timing, the USART BRR value and the SPI divider.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV2;
  RCC_OscInitStruct.PLL.PLLN = 75;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* When the watchdog is already running this becomes a controlled reset loop
   * rather than a hang - which is the intended behaviour for both roles. */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
