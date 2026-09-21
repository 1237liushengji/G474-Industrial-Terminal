/**
  ******************************************************************************
  * @file    bsp_key.c
  * @brief   Polling button driver with debounce & short/long press.
  *
  *  Keys are active-low (board connects to GND). Debounce: the raw level
  *  must be stable for DEBOUNCE_MS. Long press: KEY_LONG_MS.
  ******************************************************************************
  */
#include "bsp_key.h"
#include "bsp_board_cfg.h"

#define KEY_DEBOUNCE_MS   20U
#define KEY_LONG_MS       1000U

typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
    uint8_t       stable_level;    /* debounced level (0=press) */
    uint8_t       debounce_cnt;
    uint8_t       pressed;         /* debounced pressed state   */
    uint8_t       long_reported;
    uint32_t      press_start_ms;
} key_state_t;

static key_state_t s_keys[KEY_COUNT];

void KEY_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    KEY_GPIO_CLK_ENABLE();

    gpio.Pin  = KEY_WKUP_PIN | KEY_K0_PIN | KEY_K1_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;    /* board keys pull to GND when pressed */
    HAL_GPIO_Init(KEY_WKUP_PORT, &gpio);
    HAL_GPIO_Init(KEY_K0_PORT, &gpio);
    HAL_GPIO_Init(KEY_K1_PORT, &gpio);

    s_keys[KEY_WKUP].port = KEY_WKUP_PORT; s_keys[KEY_WKUP].pin = KEY_WKUP_PIN;
    s_keys[KEY_K0].port   = KEY_K0_PORT;   s_keys[KEY_K0].pin   = KEY_K0_PIN;
    s_keys[KEY_K1].port   = KEY_K1_PORT;   s_keys[KEY_K1].pin   = KEY_K1_PIN;
}

static uint8_t KEY_RawLevel(key_state_t *k)
{
    return (HAL_GPIO_ReadPin(k->port, k->pin) == GPIO_PIN_RESET) ? 1U : 0U;
}

key_event_t KEY_Scan(key_id_t *key)
{
    key_event_t result = KEY_EVENT_NONE;
    uint8_t     i;

    for (i = 0; i < KEY_COUNT; i++)
    {
        key_state_t *k = &s_keys[i];
        uint8_t raw = KEY_RawLevel(k);

        /* debounce: require stable level across consecutive scans */
        if (raw == k->stable_level)
        {
            if (k->debounce_cnt < KEY_DEBOUNCE_MS)
            {
                k->debounce_cnt++;
            }
        }
        else
        {
            k->debounce_cnt = 0U;
            k->stable_level = raw;
        }

        if (k->debounce_cnt >= KEY_DEBOUNCE_MS)
        {
            if ((raw != 0U) && !k->pressed)
            {
                /* pressed (debounced) */
                k->pressed = 1U;
                k->long_reported = 0U;
                k->press_start_ms = HAL_GetTick();
            }
            else if ((raw == 0U) && k->pressed)
            {
                /* released */
                k->pressed = 0U;
                if (!k->long_reported)
                {
                    if (key != 0)
                    {
                        *key = (key_id_t)i;
                    }
                    result = KEY_EVENT_SHORT;
                }
            }
            else if ((raw != 0U) && k->pressed && !k->long_reported &&
                     ((HAL_GetTick() - k->press_start_ms) >= KEY_LONG_MS))
            {
                k->long_reported = 1U;
                if (key != 0)
                {
                    *key = (key_id_t)i;
                }
                result = KEY_EVENT_LONG;
            }
        }

        if (result != KEY_EVENT_NONE)
        {
            break;   /* one event per scan call */
        }
    }
    return result;
}
