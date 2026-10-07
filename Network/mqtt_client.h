/**
  ******************************************************************************
  * @file    mqtt_client.h
  * @brief   MQTT-C client service (M2, v0.6): PAL + session management.
  ******************************************************************************
  */
#ifndef __MQTT_CLIENT_H
#define __MQTT_CLIENT_H

#include <stdint.h>

/** Prepare buffers (no I/O).  Call before the scheduler. */
void Mqtt_Init(void);

/** Drive ESP state machine + MQTT sync; called from uplink_task every 100 ms. */
void Mqtt_Service(uint32_t now);

/** 1 = broker session established (CONNACK seen). */
uint8_t Mqtt_IsConnected(void);

/**
  * @brief  Queue one publish (QoS/retain per Tools/mqtt/mqtt_topics.md).
  * @retval 1 queued (flushed by the next Mqtt_Service), 0 = not connected or
  *         send buffer full (caller caches to the offline pending log)
  */
uint8_t Mqtt_PublishRaw(const char *topic, const uint8_t *payload,
                        uint16_t len, uint8_t qos, uint8_t retain);

/** Link-mask bits owned by the network layer (bit2 WiFi / bit3 MQTT). */
uint8_t Mqtt_LinkBits(void);

uint32_t Mqtt_GetReconnectCount(void);
uint32_t Mqtt_GetPublishFailCount(void);

#endif /* __MQTT_CLIENT_H */
