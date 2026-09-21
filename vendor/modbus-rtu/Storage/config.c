/**
  ******************************************************************************
  * @file    config.c
  * @brief   Parameter persistence on 24C02.
  ******************************************************************************
  */
#include "config.h"
#include "bsp_eeprom.h"
#include "modbus_crc.h"   /* CRC16-Modbus reused for media integrity */
#include <string.h>

/*====================================================================*/
/* Static                                                              */
/*====================================================================*/
static uint8_t s_loaded = 0U;

/* on-media layout (17 bytes): 15 bytes payload + 2 bytes CRC
   magic[2] ver[1] slave_id[1] baud_idx[1] temp_limit[2] volt_limit[2]
   sample_period[2] dev_mode[1] reserved[3] crc[2]                       */
typedef struct {
    uint8_t  magic[2];      /* 'M','B' */
    uint8_t  version;
    uint8_t  slave_id;
    uint8_t  baud_idx;
    uint8_t  temp_limit[2];
    uint8_t  volt_limit[2];
    uint8_t  sample_period[2];
    uint8_t  dev_mode;
    uint8_t  reserved[3];
    uint8_t  crc[2];
} config_record_t;

#define CONFIG_RECORD_SIZE   ((uint16_t)sizeof(config_record_t))   /* 17 */

static void DefaultParams(config_param_t *p)
{
    p->slave_id      = 1U;
    p->baud_idx      = MB_BAUD_IDX_115200;
    p->temp_limit    = 500U;   /* 50.0 C  */
    p->volt_limit    = 1600U;  /* 16.00 V */
    p->sample_period = 500U;   /* ms      */
    p->dev_mode      = 0U;
}

static void RecordFromParams(config_record_t *rec, const config_param_t *p)
{
    memset(rec, 0, sizeof(*rec));
    rec->magic[0] = (uint8_t)(CONFIG_MAGIC >> 8);
    rec->magic[1] = (uint8_t)(CONFIG_MAGIC & 0xFF);
    rec->version  = CONFIG_VERSION;
    rec->slave_id = p->slave_id;
    rec->baud_idx = p->baud_idx;
    rec->temp_limit[0] = (uint8_t)(p->temp_limit >> 8);
    rec->temp_limit[1] = (uint8_t)(p->temp_limit & 0xFF);
    rec->volt_limit[0] = (uint8_t)(p->volt_limit >> 8);
    rec->volt_limit[1] = (uint8_t)(p->volt_limit & 0xFF);
    rec->sample_period[0] = (uint8_t)(p->sample_period >> 8);
    rec->sample_period[1] = (uint8_t)(p->sample_period & 0xFF);
    rec->dev_mode = p->dev_mode;
}

static void ParamsFromRecord(config_param_t *p, const config_record_t *rec)
{
    p->slave_id      = rec->slave_id;
    p->baud_idx      = rec->baud_idx;
    p->temp_limit    = (uint16_t)((rec->temp_limit[0] << 8) | rec->temp_limit[1]);
    p->volt_limit    = (uint16_t)((rec->volt_limit[0] << 8) | rec->volt_limit[1]);
    p->sample_period = (uint16_t)((rec->sample_period[0] << 8) | rec->sample_period[1]);
    p->dev_mode      = rec->dev_mode;
}

static uint8_t RecordValid(const config_record_t *rec)
{
    uint16_t crc;
    uint16_t magic = (uint16_t)((rec->magic[0] << 8) | rec->magic[1]);
    const uint8_t *raw = (const uint8_t *)rec;

    if (magic != CONFIG_MAGIC)
    {
        return 0U;
    }
    if (rec->version != CONFIG_VERSION)
    {
        return 0U;
    }
    if ((rec->slave_id < 1U) || (rec->slave_id > 247U))
    {
        return 0U;
    }
    if (rec->baud_idx > MB_BAUD_IDX_MAX)
    {
        return 0U;
    }
    /* CRC covers all bytes before the CRC field */
    crc = MODBUS_CRC16(raw, (uint16_t)(CONFIG_RECORD_SIZE - 2U));
    if ((uint8_t)(crc & 0xFF) != rec->crc[0])
    {
        return 0U;
    }
    if ((uint8_t)((crc >> 8) & 0xFF) != rec->crc[1])
    {
        return 0U;
    }
    return 1U;
}

/*====================================================================*/
/* Public API                                                          */
/*====================================================================*/
uint8_t CONFIG_Init(config_param_t *out)
{
    config_record_t rec;

    if (EEPROM_Init() != 0)
    {
        DefaultParams(out);
        s_loaded = 0U;
        return 0U;
    }

    if ((EEPROM_ReadBytes(CONFIG_EEPROM_BASE, (uint8_t *)&rec,
                          CONFIG_RECORD_SIZE) == 0) &&
        RecordValid(&rec))
    {
        ParamsFromRecord(out, &rec);
        s_loaded = 1U;
    }
    else
    {
        /* first boot or damaged record -> defaults + write back */
        DefaultParams(out);
        (void)CONFIG_Save(out);
        s_loaded = 0U;
    }
    return s_loaded;
}

uint8_t CONFIG_Save(const config_param_t *p)
{
    config_record_t rec;
    uint16_t crc;
    const uint8_t *raw;

    RecordFromParams(&rec, p);
    raw = (const uint8_t *)&rec;
    crc = MODBUS_CRC16(raw, (uint16_t)(CONFIG_RECORD_SIZE - 2U));
    rec.crc[0] = (uint8_t)(crc & 0xFF);
    rec.crc[1] = (uint8_t)((crc >> 8) & 0xFF);

    if (EEPROM_WriteBytes(CONFIG_EEPROM_BASE, raw, CONFIG_RECORD_SIZE) != 0)
    {
        return 0U;
    }
    s_loaded = 1U;
    return 1U;
}

void CONFIG_ApplyToRegisters(const config_param_t *p)
{
    MB_REG_SetHolding(MB_REG_HOLD_TEMP_LIMIT,    p->temp_limit);
    MB_REG_SetHolding(MB_REG_HOLD_VOLT_LIMIT,    p->volt_limit);
    MB_REG_SetHolding(MB_REG_HOLD_SAMPLE_PERIOD, p->sample_period);
    MB_REG_SetHolding(MB_REG_HOLD_DEV_MODE,      p->dev_mode);
    MB_REG_SetHolding(MB_REG_HOLD_SLAVE_ID,      p->slave_id);
    MB_REG_SetHolding(MB_REG_HOLD_BAUD_IDX,      p->baud_idx);
}

void CONFIG_ReadFromRegisters(config_param_t *p)
{
    p->temp_limit    = MB_REG_GetHolding(MB_REG_HOLD_TEMP_LIMIT);
    p->volt_limit    = MB_REG_GetHolding(MB_REG_HOLD_VOLT_LIMIT);
    p->sample_period = MB_REG_GetHolding(MB_REG_HOLD_SAMPLE_PERIOD);
    p->dev_mode      = (uint8_t)MB_REG_GetHolding(MB_REG_HOLD_DEV_MODE);
    p->slave_id      = (uint8_t)MB_REG_GetHolding(MB_REG_HOLD_SLAVE_ID);
    p->baud_idx      = (uint8_t)MB_REG_GetHolding(MB_REG_HOLD_BAUD_IDX);
}
