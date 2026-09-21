/**
  ******************************************************************************
  * @file    bsp_eeprom.h
  * @brief   AT24C02 (256 bytes, I2C) driver - BSP self-init style.
  ******************************************************************************
  */
#ifndef __BSP_EEPROM_H
#define __BSP_EEPROM_H

#include <stdint.h>

/** Init I2C1 on PA15(SCL)/PB9(SDA), 100 kHz. Call once. */
int32_t EEPROM_Init(void);

/**
  * @brief  Write len bytes to EEPROM address (handles 8-byte page limit
  *         and write-cycle timing internally).
  * @param  addr  EEPROM byte address 0..255
  * @param  data
  * @param  len   must stay inside 0..255
  * @retval 0 ok, -1 error
  */
int32_t EEPROM_WriteBytes(uint16_t addr, const uint8_t *data, uint16_t len);

/**
  * @brief  Read len bytes from EEPROM address.
  * @retval 0 ok, -1 error
  */
int32_t EEPROM_ReadBytes(uint16_t addr, uint8_t *data, uint16_t len);

#endif /* __BSP_EEPROM_H */
