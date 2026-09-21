/**
  ******************************************************************************
  * @file    modbus_register.h
  * @brief   Register bank for the Modbus slave (single source of truth)
  * @note    Addresses here are PDU addresses (0-based).
  *          See docs/03-寄存器映射表.md - the ONLY contract.
  ******************************************************************************
  */
#ifndef __MODBUS_REGISTER_H
#define __MODBUS_REGISTER_H

#include <stdint.h>

/*====================================================================*/
/* Holding register address map (15 regs, addr 0x00..0x0E)             */
/*====================================================================*/
#define MB_REG_HOLD_TEMP          0x00   /* 40001 0.1C   RO */
#define MB_REG_HOLD_HUMI          0x01   /* 40002 0.1%RH RO */
#define MB_REG_HOLD_VOLT          0x02   /* 40003 0.01V  RO */
#define MB_REG_HOLD_CURR          0x03   /* 40004 0.01A  RO */
#define MB_REG_HOLD_STATUS        0x04   /* 40005 device status */
#define MB_REG_HOLD_ERRCODE       0x05   /* 40006 last error code */
#define MB_REG_HOLD_RXCNT         0x06   /* 40007 RX frames  RO */
#define MB_REG_HOLD_TXCNT         0x07   /* 40008 TX frames  RO */
#define MB_REG_HOLD_CRCERR        0x08   /* 40009 CRC errors RO */
#define MB_REG_HOLD_TEMP_LIMIT    0x09   /* 40010 0.1C   RW config */
#define MB_REG_HOLD_VOLT_LIMIT    0x0A   /* 40011 0.01V  RW config */
#define MB_REG_HOLD_SAMPLE_PERIOD 0x0B   /* 40012 ms     RW config */
#define MB_REG_HOLD_DEV_MODE      0x0C   /* 40013 mode   RW config */
#define MB_REG_HOLD_SLAVE_ID      0x0D   /* 40014 addr   RW config */
#define MB_REG_HOLD_BAUD_IDX      0x0E   /* 40015 baud table idx RW config */

#define MB_REG_HOLD_COUNT         15U
#define MB_REG_HOLD_MAX_ADDR      (MB_REG_HOLD_COUNT - 1U)
#define MB_REG_HOLD_CFG_FIRST     MB_REG_HOLD_TEMP_LIMIT   /* writable start */

/*====================================================================*/
/* Input register map (0x04 read)                                       */
/*====================================================================*/
#define MB_REG_IN_ADC1            0x00   /* 30001 */
#define MB_REG_IN_ADC2            0x01   /* 30002 */
#define MB_REG_IN_MCU_TEMP        0x02   /* 30003 0.1C */
#define MB_REG_IN_VREFINT         0x03   /* 30004 */
#define MB_REG_IN_COUNT           4U

/*====================================================================*/
/* Coil map (0x01 read / 0x05 write single)                             */
/*====================================================================*/
#define MB_REG_COIL_FAN           0x00   /* 00001 */
#define MB_REG_COIL_RELAY         0x01   /* 00002 */
#define MB_REG_COIL_LED_BLUE      0x02   /* 00003 */
#define MB_REG_COIL_LED_GREEN     0x03   /* 00004 */
#define MB_REG_COIL_COUNT         4U

/*====================================================================*/
/* Baudrate table index -> value (40015)                                */
/*   Valid range 0..4 = 9600..115200. 230400 was removed because the    */
/*   on-board RS485 transceiver is not rated for it (see docs/03).      */
/*====================================================================*/
#define MB_BAUD_IDX_9600          0U
#define MB_BAUD_IDX_19200         1U
#define MB_BAUD_IDX_38400         2U
#define MB_BAUD_IDX_57600         3U
#define MB_BAUD_IDX_115200        4U
#define MB_BAUD_IDX_MAX           MB_BAUD_IDX_115200

uint32_t MB_REG_BaudFromIdx(uint8_t idx);   /* 0 -> default 115200 */

/*====================================================================*/
/* API                                                                 */
/*====================================================================*/

/** Init bank with default values (config load is hooked later). */
void MB_REG_Init(void);

/** Holding registers */
uint16_t MB_REG_GetHolding(uint16_t addr);
void     MB_REG_SetHolding(uint16_t addr, uint16_t value);
uint8_t  MB_REG_HoldingWritable(uint16_t addr);

/** Input registers */
uint16_t MB_REG_GetInput(uint16_t addr);

/** Coils (bit access) */
uint8_t  MB_REG_GetCoil(uint16_t addr);
void     MB_REG_SetCoil(uint16_t addr, uint8_t on);

/** Statistics counters (auto incremented by protocol engine) */
void MB_REG_IncRxCount(void);
void MB_REG_IncTxCount(void);
void MB_REG_IncCrcError(void);
void MB_REG_SetLastError(uint16_t err);
void MB_REG_SetDeviceStatus(uint16_t st);

/** Config-region write flag: set when a 40010..40015 register is written
 *  through MB_REG_SetHolding; the app loop persists it to EEPROM. */
uint8_t MB_REG_ConfigDirty(void);
void    MB_REG_ClearConfigDirty(void);

/**
  * @brief  Semantic range check for a config register write.
  * @param  addr   config register address (40010..40015)
  * @param  value  proposed value
  * @retval 1 valid / 0 rejected
  * @note   Limits: SlaveID 1..247, BaudIdx 0..4 (9600..115200),
  *         TempLimit/VoltLimit nonzero, SamplePeriod 10..60000,
  *         DeviceMode 0..3. Writes outside these ranges would brick the
  *         node (unreachable slave id / invalid baud), so the protocol
  *         layer checks this before accepting 0x06/0x10.
  */
uint8_t MB_REG_ValidateConfigValue(uint16_t addr, uint16_t value);

/** Default config values used when EEPROM is empty (v0.9+ loads real) */
uint8_t  MB_REG_GetSlaveId(void);
uint16_t MB_REG_GetBaudIdx(void);

#endif /* __MODBUS_REGISTER_H */
