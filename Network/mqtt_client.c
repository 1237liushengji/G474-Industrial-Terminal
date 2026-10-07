/**
  ******************************************************************************
  * @file    mqtt_client.c
  * @brief   MQTT-C client service (M2, v0.6).
  *
  *  - implements the PAL transport hooks declared in mqttc/include/mqtt_pal.h
  *    (bytes flow through the ESP-AT pass-through link);
  *  - owns the single MQTT session: connect (clean session = false + LWT per
  *    docs/03 §5.3), 100 ms sync pump, error detection, reconnect with the
  *    shared 1->2->...->30 s backoff (same sequence as bus-off, docs/03 §5.3);
  *  - thread confinement: everything here is called from uplink_task.
  ******************************************************************************
  */
#include "mqtt_client.h"
#include "app_board_cfg.h"
#include "app_rtos.h"
#include "at_window.h"
#include "esp_at.h"
#include "log_service.h"
#include "types.h"

#define MQTTC_PAL_FILE mqtt_pal.h
#include "mqtt.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

/*====================================================================*/
/* PAL: transport + time                                               */
/*====================================================================*/
static struct mqtt_client s_client;
static uint8_t s_sendbuf[MQTT_SEND_BUF];
static uint8_t s_recvbuf[MQTT_RECV_BUF];

static uint8_t  s_connected;
static uint8_t  s_inited;
static uint32_t s_reconnects;
static uint32_t s_pub_fails;
static uint32_t s_retry_at;
static uint32_t s_backoff_ms = CAN_BACKOFF_INIT_MS;

uint32_t Mqtt_Pal_TimeSeconds(void)
{
    return HAL_GetTick() / 1000U;          /* keepalive bookkeeping only */
}

ssize_t mqtt_pal_sendall(mqtt_pal_socket_handle fd, const void *buf,
                         size_t len, int flags)
{
    (void)fd;
    (void)flags;

    if (Esp_IsReady() == 0U)
    {
        return -1;
    }
    Esp_WriteBytes((const uint8_t *)buf, (uint16_t)len);
    return (ssize_t)len;
}

ssize_t mqtt_pal_recvall(mqtt_pal_socket_handle fd, void *buf,
                         size_t bufsz, int flags)
{
    uint16_t n;

    (void)fd;
    (void)flags;

    if (Esp_IsReady() == 0U)
    {
        return -1;
    }
    n = Esp_Read((uint8_t *)buf, (uint16_t)((bufsz > 0xFFFFU) ? 0xFFFFU : bufsz));
    return (ssize_t)n;                     /* 0 = nothing yet (non-blocking) */
}

/** inbound publishes: v1 subscribes to nothing; count and ignore. */
static volatile uint32_t s_inbound;
static void Mqtt_PublishCallback(void **state, struct mqtt_response_publish *p)
{
    (void)state;
    (void)p;
    s_inbound++;
}

/*====================================================================*/
/* Session management                                                  */
/*====================================================================*/
void Mqtt_Init(void)
{
    if (s_inited != 0U)
    {
        return;
    }
    (void)mqtt_init(&s_client, 0U, s_sendbuf, sizeof(s_sendbuf),
                    s_recvbuf, sizeof(s_recvbuf), Mqtt_PublishCallback);
    s_inited = 1U;
}

uint8_t Mqtt_IsConnected(void)
{
    return s_connected;
}

uint8_t Mqtt_LinkBits(void)
{
    uint8_t bits = 0U;

    if (Esp_IsReady() != 0U)
    {
        bits |= 0x04U;                     /* bit2: WiFi (pass-through up) */
    }
    if (s_connected != 0U)
    {
        bits |= 0x08U;                     /* bit3: MQTT session        */
    }
    return bits;
}

uint32_t Mqtt_GetReconnectCount(void)
{
    return s_reconnects;
}

uint32_t Mqtt_GetPublishFailCount(void)
{
    return s_pub_fails;
}

