/**
  ******************************************************************************
  * @file    bsp_board_assert.c
  * @brief   Pin-ownership assertions (acceptance A-09) - implementation.
  *
  *  These pin literals live in BSP/ on purpose: this module IS the bridge
  *  between App/app_board_cfg.h and the physical GPIO registers, and the
  *  A-01 rule confines pin literals to BSP/.
  *
  *  Every expectation below comes from App/app_board_cfg.h; nothing here
  *  invents pin facts.  A failure is a firmware/configuration defect, not a
  *  runtime condition - hence print + log + panic (IWDG resets).
  ******************************************************************************
  */
#include "bsp_board_assert.h"
#include "app_board_cfg.h"
#include "bsp_console.h"
#include "bsp_printf.h"
#include "app_rtos.h"
#include "log_service.h"
#include "types.h"

#include "FreeRTOS.h"
#include "task.h"

typedef struct
{
    GPIO_TypeDef *port;
    uint32_t      pin;
    uint32_t      mode;      /* expected MODER value (0/1/2/3) */
    uint32_t      af;        /* expected AFR nibble, or 0xFFFFFFFF = don't care */
    const char   *name;
} pin_expect_t;

static const pin_expect_t s_expect[] =
{
    /* CAN backbone (FDCAN1 on PD0/PD1, AF9) */
    { GPIOD, GPIO_PIN_0,  0x2U, 9U, "CAN_RX PD0" },
    { GPIOD, GPIO_PIN_1,  0x2U, 9U, "CAN_TX PD1" },
    /* RS485 (USART2 on PD5/PD6, AF7) */
    { GPIOD, GPIO_PIN_5,  0x2U, 7U, "RS485_TX PD5" },
    { GPIOD, GPIO_PIN_6,  0x2U, 7U, "RS485_RX PD6" },
    /* console (USART1 on PA9/PA10, AF7) */
    { GPIOA, GPIO_PIN_9,  0x2U, 7U, "CONSOLE_TX PA9" },
    { GPIOA, GPIO_PIN_10, 0x2U, 7U, "CONSOLE_RX PA10" },
    /* EEPROM (I2C1 on PA15/PB9, AF4) */
    { GPIOA, GPIO_PIN_15, 0x2U, 4U, "EEPROM_SCL PA15" },
    { GPIOB, GPIO_PIN_9,  0x2U, 4U, "EEPROM_SDA PB9" },
    /* LCD bit-bang (outputs) */
    { GPIOB, GPIO_PIN_3,  0x1U, 0xFFFFFFFFU, "LCD_SCL PB3" },
    { GPIOB, GPIO_PIN_5,  0x1U, 0xFFFFFFFFU, "LCD_SDA PB5" },
    { GPIOD, GPIO_PIN_11, 0x1U, 0xFFFFFFFFU, "LCD_CS PD11" },
    { GPIOD, GPIO_PIN_12, 0x1U, 0xFFFFFFFFU, "LCD_DC PD12" },
    { GPIOD, GPIO_PIN_13, 0x1U, 0xFFFFFFFFU, "LCD_BL PD13" },
    /* user key + user LED (schematic V1.1; app_board_cfg.h §9) */
    { GPIOD, GPIO_PIN_15, 0x0U, 0xFFFFFFFFU, "KEY_K0 PD15" },
    { GPIOC, GPIO_PIN_0,  0x1U, 0xFFFFFFFFU, "LED1 PC0" },
    /* optional external keys (quiet inputs when not wired) */
    { GPIOA, GPIO_PIN_3,  0x0U, 0xFFFFFFFFU, "KEY_K1 PA3" },
    { GPIOA, GPIO_PIN_0,  0x0U, 0xFFFFFFFFU, "KEY_WKUP PA0" },
#if (FORM_MODE == FORM_M2)
    /* ESP-AT (USART3 on PB10/PB11, AF7) - only present/configured in M2 */
    { GPIOB, GPIO_PIN_10, 0x2U, 7U, "ESP_TX PB10" },
    { GPIOB, GPIO_PIN_11, 0x2U, 7U, "ESP_RX PB11" },
#endif
};

static uint32_t PinMode(GPIO_TypeDef *port, uint32_t pin)
{
    uint32_t shift = 0U;
    uint32_t p = pin;

    while (p > 1U)
    {
        p >>= 1U;
        shift += 2U;
    }
    return (port->MODER >> shift) & 0x3U;
}

static uint32_t PinAf(GPIO_TypeDef *port, uint32_t pin)
{
    uint32_t pos;

    /* AFR index: 0 high nibble set for pins 8..15 */
    pos = 0U;
    {
        uint32_t p = pin;
        while (p > 1U)
        {
            p >>= 1U;
            pos++;
        }
    }
    if (pos < 8U)
    {
        return (port->AFR[0] >> (pos * 4U)) & 0xFU;
    }
    return (port->AFR[1] >> ((pos - 8U) * 4U)) & 0xFU;
}

void Board_Assert_Check(void)
{
    uint32_t i;
    uint32_t bad = 0U;

    for (i = 0U; i < (uint32_t)(sizeof(s_expect) / sizeof(s_expect[0])); i++)
    {
        const pin_expect_t *e = &s_expect[i];
        uint32_t mode = PinMode(e->port, e->pin);
        uint32_t ok = (mode == e->mode);

        if (ok && (e->af != 0xFFFFFFFFU))
        {
            ok = (PinAf(e->port, e->pin) == e->af) ? 1U : 0U;
        }
        if (ok == 0U)
        {
            bad++;
            BSP_Printf("A-09 FAIL %s: MODER=%lu AFR=%lu (want mode=%lu af=%ld)\r\n",
                       e->name,
                       (unsigned long)mode,
                       (unsigned long)PinAf(e->port, e->pin),
                       (unsigned long)e->mode,
                       (e->af == 0xFFFFFFFFU) ? -1L : (long)e->af);
        }
    }

    if (bad != 0U)
    {
        (void)LOG_SVC_PostEvent((uint8_t)LOG_SYSTEM_ERROR,
                                (uint8_t)(bad & 0xFFU), 0x09U, 0U); /* p0=count, p1=0x09:A-09 */
        BSP_Puts("A-09: pin ownership check FAILED - halting (IWDG will reset)\r\n");
        taskDISABLE_INTERRUPTS();
        for (;;)
        {
        }
    }
    BSP_Puts("A-09: pin ownership OK\r\n");
}
