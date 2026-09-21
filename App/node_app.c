/**
  ******************************************************************************
  * @file    slave_main.c
  * @brief   Slave node (B board) application entry.
  * @note    Compiled only when MODBUS_NODE_ROLE == NODE_ROLE_SLAVE.
  *
  *  v1.0 UI: 3 pages on the SPI TFT LCD
  *    HOME    - device info + live measurands + comm counters
  *    ERROR   - CRC / exception counters
  *    CONFIG  - editable parameters (K1 modify, WKUP confirm+save)
  *  Key K0 cycles pages. Parameters edited here go through the register
  *  bank -> auto-saved to 24C02 by the config layer.
  ******************************************************************************
  */
#include "slave_main.h"

#include <string.h>
#include "led.h"
#include "bsp_rs485.h"
#include "bsp_uart.h"
#include "bsp_tick.h"
#include "bsp_board_cfg.h"
#include "bsp_lcd.h"
#include "bsp_key.h"
#include "bsp_w25q128.h"
#include "modbus_slave.h"
#include "modbus_register.h"
#include "config.h"
#include "log.h"
#include "types.h"

/*====================================================================*/
/* Log event bridge (slave engine events -> W25Q128)                   */
/*====================================================================*/
static void Slave_LogEvent(uint8_t event, uint16_t param)
{
    LOG_WriteEvent((log_event_t)event,
                   (uint8_t)(param & 0xFFU), (uint8_t)((param >> 8) & 0xFFU), 0U);
}

/*====================================================================*/
/* UI state                                                            */
/*====================================================================*/
typedef enum {
    UI_PAGE_HOME = 0,
    UI_PAGE_ERROR,
    UI_PAGE_CONFIG,
    UI_PAGE_COUNT
} ui_page_t;

static ui_page_t s_page = UI_PAGE_HOME;
static uint8_t   s_cfg_cursor = 0U;    /* selected CONFIG row while editing */
static uint8_t   s_editing = 0U;       /* 1 = adjusting a parameter value   */
static uint32_t  s_ui_last_ms = 0U;
static uint8_t   s_ui_redraw = 1U;

/* measured demo values (device_manager replaces later) */
static uint16_t s_demo_temp = 286U;    /* 28.6 C */
static uint16_t s_demo_humi = 632U;    /* 63.2 % */
static uint16_t s_demo_volt = 1208U;   /* 12.08 V */
static uint16_t s_demo_curr = 125U;    /* 1.25 A */

