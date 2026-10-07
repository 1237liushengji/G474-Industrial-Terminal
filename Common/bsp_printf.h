/**
  ******************************************************************************
  * @file    bsp_printf.h
  * @brief   Reentrant formatted output over the console UART.
  *
  *  Replaces the v3.0 single 96-byte static buffer (non-reentrant) with a
  *  per-task pool of CONSOLE_TX_POOL_NUM x CONSOLE_TX_BUF_LEN buffers plus a
  *  mutex around the actual UART write (docs/02 decision 4):
  *   - every task formats into its own slot -> no data race on the buffer
  *   - only the blocking UART write is serialized -> short critical path
  *  No floating point in format strings (docs/03 §5.2 - JSON formatting is
  *  integer based, and %f would drag the soft-float formatter into flash).
  ******************************************************************************
  */
#ifndef __BSP_PRINTF_H
#define __BSP_PRINTF_H

#include <stdint.h>

/** Create the TX mutex.  Call once before the scheduler starts. */
void BSP_Printf_Init(void);

/**
  * @brief  printf-style output to the console UART.
  * @retval number of bytes written (negative on formatting error)
  */
int BSP_Printf(const char *fmt, ...);

/** Raw string output through the same mutex. */
void BSP_Puts(const char *s);

#endif /* __BSP_PRINTF_H */
