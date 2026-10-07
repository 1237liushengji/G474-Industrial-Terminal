/**
  ******************************************************************************
  * @file    gw_ui.c
  * @brief   Gateway monitoring page (v0.7, docs/05) - A board TFT.
  *
  *  Layout (20 cols x 17 rows on the 240x280 panel):
  *    - LINK      : 4 link bits (485 / CAN / WiFi / MQTT)          [live]
  *    - CAN       : tx / txAck / ovr / busOff                      [live]
  *    - MB        : timeouts / retries / exceptions                [live]
  *    - MQTT      : conn / reconnects / publish failures           [live, M2]
  *    - BACKFILL  : backlog / replayed / lost                      [live, M2]
  *    - SNAP      : age / ok / err                                 [live]
  *    - peer HB age, uptime, free heap, task_ovr                   [live]
  *  Redraw policy (A-12): static labels once; only value fields on the
  *  500 ms tick - no full-screen clear outside bring-up, so the LCD work
  *  never blocks the protocol tasks beyond their normal margins.
  *
  *  Keys (silk): K0 = request one snapshot now (Bridge_RequestSnapshot)
  *        K1 = force an immediate backfill sweep (M2; no-op in M1)
  ******************************************************************************
  */
#include "gw_ui.h"
#include "app_board_cfg.h"
#include "app_rtos.h"
#include "backfill.h"
#include "bridge.h"
#include "bsp_key.h"
#include "bsp_lcd.h"
#include "can_drv.h"
#include "mqtt_client.h"
#include "modbus_register.h"
#include "uplink_port.h"

#include "FreeRTOS.h"
#include "task.h"