static void Mqtt_ScheduleRetry(uint32_t now)
{
    s_backoff_ms = (s_backoff_ms == 0U) ? CAN_BACKOFF_INIT_MS
                                        : s_backoff_ms * 2U;
    if (s_backoff_ms > CAN_BACKOFF_MAX_MS)
    {
        s_backoff_ms = CAN_BACKOFF_MAX_MS;
    }
    s_retry_at = now + s_backoff_ms;
}

/** Lost session: try the +++ window; if it fails the ESP itself is reset. */
static void Mqtt_LostSession(uint32_t now)
{
    if (s_connected != 0U)
    {
        s_connected = 0U;
        (void)LOG_SVC_PostEvent((uint8_t)LOG_MQTT_RECONNECT, 0U, 0U, 0U);
    }
    else
    {
        (void)LOG_SVC_PostEvent((uint8_t)LOG_MQTT_RECONNECT, 1U, 0U, 0U);
    }

    if (AtWindow_ExitPassThrough() == 0U)
    {
        Esp_LinkLost();                    /* full RECOVER through the SM */
        s_backoff_ms = CAN_BACKOFF_INIT_MS;
    }
    Mqtt_ScheduleRetry(now);
    s_reconnects++;
}

void Mqtt_Service(uint32_t now)
{
    Mqtt_Init();

    if (s_connected == 0U)
    {
        enum MQTTErrors err;

        if ((int32_t)(now - s_retry_at) < 0)
        {
            return;                        /* waiting out the backoff */
        }
        if (Esp_Service(now) == 0)
        {
            return;                        /* still climbing AT states */
        }

        /* fresh session each attempt: clean_session=false keeps QoS1
         * bookkeeping on the broker side across OUR reconnects */
        mqtt_reinit(&s_client, 0U, s_sendbuf, sizeof(s_sendbuf),
                    s_recvbuf, sizeof(s_recvbuf));
        err = mqtt_connect(&s_client, MQTT_CLIENT_ID,
                           MQTT_TOPIC_PREFIX "/n0/lwt",
                           "{\"online\":false}", 16U,
                           NULL, NULL,
                           MQTT_CONNECT_WILL_QOS_1 | MQTT_CONNECT_WILL_RETAIN,
                           MQTT_KEEPALIVE_S);
        if (err != MQTT_OK)
        {
            Mqtt_LostSession(now);
            return;
        }

        /* pump until the CONNACK lands (or the error surfaces) */
        for (uint8_t i = 0U; i < 20U; i++)
        {
            App_Hb_Beat(APP_HB_UPLINK);
            (void)mqtt_sync(&s_client);
            if (s_client.error != MQTT_OK)
            {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (s_client.error == MQTT_OK)
        {
            s_connected = 1U;
            s_backoff_ms = CAN_BACKOFF_INIT_MS;
        }
        else
        {
            Mqtt_LostSession(now);
        }
        return;
    }

    /* connected: keep the session alive (keepalive pings + keepalive window
     * are handled inside mqtt_sync via MQTT_PAL_TIME) */
    if ((mqtt_sync(&s_client) != MQTT_OK) || (s_client.error != MQTT_OK))
    {
        Mqtt_LostSession(now);
    }
}

uint8_t Mqtt_PublishRaw(const char *topic, const uint8_t *payload,
                        uint16_t len, uint8_t qos, uint8_t retain)
{
    enum MQTTErrors err;
    uint8_t flags = (qos >= 2U) ? MQTT_PUBLISH_QOS_2
                  : (qos == 1U) ? MQTT_PUBLISH_QOS_1
                                : MQTT_PUBLISH_QOS_0;

    if ((s_connected == 0U) || (topic == NULL) || (payload == NULL))
    {
        return 0U;
    }
    if (retain != 0U)
    {
        flags |= MQTT_PUBLISH_RETAIN;
    }
    err = mqtt_publish(&s_client, topic, payload, len, flags);
    if (err != MQTT_OK)
    {
        s_pub_fails++;
        return 0U;
    }
    return 1U;
}
