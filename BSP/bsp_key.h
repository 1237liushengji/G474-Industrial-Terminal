/**
  ******************************************************************************
  * @file    bsp_key.h
  * @brief   Button driver (WKUP=PA0, K0=PA2, K1=PA3) - polling, debounce,
  *          short press / long press events.
  ******************************************************************************
  */
#ifndef __BSP_KEY_H
#define __BSP_KEY_H

#include <stdint.h>

typedef enum {
    KEY_WKUP = 0,   /* confirm / save   */
    KEY_K0,         /* next item        */
    KEY_K1,         /* modify           */
    KEY_COUNT
} key_id_t;

typedef enum {
    KEY_EVENT_NONE = 0,
    KEY_EVENT_SHORT,    /* released before long-press time   */
    KEY_EVENT_LONG      /* held longer than threshold        */
} key_event_t;

/**
  * @brief  Init key GPIOs (pull-up inputs; keys active low).
  */
void KEY_Init(void);

/**
  * @brief  Poll all keys; call every ~5-10 ms from the main loop.
  * @param  key  [out] key that generated the event
  * @retval KEY_EVENT_NONE / SHORT / LONG
  */
key_event_t KEY_Scan(key_id_t *key);

#endif /* __BSP_KEY_H */
