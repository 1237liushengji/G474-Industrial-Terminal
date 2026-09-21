/**
  ******************************************************************************
  * @file    config.h
  * @brief   Parameter persistence layer on 24C02 (Storage layer).
  *
  *  config_param_t is the in-RAM working copy (this header).
  *  On-media layout is the packed 17-byte record defined in config.c
  *  (see its comment; offsets derive from sizeof, never hard-code).
  ******************************************************************************
  */
#ifndef __CONFIG_H
#define __CONFIG_H

#include <stdint.h>
#include "modbus_register.h"   /* register address map (shared contract) */

#define CONFIG_MAGIC           0x4D42U   /* "MB" */
#define CONFIG_VERSION          1U
#define CONFIG_EEPROM_BASE      0x00U    /* base offset inside 24C02 */

typedef struct {
    uint8_t  slave_id;          /* 1..247 */
    uint8_t  baud_idx;          /* 0..4 (9600..115200) */
    uint16_t temp_limit;        /* 0.1 C   */
    uint16_t volt_limit;        /* 0.01 V  */
    uint16_t sample_period;     /* ms      */
    uint8_t  dev_mode;
    /* crc16 appended on media */
} config_param_t;

/**
  * @brief  Init storage: load params from EEPROM.
  *         On first boot / CRC failure, defaults are used and saved once.
  * @param  out  receives active parameters
  * @retval 1 loaded from EEPROM, 0 used defaults (fresh/corrupt)
  */
uint8_t CONFIG_Init(config_param_t *out);

/**
  * @brief  Save parameters to EEPROM with version+CRC header.
  * @retval 1 ok, 0 failed
  */
uint8_t CONFIG_Save(const config_param_t *p);

/**
  * @brief  Push parameter RAM values into the shared register bank
  *         (40010..40015). Call after CONFIG_Init.
  */
void CONFIG_ApplyToRegisters(const config_param_t *p);

/**
  * @brief  Pull current values from the register bank into a param struct.
  */
void CONFIG_ReadFromRegisters(config_param_t *p);

#endif /* __CONFIG_H */
