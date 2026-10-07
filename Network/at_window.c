/**
  ******************************************************************************
  * @file    at_window.c
  * @brief   ESP pass-through escape window - implementation (v0.6).
  *
  *  Sequence (docs/02 decision 5, R-3):
  *    1. wait for >= AT_WINDOW_GUARD_MS of silence on RX (the ESP "+++"
  *       entry guard; the classic failure mode is sending +++ while data is
  *       still in flight, which the ESP treats as payload);
  *    2. send "+++" with NO CR/LF;
  *    3. wait up to AT_WINDOW_OK_MS for "OK";
  *    4. timeout -> the link is declared dead by the caller.
  ******************************************************************************
  */
#include "at_window.h"
#include "esp_at.h"
#include "app_rtos.h"
#include "main.h"      /* HAL_GetTick */

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

uint8_t AtWindow_ExitPassThrough(void)
{
    char     rsp[64];
    uint16_t rlen = 0U;
    uint32_t quiet_since = HAL_GetTick();
    uint32_t start;

    /* 1) silence guard: track the last received byte */
    while ((HAL_GetTick() - quiet_since) < AT_WINDOW_GUARD_MS)
    {
        if (Esp_RxAvailable() != 0U)
        {
            Esp_FlushRx();                 /* discard, keep waiting for quiet */
            quiet_since = HAL_GetTick();
        }
        vTaskDelay(1U);
    }

    /* 2) escape sequence without line ending */
    Esp_WriteBytes((const uint8_t *)"+++", 3U);

    /* 3) expect OK */
    start = HAL_GetTick();
    while ((HAL_GetTick() - start) < AT_WINDOW_OK_MS)
    {
        uint8_t b;

        App_Hb_Beat(APP_HB_UPLINK);
        while (Esp_Read(&b, 1U) != 0U)
        {
            if (rlen >= (uint16_t)(sizeof(rsp) - 1U))
            {
                rlen = 0U;                 /* window reset is fine here */
            }
            rsp[rlen] = (char)b;
            rlen++;
            rsp[rlen] = '\0';
            if (strstr(rsp, "OK") != NULL)
            {
                return 1U;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return 0U;
}
