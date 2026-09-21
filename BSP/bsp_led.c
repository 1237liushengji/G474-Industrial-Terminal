#include "led.h"  

void LED_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	__HAL_RCC_LED1_CLK_ENABLE;			// GPIOE 时钟使能
	__HAL_RCC_LED2_CLK_ENABLE;			// 同属 GPIOE，再使能一次（无害）

	HAL_GPIO_WritePin(LED1_PORT, LED1_PIN, GPIO_PIN_RESET);		// 低电平点亮 LED1
	HAL_GPIO_WritePin(LED2_PORT, LED2_PIN, GPIO_PIN_RESET);		// 低电平点亮 LED2

	GPIO_InitStruct.Mode 	= GPIO_MODE_OUTPUT_PP;	// 推挽输出
	GPIO_InitStruct.Pull 	= GPIO_NOPULL;			// 无上下拉
	GPIO_InitStruct.Speed 	= GPIO_SPEED_FREQ_LOW;	// 低速

	GPIO_InitStruct.Pin 	= LED1_PIN;
	HAL_GPIO_Init(LED1_PORT, &GPIO_InitStruct);

	GPIO_InitStruct.Pin 	= LED2_PIN;
	HAL_GPIO_Init(LED2_PORT, &GPIO_InitStruct);
}

