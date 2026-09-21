/**
  ******************************************************************************
  * @file    modbus_register.c
  * @brief   Register bank implementation.
  * @note    Values are plain RAM. Measured values (temp/humi/volt/...) are
  *          refreshed by device_manager (App layer); config registers are
  *          mirrored to EEPROM by Storage/config (v0.9+).
  ******************************************************************************
  */
#include "modbus_register.h"

/*====================================================================*/
/* Static storage                                                      */
/*====================================================================*/
static uint16_t s_holding[MB_REG_HOLD_COUNT];
static uint16_t s_input[MB_REG_IN_COUNT];
static uint8_t  s_coils;
static volatile uint8_t s_cfg_dirty = 0U;

/*====================================================================*/
/* Defaults (pre-config-load values)                                   */
/*====================================================================*/
#define DEFAULT_SLAVE_ID       1U
#define DEFAULT_BAUD_IDX       MB_BAUD_IDX_115200
#define DEFAULT_TEMP_LIMIT     500U    /* 50.0 C */
#define DEFAULT_VOLT_LIMIT     1600U   /* 16.00 V */
#define DEFAULT_SAMPLE_PERIOD  500U    /* ms */
#define DEFAULT_DEV_MODE       0U

void MB_REG_Init(void)
{
    uint16_t i;

    for (i = 0; i < MB_REG_HOLD_COUNT; i++)
    {
        s_holding[i] = 0U;
    }
    for (i = 0; i < MB_REG_IN_COUNT; i++)
    {
        s_input[i] = 0U;
    }
    s_coils = 0U;

    /* measured region: give demo values until device_manager runs */
    s_holding[MB_REG_HOLD_TEMP] = 286U;      /* 28.6 C  */
    s_holding[MB_REG_HOLD_HUMI] = 632U;      /* 63.2 %RH*/
    s_holding[MB_REG_HOLD_VOLT] = 1208U;     /* 12.08 V */
    s_holding[MB_REG_HOLD_CURR] = 125U;      /* 1.25 A  */
    s_holding[MB_REG_HOLD_STATUS] = 1U;      /* normal  */
    s_holding[MB_REG_HOLD_ERRCODE] = 0U;

    /* config region defaults */
    s_holding[MB_REG_HOLD_TEMP_LIMIT]    = DEFAULT_TEMP_LIMIT;
    s_holding[MB_REG_HOLD_VOLT_LIMIT]    = DEFAULT_VOLT_LIMIT;
    s_holding[MB_REG_HOLD_SAMPLE_PERIOD] = DEFAULT_SAMPLE_PERIOD;
    s_holding[MB_REG_HOLD_DEV_MODE]      = DEFAULT_DEV_MODE;
    s_holding[MB_REG_HOLD_SLAVE_ID]      = DEFAULT_SLAVE_ID;
    s_holding[MB_REG_HOLD_BAUD_IDX]      = DEFAULT_BAUD_IDX;
}

/*====================================================================*/
/* Holding registers                                                   */
/*====================================================================*/
uint16_t MB_REG_GetHolding(uint16_t addr)
{
    if (addr < MB_REG_HOLD_COUNT)
    {
        return s_holding[addr];
    }
    return 0U;
}

void MB_REG_SetHolding(uint16_t addr, uint16_t value)
{
    if (addr < MB_REG_HOLD_COUNT)
    {
        s_holding[addr] = value;
        if (addr >= MB_REG_HOLD_CFG_FIRST)
        {
            s_cfg_dirty = 1U;   /* 40010..40015 changed -> persist later */
        }
    }
}

uint8_t MB_REG_ConfigDirty(void)
{
    return s_cfg_dirty;
}

void MB_REG_ClearConfigDirty(void)
{
    s_cfg_dirty = 0U;
}

uint8_t MB_REG_HoldingWritable(uint16_t addr)
{
    /* writable: config region 40010..40015 only */
    return (addr >= MB_REG_HOLD_CFG_FIRST) && (addr <= MB_REG_HOLD_MAX_ADDR);
}

uint8_t MB_REG_ValidateConfigValue(uint16_t addr, uint16_t value)
{
    switch (addr)
    {
        case MB_REG_HOLD_TEMP_LIMIT:
        case MB_REG_HOLD_VOLT_LIMIT:
            return (value != 0U) ? 1U : 0U;

        case MB_REG_HOLD_SAMPLE_PERIOD:
            /* 10 ms .. 60000 ms */
            return ((value >= 10U) && (value <= 60000U)) ? 1U : 0U;

        case MB_REG_HOLD_DEV_MODE:
            return (value <= 3U) ? 1U : 0U;

        case MB_REG_HOLD_SLAVE_ID:
            /* 1..247 (0 = broadcast, 248+ reserved) */
            return ((value >= 1U) && (value <= 247U)) ? 1U : 0U;

        case MB_REG_HOLD_BAUD_IDX:
            /* 0..4 = 9600..115200 (230400 removed: transceiver limit) */
            return (value <= 4U) ? 1U : 0U;

        default:
            /* config region only; non-config handled by caller */
            return 0U;
    }
}

/*====================================================================*/
/* Input registers                                                     */
/*====================================================================*/
uint16_t MB_REG_GetInput(uint16_t addr)
{
    if (addr < MB_REG_IN_COUNT)
    {
        return s_input[addr];
    }
    return 0U;
}

/*====================================================================*/
/* Coils                                                               */
/*====================================================================*/
uint8_t MB_REG_GetCoil(uint16_t addr)
{
    if (addr < MB_REG_COIL_COUNT)
    {
        return (uint8_t)((s_coils >> addr) & 0x01U);
    }
    return 0U;
}

void MB_REG_SetCoil(uint16_t addr, uint8_t on)
{
    if (addr < MB_REG_COIL_COUNT)
    {
        if (on != 0U)
        {
            s_coils |= (uint8_t)(1U << addr);
        }
        else
        {
            s_coils &= (uint8_t)~(1U << addr);
        }
    }
}

/*====================================================================*/
/* Statistics                                                          */
/*====================================================================*/
void MB_REG_IncRxCount(void)
{
    s_holding[MB_REG_HOLD_RXCNT]++;
}

void MB_REG_IncTxCount(void)
{
    s_holding[MB_REG_HOLD_TXCNT]++;
}

void MB_REG_IncCrcError(void)
{
    s_holding[MB_REG_HOLD_CRCERR]++;
}

void MB_REG_SetLastError(uint16_t err)
{
    s_holding[MB_REG_HOLD_ERRCODE] = err;
}

void MB_REG_SetDeviceStatus(uint16_t st)
{
    s_holding[MB_REG_HOLD_STATUS] = st;
}

/*====================================================================*/
/* Config helpers                                                      */
/*====================================================================*/
uint8_t MB_REG_GetSlaveId(void)
{
    return (uint8_t)(s_holding[MB_REG_HOLD_SLAVE_ID] & 0xFFU);
}

uint16_t MB_REG_GetBaudIdx(void)
{
    return s_holding[MB_REG_HOLD_BAUD_IDX];
}

uint32_t MB_REG_BaudFromIdx(uint8_t idx)
{
    static const uint32_t baud_table[] = {
        9600U, 19200U, 38400U, 57600U, 115200U
    };
    if (idx > MB_BAUD_IDX_MAX)
    {
        return 115200U;
    }
    return baud_table[idx];
}
