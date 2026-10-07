/**
  ******************************************************************************
  * @file    bsp_board_assert.h
  * @brief   Pin-ownership assertions at boot (acceptance A-09).
  ******************************************************************************
  */
#ifndef __BSP_BOARD_ASSERT_H
#define __BSP_BOARD_ASSERT_H

/**
  * @brief  Verify that every owned pin really sits in the mode/AF the driver
  *         configured (reads GPIOx->MODER/AFR and compares with the config
  *         point's macros).
  *
  *  Why: the v2.0 history contains a real field bug where a cross-port pin
  *  mask overwrote PB11 (then USART3_RX), silently killing the slave - such
  *  "register written from far away" defects are only catchable by
  *  asserting ownership at boot.
  *
  *  On mismatch: prints the actual MODER/AFR values on the console, records
  *  a LOG_SYSTEM_ERROR, then panics (IWDG then performs a controlled reset).
  *  Call once, after all drivers initialised (wdt_task, ~1 s after start).
  */
void Board_Assert_Check(void);

#endif /* __BSP_BOARD_ASSERT_H */
