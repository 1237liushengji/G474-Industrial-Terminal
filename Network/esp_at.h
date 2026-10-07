/**
  ******************************************************************************
  * @file    esp_at.h
  * @brief   ESP-12F AT link driver + connection state machine (M2, v0.6).
  *
  *  Hardware: LPUART1 on PB10(RX)/PB11(TX) (app_board_cfg §4) - the Wi-Fi
  *  jumper caps connect the on-board ESP8266 to this pair; PB10/PB11 are
  *  3.3 V only (risk R-8).  The 485 caps use the SAME pads with the mirror
  *  direction (USART3) - fit one group only (docs/迁移说明 §17).
  *
  *  State machine (docs/02 decision 5):
  *    DOWN -(backoff)-> RESET -> AT -> CWMODE -> JOIN -> CIPMUX -> CIPMODE
  *         -> OPEN(TCP) -> CIPSEND'>' -> PASS_THRU -(MQTT error)->
  *         at_window(+++) -> DOWN (backoff 1->2->...->30 s, shared sequence)
  *
  *  Esp_Service() is called from uplink_task (T4).  Its AT steps block with
  *  BOUNDED timeouts (RESET ~3 s, JOIN ~20 s); during every wait it beats the
  *  T4 heartbeat so the watchdog sees progress - long ESP operations are
  *  legitimate work, not a stall.
  ******************************************************************************
  */
#ifndef __ESP_AT_H
#define __ESP_AT_H

#include <stdint.h>

/** Bring up LPUART1 + RX ring (call once, before the scheduler is fine). */
void Esp_Init(void);

/** Drive the state machine; @retval 1 = pass-through active (link ready). */
int Esp_Service(uint32_t now);

/** 1 while the transparent (pass-through) link is up. */
uint8_t Esp_IsReady(void);

/** Force the link down after an MQTT-level failure (applies the backoff). */
void Esp_LinkLost(void);

/** Raw pass-through bytes (MQTT-C PAL uses these two). */
void     Esp_WriteBytes(const uint8_t *data, uint16_t len);
uint16_t Esp_Read(uint8_t *dst, uint16_t max);
uint16_t Esp_RxAvailable(void);
void     Esp_FlushRx(void);

/** Reconnect counter for the status topic / console. */
uint32_t Esp_GetReconnectCount(void);

#endif /* __ESP_AT_H */
