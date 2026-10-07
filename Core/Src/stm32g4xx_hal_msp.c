/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file         stm32g4xx_hal_msp.c
  * @brief        This file provides code for the MSP Initialization
  *               and de-Initialization codes.
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
#include "app_board_cfg.h"
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN Define */

/* USER CODE END Define */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN Macro */

/* USER CODE END Macro */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* External functions --------------------------------------------------------*/
/* USER CODE BEGIN ExternalFunctions */

/* USER CODE END ExternalFunctions */

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */
/**
  * Initializes the Global MSP.
  */
void HAL_MspInit(void)
{

  /* USER CODE BEGIN MspInit 0 */

  /* USER CODE END MspInit 0 */

  __HAL_RCC_SYSCFG_CLK_ENABLE();
  __HAL_RCC_PWR_CLK_ENABLE();

  /* System interrupt init*/

  /** Disable the internal Pull-Up in Dead Battery pins of UCPD peripheral
  */
  HAL_PWREx_DisableUCPDDeadBattery();

  /* USER CODE BEGIN MspInit 1 */

  /* USER CODE END MspInit 1 */
}

/* USER CODE BEGIN 1 */

/**
  * @brief FDCAN low-level init: kernel clock, GPIO alternate function, NVIC.
  *        This is the ONE peripheral that is initialised through the MSP
  *        rather than self-init, because HAL_FDCAN_Init() depends on it.
  *        Every pin/instance value comes from app_board_cfg.h.
  *        Reference: STM32G474E-EVAL FDCAN_Classic_Frame_Networking example.
  */
void HAL_FDCAN_MspInit(FDCAN_HandleTypeDef* fdcanHandle)
{
  GPIO_InitTypeDef         GPIO_InitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit   = {0};

  if (fdcanHandle->Instance == CAN_PERIPH)
  {
    /* FDCAN kernel clock = PCLK1 = 150 MHz.  The bit timing in app_board_cfg.h
     * is derived from exactly this clock, so do not change one without the
     * other. */
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_FDCAN;
    PeriphClkInit.FdcanClockSelection  = RCC_FDCANCLKSOURCE_PCLK1;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
      Error_Handler();
    }

    CAN_GPIO_CLK_ENABLE();
    CAN_CLK_ENABLE();

    /* FDCAN1: PD0 = RX (to transceiver CTX), PD1 = TX (to transceiver CRX).
     * AF9 per DS12288 Rev6 Table 13, Port D. */
    GPIO_InitStruct.Pin       = CAN_RX_PIN | CAN_TX_PIN;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull      = GPIO_NOPULL;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = CAN_AF;
    HAL_GPIO_Init(CAN_RX_GPIO_PORT, &GPIO_InitStruct);

    /* Interrupt line 0.  Priority comes from the NVIC table in docs/04 §5
     * (IRQ_PRIO_CAN = (6,0)).  v3.0 ran at (1,0), which is below
     * configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (5) and therefore ILLEGAL
     * for an ISR that calls *FromISR APIs - the RX callback now posts into a
     * FreeRTOS queue, so the priority had to change.  Cost of the change is
     * re-running acceptance A-03 (frame accounting) and A-04 (bus-off) - see
     * docs/05 v0.2 and docs/02 §5 decision 2. */
    HAL_NVIC_SetPriority(CAN_IRQn, IRQ_PRIO_CAN, 0);
    HAL_NVIC_EnableIRQ(CAN_IRQn);
  }
}

/**
  * @brief FDCAN low-level de-init.
  */
void HAL_FDCAN_MspDeInit(FDCAN_HandleTypeDef* fdcanHandle)
{
  if (fdcanHandle->Instance == CAN_PERIPH)
  {
    __HAL_RCC_FDCAN_CLK_DISABLE();
    HAL_GPIO_DeInit(CAN_RX_GPIO_PORT, CAN_RX_PIN | CAN_TX_PIN);
    HAL_NVIC_DisableIRQ(CAN_IRQn);
  }
}


/* USER CODE END 1 */
