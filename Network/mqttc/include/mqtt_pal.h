/**
  ******************************************************************************
  * @file    mqtt_pal.h
  * @brief   MQTT-C platform abstraction layer - STM32G474 / FreeRTOS port.
  *
  *  Upstream MQTT-C (v1.1.6) ships one mqtt_pal.h per platform; this file is
  *  OUR platform port, written against the documented PAL contract (mqtt_pal.h
  *  doc-block upstream).  It replaces the vendor file on purpose - see
  *  Network/mqttc/README.md.
  *
  *  Transport model (docs/02 decision 5):
  *   - MQTT-C calls mqtt_pal_sendall()/mqtt_pal_recvall() on the client's
  *     socketfd.  Here that fd is a dummy handle; the actual bytes flow
  *     through the ESP-AT pass-through link (Network/esp_at.c).
  *   - recvall is NON-BLOCKING (returns 0 when nothing arrived); mqtt_sync()
  *     is pumped from uplink_task at 100 ms cadence, so it never blocks the
  *     scheduler (T4 must not stall T1/T2, docs/02 决策 3).
  *   - the whole MQTT client is owned by uplink_task only; the mutex macros
  *     are therefore no-ops with a documented single-task confinement.
  *
  *  Time: MQTT_PAL_TIME() is 1-second resolution fed by HAL_GetTick
  *  (keepalive bookkeeping only - not the microsecond DWT source).
  ******************************************************************************
  */
#ifndef __MQTT_PAL_H__
#define __MQTT_PAL_H__

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

#if defined(__cplusplus)
extern "C" {
#endif

/*--------------------------------------------------------------------*/
/* Types                                                              */
/*--------------------------------------------------------------------*/
/* ARM Compiler 6 stdlib has no POSIX ssize_t - MQTT-C only needs a
 * signed size type for send/recv return values. */
typedef int32_t ssize_t;

typedef uint32_t mqtt_pal_time_t;      /* seconds                     */
typedef uint8_t  mqtt_pal_mutex_t;     /* unused (single task owner)  */
typedef uint32_t mqtt_pal_socket_handle;   /* dummy: transport is global */

/*--------------------------------------------------------------------*/
/* Endianness / time / mutexes                                        */
/*--------------------------------------------------------------------*/
#define MQTT_PAL_HTONS(s)  ((uint16_t)((((uint16_t)(s) & 0x00FFU) << 8) | \
                                       (((uint16_t)(s) & 0xFF00U) >> 8)))
#define MQTT_PAL_NTOHS(s)  MQTT_PAL_HTONS(s)

uint32_t Mqtt_Pal_TimeSeconds(void);   /* implemented in mqtt_client.c */
#define MQTT_PAL_TIME()    Mqtt_Pal_TimeSeconds()

#define MQTT_PAL_MUTEX_INIT(mtx_ptr)      do { *(mtx_ptr) = 0U; } while (0)
#define MQTT_PAL_MUTEX_LOCK(mtx_ptr)      do { (void)(mtx_ptr); } while (0)
#define MQTT_PAL_MUTEX_UNLOCK(mtx_ptr)    do { (void)(mtx_ptr); } while (0)

/*--------------------------------------------------------------------*/
/* Transport (implemented in Network/mqtt_client.c)                   */
/*--------------------------------------------------------------------*/
/**
  * @brief  Send len bytes into the ESP pass-through link (blocking on TX
  *         register only; bounded by the UART bitrate).
  * @retval bytes accepted (== len on success), -1 on link-down
  */
ssize_t mqtt_pal_sendall(mqtt_pal_socket_handle fd, const void *buf,
                         size_t len, int flags);

/**
  * @brief  Pull up to bufsz bytes from the ESP RX ring (NON-blocking).
  * @retval bytes read (0 = nothing available), -1 on hard link failure
  */
ssize_t mqtt_pal_recvall(mqtt_pal_socket_handle fd, void *buf,
                         size_t bufsz, int flags);

#if defined(__cplusplus)
}
#endif

#endif /* __MQTT_PAL_H__ */
