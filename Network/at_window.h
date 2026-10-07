/**
  ******************************************************************************
  * @file    at_window.h
  * @brief   ESP pass-through escape window (risk R-3, docs/02 decision 5).
  ******************************************************************************
  */
#ifndef __AT_WINDOW_H
#define __AT_WINDOW_H

#include <stdint.h>

/** Quiet guard before "+++" (docs: >= 21 bit times; 10 ms used). */
#define AT_WINDOW_GUARD_MS   10U
/** How long to wait for the "OK" after "+++". */
#define AT_WINDOW_OK_MS      1000U

/**
  * @brief  Leave the ESP transparent mode: silence guard -> "+++" -> wait OK.
  * @retval 1 escaped cleanly (link may be reused), 0 = no OK within timeout
  *         (caller must treat the link as dead and go through RECOVER).
  * @note   Called from uplink_task only, typically after an MQTT-level error.
  */
uint8_t AtWindow_ExitPassThrough(void);

#endif /* __AT_WINDOW_H */
