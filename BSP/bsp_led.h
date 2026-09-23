/**
  ******************************************************************************
  * @file    bsp_led.h
  * @brief   User status LEDs (active low).  Pins live in app_board_cfg.h only.
  * @note    Board fact: only ONE user LED is populated (the blue one).  The
  *          white LED on the board is a hard-wired power indicator.
  *
  *          All writes go through BSRR, never through an ODR read-modify-write.
  *          CAN v3.0 chased a phantom "LED freeze" caused by HAL_GPIO_TogglePin
  *          (GPIOx->ODR ^= pin) losing writes under sniff load - see commit
  *          26745d8.  LED_TOGGLE below still reads ODR to pick a direction but
  *          performs the actual drive with a single atomic BSRR write, so a
  *          stale read can at worst invert one toggle - it can never clobber
  *          another pin's state nor silently drop the write.
  *
  *          Call style is object-like macros (used as `LED1_ON;`) to stay
  *          source-compatible with the existing application code.
  ******************************************************************************
  */
#ifndef __BSP_LED_H
#define __BSP_LED_H

#include "main.h"
#include "app_board_cfg.h"

/* LED_PORT / LED1_PIN / LED2_PIN come from app_board_cfg.h */
#define LED1_PORT           LED_PORT
#define LED2_PORT           LED_PORT

#define LED_SET(pin)        ((LED_PORT)->BSRR = (uint32_t)(pin))
#define LED_CLR(pin)        ((LED_PORT)->BSRR = (uint32_t)(pin) << 16U)

#define LED_TOGGLE(pin)     do {                                              \
                              if (((LED_PORT)->ODR & (pin)) != 0U) {          \
                                LED_CLR(pin);   /* was off -> drive low  */   \
                              } else {                                        \
                                LED_SET(pin);   /* was on  -> drive high */    \
                              }                                               \
                            } while (0)

/* Active low: driving the pin low turns the LED on. */
#define LED1_ON             LED_CLR(LED1_PIN)
#define LED1_OFF            LED_SET(LED1_PIN)
#define LED1_Toggle         LED_TOGGLE(LED1_PIN)

#define LED2_ON             LED_CLR(LED2_PIN)
#define LED2_OFF            LED_SET(LED2_PIN)
#define LED2_Toggle         LED_TOGGLE(LED2_PIN)

/** Configure both LED pins as push-pull outputs, both OFF on entry. */
void LED_Init(void);

#endif /* __BSP_LED_H */