/*====================================================================*/
/* small helpers                                                       */
/*====================================================================*/
static void U32ToStr(uint32_t v, char *out)
{
    char tmp[12];
    int  i = 0;

    if (v == 0U)
    {
        tmp[i++] = '0';
    }
    while (v > 0U)
    {
        tmp[i++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (i > 0)
    {
        *out++ = tmp[--i];
    }
    *out = '\0';
}

/*====================================================================*/
/* static layer                                                        */
/*====================================================================*/
static void Gw_UiDrawStatic(void)
{
    LCD_Clear();
    LCD_Print(0, 4, "GATEWAY MONITOR", LCD_COLOR_CYAN, LCD_COLOR_BLACK);
    LCD_Print(1, 0, "--------------------", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
    LCD_Print(2, 0, "LINK", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    LCD_Print(3, 0, "CAN tx/ack ovr bo", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    LCD_Print(5, 0, "MB to/rt/exc", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    LCD_Print(7, 0, "MQTT conn rec pf", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    LCD_Print(8, 0, "BF bklg rply lost", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    LCD_Print(10, 0, "SNAP ok/err age", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    LCD_Print(12, 0, "PEER HB age", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    LCD_Print(13, 0, "UP", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    LCD_Print(14, 0, "HEAP", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    LCD_Print(15, 0, "K0:snap K1:bf", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
}

/*====================================================================*/
/* dynamic layer                                                       */
/*====================================================================*/
static void Gw_UiDrawValues(void)
{
    char     b[24];
    uint16_t lk = MB_REG_GetHolding(MB_REG_HOLD_LINK_MASK);
    const CAN_Stats *cs = CAN_GetStats();

    /* LINK row: 485 / CAN / WiFi / MQTT as +/- */
    b[0] = '4'; b[1] = (lk & 0x01U) ? '+' : '-';
    b[2] = ' '; b[3] = 'C'; b[4] = (lk & 0x02U) ? '+' : '-';
    b[5] = ' '; b[6] = 'W'; b[7] = (lk & 0x04U) ? '+' : '-';
    b[8] = ' '; b[9] = 'M'; b[10] = (lk & 0x08U) ? '+' : '-';
    b[11] = '\0';
    LCD_PrintField(2, 8, b, 12, LCD_COLOR_GREEN, LCD_COLOR_BLACK);

    /* CAN row */
    U32ToStr(cs->txAckCount & 0xFFFFU, b);
    LCD_PrintField(3, 4, b, 5, LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    U32ToStr(cs->rxOverruns & 0xFFFFU, b);
    LCD_PrintField(3, 12, b, 5, LCD_COLOR_YELLOW, LCD_COLOR_BLACK);

    /* MB row */
    U32ToStr(MB_REG_GetHolding(MB_REG_HOLD_MB_TIMEOUT), b);
    LCD_PrintField(5, 4, b, 5, LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    U32ToStr(MB_REG_GetHolding(MB_REG_HOLD_MB_EXCEPTION), b);
    LCD_PrintField(5, 14, b, 5, LCD_COLOR_YELLOW, LCD_COLOR_BLACK);

#if (FORM_MODE == FORM_M2)
    /* MQTT row */
    b[0] = Mqtt_IsConnected() ? 'Y' : 'N';
    b[1] = '\0';
    LCD_PrintField(7, 5, b, 2,
                   Mqtt_IsConnected() ? LCD_COLOR_GREEN : LCD_COLOR_RED,
                   LCD_COLOR_BLACK);
    U32ToStr(Mqtt_GetReconnectCount(), b);
    LCD_PrintField(7, 9, b, 5, LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    U32ToStr(Mqtt_GetPublishFailCount(), b);
    LCD_PrintField(7, 16, b, 5, LCD_COLOR_YELLOW, LCD_COLOR_BLACK);

    /* BACKFILL row */
    U32ToStr(Backfill_GetBacklog(), b);
    LCD_PrintField(8, 5, b, 6, LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    U32ToStr(Backfill_GetReplayedCount(), b);
    LCD_PrintField(8, 11, b, 5, LCD_COLOR_GREEN, LCD_COLOR_BLACK);
    U32ToStr(Backfill_GetLostCount(), b);
    LCD_PrintField(8, 17, b, 4, LCD_COLOR_RED, LCD_COLOR_BLACK);
#else
    LCD_PrintField(7, 5, "M1: PC shadow gw", 16, LCD_COLOR_GRAY, LCD_COLOR_BLACK);
    LCD_PrintField(8, 5, "no device uplink", 16, LCD_COLOR_GRAY, LCD_COLOR_BLACK);
#endif

    /* SNAP row */
    U32ToStr(Bridge_GetSnapOkCount(), b);
    LCD_PrintField(10, 5, b, 5, LCD_COLOR_GREEN, LCD_COLOR_BLACK);
    U32ToStr(Bridge_GetSnapErrCount(), b);
    LCD_PrintField(10, 12, b, 5, LCD_COLOR_RED, LCD_COLOR_BLACK);

    /* PEER HB age (s) */
    {
        uint32_t age = Bridge_GetHbAgeMs();
        if (age == 0xFFFFFFFFU)
        {
            LCD_PrintField(12, 12, "never", 6, LCD_COLOR_RED, LCD_COLOR_BLACK);
        }
        else
        {
            U32ToStr(age / 1000U, b);
            LCD_PrintField(12, 12, b, 6, LCD_COLOR_WHITE, LCD_COLOR_BLACK);
        }
    }

    /* UP + HEAP + task_ovr */
    U32ToStr(HAL_GetTick() / 1000U, b);
    LCD_PrintField(13, 3, b, 7, LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    U32ToStr((uint32_t)xPortGetFreeHeapSize(), b);
    LCD_PrintField(14, 5, b, 6, LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    U32ToStr(App_GetTaskOvr(), b);
    LCD_PrintField(14, 13, b, 5, LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
}

/*====================================================================*/
/* public                                                              */
/*====================================================================*/
static uint32_t s_last_ms;

void Gw_UiInit(void)
{
    KEY_Init();
    LCD_Init();
    Gw_UiDrawStatic();
    Gw_UiDrawValues();
    s_last_ms = HAL_GetTick();
}

void Gw_UiStep(void)
{
    key_id_t    key;
    key_event_t kev = KEY_Scan(&key);

    if (kev != KEY_EVENT_NONE)
    {
        switch (key)
        {
            case KEY_K0:
                Bridge_RequestSnapshot();       /* manual snapshot (docs v0.7) */
                LCD_PrintField(15, 0, "snap req   ", 12,
                               LCD_COLOR_CYAN, LCD_COLOR_BLACK);
                break;
            case KEY_K1:
                Backfill_Force();               /* manual backfill sweep */
                LCD_PrintField(15, 0, "bf sweep   ", 12,
                               LCD_COLOR_CYAN, LCD_COLOR_BLACK);
                break;
            default:
                break;
        }
    }

    /* values refresh: 1 s tick, never a full redraw (acceptance A-12) */
    if ((HAL_GetTick() - s_last_ms) >= 1000U)
    {
        s_last_ms = HAL_GetTick();
        Gw_UiDrawValues();
    }
}