/*====================================================================*/
/* Small numeric formatting (no stdio dependency)                       */
/*====================================================================*/
/* uint32 -> decimal string. tmp sized for the full 32-bit range. */
static void U32ToStr(uint32_t v, char *out)
{
    char tmp[12];          /* 10 digits max + terminator */
    int i = 0;

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

/* value with one decimal digit, e.g. 286 -> "28.6" */
static void U16ToStr1(uint16_t v, char *out)
{
    char tmp[16];
    char *p = tmp;
    uint16_t ip = (uint16_t)(v / 10U);
    uint16_t dp = (uint16_t)(v % 10U);

    U32ToStr(ip, p);
    while (*p) { *out++ = *p++; }
    *out++ = '.';
    p = tmp;
    U32ToStr(dp, p);
    while (*p) { *out++ = *p++; }
    *out = '\0';
}

/*====================================================================*/
/* RX bridge                                                           */
/*====================================================================*/
static void RSCB_SlaveRx(uint8_t byte)
{
    MB_Slave_OnRxByte(byte, BSP_Tick_GetUs());
}

#if (RS485_RX_MODE == 1)
/* DMA mode: whole frames arrive from BSP_UART_RxDmaService() */
static void RSCB_SlaveRxFrame(const uint8_t *frame, uint16_t len,
                              uint32_t now_us)
{
    (void)now_us;
    if (MB_Slave_OnRxFrame(frame, len) != 0U)
    {
        LED2_Toggle;   /* response sent */
    }
}
#endif

/* defined below, used in the main loop */
static void Slave_ApplyConfigChange(void);

/*====================================================================*/
/* Home page                                                           */
/*====================================================================*/
/* 20 columns x 17 rows layout for the 240x280 panel                   */
static const char s_line[] = "--------------------";

/* static labels drawn once per page switch */
static void UI_DrawHomeStatic(void)
{
    char idbuf[8];
    char bbuf[8];
    char buf[32];

    U32ToStr(MB_REG_GetSlaveId(), idbuf);
    U32ToStr(MB_REG_BaudFromIdx((uint8_t)MB_REG_GetBaudIdx()), bbuf);

    LCD_Print(0, 3, "MODBUS SLAVE", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    buf[0] = 'I'; buf[1] = 'D'; buf[2] = ':'; buf[3] = '\0';
    strcat(buf, idbuf);
    strcat(buf, "  ");
    strcat(buf, bbuf);
    LCD_Print(1, 3, buf, LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
    LCD_Print(2, 0, s_line, LCD_COLOR_GRAY, LCD_COLOR_BLACK);

    LCD_Print(3, 0, "TEMP:", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    LCD_Print(4, 0, "HUMI:", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    LCD_Print(5, 0, "VOLT:", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    LCD_Print(6, 0, "CURR:", LCD_COLOR_WHITE, LCD_COLOR_BLACK);

    LCD_Print(7, 0, s_line, LCD_COLOR_GRAY, LCD_COLOR_BLACK);

    LCD_Print(9, 0, "RX:", LCD_COLOR_GREEN, LCD_COLOR_BLACK);
    LCD_Print(9, 11, "TX:", LCD_COLOR_GREEN, LCD_COLOR_BLACK);
    LCD_Print(10, 0, "CRC:", LCD_COLOR_RED, LCD_COLOR_BLACK);
    LCD_Print(10, 11, "EXC:", LCD_COLOR_RED, LCD_COLOR_BLACK);

    LCD_Print(12, 0, "STATUS:", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    LCD_Print(15, 0, "K0:next K1:edit", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
}

/* value fields refreshed every 500 ms */
static void UI_DrawHomeValues(void)
{
    char buf[16];
    comm_stats_t st;
    char idbuf[8];
    char bbuf[8];

    U16ToStr1(s_demo_temp, buf);   strcat(buf, " C");
    LCD_PrintField(3, 8, buf, 9, LCD_COLOR_WHITE, LCD_COLOR_BLACK);

    U16ToStr1(s_demo_humi, buf);   strcat(buf, " %");
    LCD_PrintField(4, 8, buf, 9, LCD_COLOR_WHITE, LCD_COLOR_BLACK);

    U16ToStr1(s_demo_volt, buf);   strcat(buf, " V");
    LCD_PrintField(5, 8, buf, 9, LCD_COLOR_WHITE, LCD_COLOR_BLACK);

    U16ToStr1(s_demo_curr, buf);   strcat(buf, " A");
    LCD_PrintField(6, 8, buf, 9, LCD_COLOR_WHITE, LCD_COLOR_BLACK);

    U32ToStr(MB_REG_GetHolding(MB_REG_HOLD_RXCNT), idbuf);
    U32ToStr(MB_REG_GetHolding(MB_REG_HOLD_TXCNT), bbuf);
    LCD_PrintField(9, 4, idbuf, 6, LCD_COLOR_GREEN, LCD_COLOR_BLACK);
    LCD_PrintField(9, 15, bbuf, 5, LCD_COLOR_GREEN, LCD_COLOR_BLACK);

    MB_Slave_GetStats(&st);
    U32ToStr(st.crc_error_count, idbuf);
    U32ToStr(st.exception_count, bbuf);
    LCD_PrintField(10, 5, idbuf, 5, LCD_COLOR_RED, LCD_COLOR_BLACK);
    LCD_PrintField(10, 16, bbuf, 4, LCD_COLOR_RED, LCD_COLOR_BLACK);

    if (MB_REG_GetHolding(MB_REG_HOLD_STATUS) == 1U)
    {
        LCD_PrintField(12, 9, "OK", 4, LCD_COLOR_GREEN, LCD_COLOR_BLACK);
    }
    else
    {
        LCD_PrintField(12, 9, "WARN", 4, LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
    }
}

/*====================================================================*/
/* Error page                                                          */
/*====================================================================*/
static void UI_DrawErrorStatic(void)
{
    LCD_Print(0, 5, "ERROR PAGE", LCD_COLOR_RED, LCD_COLOR_BLACK);
    LCD_Print(1, 0, s_line, LCD_COLOR_GRAY, LCD_COLOR_BLACK);

    LCD_Print(2, 0, "CRC ERROR :", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    LCD_Print(3, 0, "EXCEPTION :", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    LCD_Print(4, 0, "RX TOTAL  :", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    LCD_Print(5, 0, "LAST ERR  :", LCD_COLOR_WHITE, LCD_COLOR_BLACK);

    LCD_Print(8, 0, "K0:next page", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
}

static void UI_DrawErrorValues(void)
{
    char buf[16];
    comm_stats_t st;

    MB_Slave_GetStats(&st);
    U32ToStr(st.crc_error_count, buf);
    LCD_PrintField(2, 12, buf, 6, LCD_COLOR_RED, LCD_COLOR_BLACK);

    U32ToStr(st.exception_count, buf);
    LCD_PrintField(3, 12, buf, 6, LCD_COLOR_RED, LCD_COLOR_BLACK);

    U32ToStr(MB_REG_GetHolding(MB_REG_HOLD_RXCNT), buf);
    LCD_PrintField(4, 12, buf, 6, LCD_COLOR_WHITE, LCD_COLOR_BLACK);

    U32ToStr(MB_REG_GetHolding(MB_REG_HOLD_ERRCODE), buf);
    LCD_PrintField(5, 12, buf, 6, LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
}

/*====================================================================*/
/* Config page (editable)                                              */
/*====================================================================*/
typedef struct {
    uint16_t addr;          /* register address */
    const char *name;
} cfg_item_t;

static const cfg_item_t s_cfg_items[] = {
    { MB_REG_HOLD_TEMP_LIMIT,    "TEMP LIMIT" },
    { MB_REG_HOLD_VOLT_LIMIT,    "VOLT LIMIT" },
    { MB_REG_HOLD_SAMPLE_PERIOD, "PERIOD(ms)" },
    { MB_REG_HOLD_SLAVE_ID,      "SLAVE ID"   },
    { MB_REG_HOLD_BAUD_IDX,      "BAUD IDX"   },
};
#define CFG_ITEM_COUNT  ((uint8_t)(sizeof(s_cfg_items) / sizeof(s_cfg_items[0])))

/* one adjustment step for the selected register */
static uint16_t CfgStep(uint16_t addr, uint16_t cur)
{
    switch (addr)
    {
        case MB_REG_HOLD_TEMP_LIMIT:    return (cur < 600U)  ? (uint16_t)(cur + 10U) : 100U;
        case MB_REG_HOLD_VOLT_LIMIT:    return (cur < 2000U) ? (uint16_t)(cur + 50U) : 800U;
        case MB_REG_HOLD_SAMPLE_PERIOD: return (cur < 5000U) ? (uint16_t)(cur + 100U) : 100U;
        case MB_REG_HOLD_SLAVE_ID:      return (cur < 247U)  ? (uint16_t)(cur + 1U)   : 1U;
        case MB_REG_HOLD_BAUD_IDX:      return (cur < MB_BAUD_IDX_MAX) ? (uint16_t)(cur + 1U) : 0U;
        default:                        return cur;
    }
}

/* static frame for the config page (labels + border + hint) */
static void UI_DrawConfig(void)
{
    uint8_t i;

    LCD_Print(0, 5, "CONFIG PAGE", LCD_COLOR_CYAN, LCD_COLOR_BLACK);
    LCD_Print(1, 0, s_line, LCD_COLOR_GRAY, LCD_COLOR_BLACK);
    for (i = 0; i < CFG_ITEM_COUNT; i++)
    {
        LCD_Print((uint8_t)(2 + i), 0, s_cfg_items[i].name,
                  LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    }
    if (s_editing)
    {
        LCD_Print(8, 0, "K0:item K1:value", LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
        LCD_Print(9, 0, "WKUP:save", LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
    }
    else
    {
        LCD_Print(8, 0, "K0:next K1:edit", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
    }
}

/* dynamic values + highlighted row on the config page */
static void UI_DrawConfigValues(void)
{
    char buf[16];
    uint8_t i;

    for (i = 0; i < CFG_ITEM_COUNT; i++)
    {
        uint16_t fg = (s_editing && (i == s_cfg_cursor)) ? LCD_COLOR_BLACK : LCD_COLOR_WHITE;
        uint16_t bg = (s_editing && (i == s_cfg_cursor)) ? LCD_COLOR_YELLOW : LCD_COLOR_BLACK;
        uint16_t val = MB_REG_GetHolding(s_cfg_items[i].addr);

        /* re-draw the highlighted row's label too (inverted colors) */
        if (s_editing && (i == s_cfg_cursor))
        {
            LCD_Print((uint8_t)(2 + i), 0, s_cfg_items[i].name, fg, bg);
        }
        U32ToStr(val, buf);
        LCD_PrintField((uint8_t)(2 + i), 13, buf, 6, fg, bg);
    }
}

static void UI_DrawConfigFrame(void)
{
    UI_DrawConfig();
    UI_DrawConfigValues();
}

/*====================================================================*/
/* Page dispatcher + key handling                                      */
/*====================================================================*/
static void UI_HandleKey(key_event_t ev, key_id_t key)
{
    (void)ev;
    switch (key)
    {
        case KEY_K0:
            if (s_page == UI_PAGE_CONFIG)
            {
                if (s_editing)
                {
                    /* in edit mode K0 moves the highlight to the next
                       editable item (TempLimit->VoltLimit->...->BaudIdx) */
                    s_cfg_cursor = (uint8_t)((s_cfg_cursor + 1U) % CFG_ITEM_COUNT);
                }
                else
                {
                    /* not editing: K0 leaves the config page as usual */
                    s_page = (ui_page_t)((s_page + 1U) % UI_PAGE_COUNT);
                }
            }
            else
            {
                /* on other pages K0 cycles pages */
                s_page = (ui_page_t)((s_page + 1U) % UI_PAGE_COUNT);
            }
            s_ui_redraw = 1U;
            break;

        case KEY_K1:
            if (s_page == UI_PAGE_CONFIG)
            {
                if (!s_editing)
                {
                    s_editing = 1U;
                    s_cfg_cursor = 0U;
                }
                else
                {
                    /* adjust the currently highlighted item */
                    uint16_t addr = s_cfg_items[s_cfg_cursor].addr;
                    uint16_t cur = MB_REG_GetHolding(addr);
                    MB_REG_SetHolding(addr, CfgStep(addr, cur));
                }
                s_ui_redraw = 1U;
            }
            break;

        case KEY_WKUP:
            if ((s_page == UI_PAGE_CONFIG) && s_editing)
            {
                /* confirm: exit edit; registers are auto-saved by config */
                s_editing = 0U;
                s_ui_redraw = 1U;
            }
            break;

        default:
            break;
    }
}

/* draw the static (label) layer of the current page - called once */
static void UI_DrawStatic(void)
{
    switch (s_page)
    {
        case UI_PAGE_HOME:    UI_DrawHomeStatic();  break;
        case UI_PAGE_ERROR:   UI_DrawErrorStatic(); break;
        case UI_PAGE_CONFIG:  UI_DrawConfig();      break;
        default:              break;
    }
}

/* refresh only the dynamic value rows (cheap, non-blocking) */
static void UI_DrawValues(void)
{
    switch (s_page)
    {
        case UI_PAGE_HOME:    UI_DrawHomeValues();   break;
        case UI_PAGE_ERROR:   UI_DrawErrorValues();  break;
        case UI_PAGE_CONFIG:  UI_DrawConfigValues(); break;
        default:              break;
    }
}

/* full redraw of the config page (static frame + values) used on edits */
static void UI_DrawConfigFull(void)
{
    UI_DrawConfig();
    UI_DrawConfigValues();
}

static void UI_Update(void)
{
    uint32_t now = HAL_GetTick();

    if (s_ui_redraw)
    {
        s_ui_redraw = 0U;
        s_ui_last_ms = now;
        /* clear whole screen first so no remnants of the previous page
           stay visible (labels of the old page are not overwritten by
           the new page's sparse label layout) */
        LCD_Clear();
        if (s_page == UI_PAGE_CONFIG)
        {
            UI_DrawConfigFull();   /* labels + values, single shot */
        }
        else
        {
            UI_DrawStatic();
            UI_DrawValues();
        }
    }
    else if ((now - s_ui_last_ms) >= 500U)
    {
        s_ui_last_ms = now;
        switch (s_page)
        {
            case UI_PAGE_HOME:   UI_DrawHomeValues();  break;
            case UI_PAGE_ERROR:  UI_DrawErrorValues(); break;
            /* CONFIG page changes are event-driven (keys) -> redraw only */
            default: break;
        }
    }
}

/*====================================================================*/
/* Entry                                                               */
/*====================================================================*/
void Slave_Main(void)
{
    config_param_t cfg;
    uint32_t baud;
    uint8_t  slave_id;
    uint32_t last_save_check = 0U;
    key_id_t key;
    key_event_t kev;

    MB_REG_Init();

    /* flash log subsystem (W25Q128) */
    (void)W25Q_Init();
    LOG_SetTickSource(HAL_GetTick);
    LOG_Init();
    LOG_WriteEvent(LOG_BOOT, 0U, 0U, 0U);

    /* load parameters from 24C02 */
    (void)CONFIG_Init(&cfg);
    CONFIG_ApplyToRegisters(&cfg);
    MB_REG_ClearConfigDirty();

    slave_id = cfg.slave_id;
    baud     = MB_REG_BaudFromIdx(cfg.baud_idx);

    RS485_Init(baud);
    RS485_SetRxCallback(RSCB_SlaveRx);
#if (RS485_RX_MODE == 1)
    RS485_SetRxFrameCallback(RSCB_SlaveRxFrame);
#endif

    MB_Slave_Init(slave_id, baud);
    MB_Slave_SetTxFunc(RS485_SendFrame);
    MB_Slave_SetEventCallback(Slave_LogEvent);

    KEY_Init();
    LCD_Init();

    LED1_OFF;
    LED2_OFF;
    s_ui_last_ms = HAL_GetTick();
    s_ui_redraw = 1U;

    /* seed demo measurands into register bank (visible to master) */
    MB_REG_SetHolding(MB_REG_HOLD_TEMP, s_demo_temp);
    MB_REG_SetHolding(MB_REG_HOLD_HUMI, s_demo_humi);
    MB_REG_SetHolding(MB_REG_HOLD_VOLT, s_demo_volt);
    MB_REG_SetHolding(MB_REG_HOLD_CURR, s_demo_curr);

    while (1)
    {
        /* Modbus engine */
#if (RS485_RX_MODE == 1)
        BSP_UART_RxDmaService();   /* delivers frames -> RxFrame callback */
#else
        if (MB_Slave_Poll(BSP_Tick_GetUs()) != 0U)
        {
            LED2_Toggle;   /* response sent */
        }
#endif

        /* keys every ~5 ms */
        kev = KEY_Scan(&key);
        if (kev != KEY_EVENT_NONE)
        {
            UI_HandleKey(kev, key);
        }

        /* LCD refresh (500 ms + on change) */
        UI_Update();

        /* auto-save config changed by Modbus writes (40010..40015) */
        if (MB_REG_ConfigDirty() &&
            ((HAL_GetTick() - last_save_check) >= 20U))
        {
            last_save_check = HAL_GetTick();
            Slave_ApplyConfigChange();
        }

        HAL_Delay(2);
    }
}

/* re-apply protocol-side settings that depend on config registers */
static void Slave_ApplyConfigChange(void)
{
    config_param_t p;
    static uint32_t s_applied_baud = 0U;   /* baud actually set on the UART */

    CONFIG_ReadFromRegisters(&p);
    if (CONFIG_Save(&p))
    {
        MB_REG_ClearConfigDirty();
        LOG_WriteEvent(LOG_CONFIG_CHANGE, p.slave_id,
                       (uint8_t)p.baud_idx, 0U);
    }

    /* live apply: slave id + baudrate */
    MB_Slave_SetSlaveId(MB_REG_GetSlaveId());
    {
        uint32_t baud = MB_REG_BaudFromIdx((uint8_t)MB_REG_GetBaudIdx());
        if (s_applied_baud == 0U)
        {
            s_applied_baud = RS485_DEFAULT_BAUDRATE;  /* baud set in RS485_Init */
        }
        if (baud != s_applied_baud)
        {
            /* reprogram the UART even when the new baud equals the default
               115200 (the old code skipped this, leaving the link dead
               after switching 9600 -> 115200) */
            BSP_UART_SetBaudrate(baud);
            s_applied_baud = baud;
        }
        MB_Slave_SetBaudrate(baud);
    }
}
