/**
  ******************************************************************************
  * @file    uplink_port.h
  * @brief   Uplink abstraction for the gateway (T4): M1 = PC shadow gateway
  *          frames over USART1, M2 = on-device MQTT-C (arrives in v0.6).
  *
  *  Wire protocol (M1, frozen in Tools/mqtt/mqtt_topics.md):
  *
  *      AA 55 | type(1) | seq(2 BE) | len(2 BE) | payload[len] | crc16(2 LE)
  *
  *  - magic 0xAA55 is mutually exclusive with console text: every console
  *    line starts with printable ASCII 0x20..0x7E (docs/02 decision 4), so
  *    the PC demultiplexes on the first byte.
  *  - crc16 = MODBUS_CRC16 over type..payload (5 + len bytes).
  *  - payload = the JSON exactly as frozen in docs/03 §5.1 - M1 publishes it
  *    verbatim, M2 will publish the same bytes through MQTT-C (the builders
  *    below are the single source for both forms).
  *
  *  Periods: telemetry 1 s, status 5 s, counters 10 s, snapshot 30 s,
  *  meta on boot + on node config change, events as they happen.
  ******************************************************************************
  */
#ifndef __UPLINK_PORT_H
#define __UPLINK_PORT_H

#include <stdint.h>

/** Create internal state (call before the scheduler). */
void Uplink_Init(void);

/** T4 body (gateway only; on the node the task is not created). */
void Uplink_TaskMain(void);

/** Queue one event for the event topic (link edges / forwarded node events). */
void Uplink_NotifyEvent(uint8_t ev, uint8_t p0, uint8_t p1, uint8_t p2);

/** Frames handed to the transport / events dropped (queue full). */
uint32_t Uplink_GetFrameCount(void);
uint32_t Uplink_GetDropCount(void);

#endif /* __UPLINK_PORT_H */
