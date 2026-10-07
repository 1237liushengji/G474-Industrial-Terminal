/**
  ******************************************************************************
  * @file    bsp_led.c
  * @brief   User status LEDs.  Initial state is OFF (does not light up at boot).
  ******************************************************************************
  */
#include "bsp_led.h"

void LED_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    LED_GPIO_CLK_ENABLE();

    /* Drive both pins high (active low -> OFF) before switching to output,
     * so no glitch is visible while the outputs are being enabled. */
    LED1_OFF;
    LED2_OFF;

    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;

    gpio.Pin = LED1_PIN;
    HAL_GPIO_Init(LED1_PORT, &gpio);

    gpio.Pin = LED2_PIN;
    HAL_GPIO_Init(LED2_PORT, &gpio);
}
