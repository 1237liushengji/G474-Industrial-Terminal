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
/* Holding register address map - 64 regs, PDU 0x00..0x3F (docs/03 §3)  */
/*   0x00-0x08 live measurements / status / counters      RO [v1 frozen] */
/*   0x09-0x0E field-editable config (24C02 persisted)    RW [v1 frozen] */
/*   0x0F      sentinel hole - ALWAYS exception 0x02, read and write,   */
/*             incl. any block access whose range crosses it            */
/*   0x10-0x17 gateway / CAN backbone counters            RO   (v0.3)   */
/*   0x18-0x1F contract metadata                          RO   (v0.3)   */
/*   0x20-0x2F extended measurements                      RO   (v0.3)   */
/*   0x30-0x3F downlink command shadow (not persisted)    RW   (v0.3)   */
/*   0x38-0x3F reserved control: exist but appear in NO projection      */
/*====================================================================*/

/*---- 0x00-0x0E: the v1-frozen block (semantics unchanged since v2.0) --*/
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

#define MB_REG_HOLD_HOLE          0x0F   /* 40016 sentinel: always 0x02 */

/*---- 0x10-0x17: gateway / CAN backbone counters (RO) -----------------*/
#define MB_REG_HOLD_CAN_TX_ACK    0x10   /* 40017 CAN frames acked      */
#define MB_REG_HOLD_CAN_RX_OVR    0x11   /* 40018 CAN rx overruns       */
#define MB_REG_HOLD_CAN_BUSOFF    0x12   /* 40019 CAN bus-off count     */
#define MB_REG_HOLD_CAN_TX_LAT    0x13   /* 40020 CAN tx max latency ms */
#define MB_REG_HOLD_MB_TIMEOUT    0x14   /* 40021 Modbus timeouts       */
#define MB_REG_HOLD_MB_RETRY      0x15   /* 40022 Modbus retries        */
#define MB_REG_HOLD_MB_EXCEPTION  0x16   /* 40023 Modbus exceptions     */
#define MB_REG_HOLD_LINK_MASK     0x17   /* 40024 bit0=485 1=CAN 2=WiFi 3=MQTT */

/*---- 0x18-0x1F: contract metadata (RO) -------------------------------*/
#define MB_REG_HOLD_CONTRACT_HASH 0x18   /* 40025 CRC16 of dp_table     */
#define MB_REG_HOLD_CONTRACT_VER  0x19   /* 40026 contract version      */
#define MB_REG_HOLD_FORM_MODE     0x1A   /* 40027 1=M1 2=M2             */
#define MB_REG_HOLD_FW_VER        0x1B   /* 40028 BCD, 1.0.0 -> 0x100   */
#define MB_REG_HOLD_HB_SEQ        0x1C   /* 40029 heartbeat/publish seq */
#define MB_REG_HOLD_UPTIME_S      0x1D   /* 40030 uptime seconds (low)  */
#define MB_REG_HOLD_FREE_HEAP     0x1E   /* 40031 FreeRTOS free heap    */
#define MB_REG_HOLD_TASK_OVR      0x1F   /* 40032 task starvation count */

/*---- 0x20-0x2F: extended measurements (RO) ---------------------------*/
#define MB_REG_HOLD_ADC1_RAW      0x20   /* 40033 ADC1 channel raw      */
#define MB_REG_HOLD_MCU_TEMP      0x21   /* 40034 MCU temp 0.1C         */
#define MB_REG_HOLD_VREF_MV       0x22   /* 40035 VREFINT mV            */
#define MB_REG_HOLD_RSV_MEAS_FIRST 0x23  /* 40036 .. 13 reserved slots  */
#define MB_REG_HOLD_RSV_MEAS_COUNT 13U

/*---- 0x30-0x3F: downlink command shadow (RW, not persisted) ----------*/
#define MB_REG_HOLD_CMD_WRITE_REG 0x30   /* 40049 target register addr  */
#define MB_REG_HOLD_CMD_WRITE_VAL 0x31   /* 40050 value to write        */
#define MB_REG_HOLD_CMD_ID        0x32   /* 40051 transaction id        */
#define MB_REG_HOLD_CMD_RESULT    0x33   /* 40052 0=none 1=ok 2=exc     */
#define MB_REG_HOLD_CMD_EXCCODE   0x34   /* 40053 exception code        */
#define MB_REG_HOLD_RSV_CTRL_FIRST 0x35  /* 40054 .. 11 reserved slots  */
#define MB_REG_HOLD_RSV_CTRL_COUNT 11U   /* 0x35..0x3F                  */

#define MB_REG_HOLD_CMD_FIRST     0x30
#define MB_REG_HOLD_CMD_LAST      0x3F

/*---- boundaries ------------------------------------------------------*/
#define MB_REG_HOLD_COUNT         64U
#define MB_REG_HOLD_MAX_ADDR      (MB_REG_HOLD_COUNT - 1U)
#define MB_REG_HOLD_CFG_FIRST     MB_REG_HOLD_TEMP_LIMIT   /* writable start */
#define MB_REG_HOLD_CFG_LAST      MB_REG_HOLD_BAUD_IDX
#define MB_REG_HOLD_CFG_COUNT     6U

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

/**
  * @brief  Write WITHOUT the config-dirty side effect (gateway mirror only).
  * @note   The A board mirrors the node's 0x00-0x0F/0x20-0x22 into its own
  *         bank (Gateway/bridge.c).  Using MB_REG_SetHolding() for 0x09-0x0E
  *         would raise the persist-dirty flag on the gateway, which has no
  *         business persisting the node's config.  Firmware-internal writes
  *         to RO tiles (stats/meta) also come through here.
  */
void     MB_REG_SetHoldingRaw(uint16_t addr, uint16_t value);

uint8_t  MB_REG_HoldingWritable(uint16_t addr);

/**
  * @brief  Range check for a block holding-register access (FC03).
  * @param  start  first PDU address, qty number of registers
  * @retval 1 valid, 0 -> caller answers exception 0x02
  * @note   Rejects: out of 0x00..0x3F AND any range that INTERSECTS the
  *         0x0F sentinel hole (keeps the v2.0 behaviour that a block read
  *         crossing 0x0F is an illegal address - golden vector E3).
  */
uint8_t  MB_REG_HoldRangeValid(uint16_t start, uint16_t qty);

/** Input registers */
uint16_t MB_REG_GetInput(uint16_t addr);
void     MB_REG_SetInput(uint16_t addr, uint16_t value);

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
  * @brief  Semantic range check for a register write (FC06/FC10).
  * @param  addr   register address inside a writable region
  * @param  value  proposed value
  * @retval 1 valid / 0 rejected
  * @note   Config region 0x09-0x0E: SlaveID 1..247, BaudIdx 0..4
  *         (9600..115200), TempLimit/VoltLimit nonzero,
  *         SamplePeriod 10..60000, DeviceMode 0..3.  Writes outside these
  *         ranges would brick the node (unreachable slave id / invalid
  *         baud), so the protocol layer checks this before accepting.
  *         Command shadow region 0x30-0x3F: any u16 value is valid -
  *         range semantics belong to the cmd executor, not the register.
  */
uint8_t MB_REG_ValidateConfigValue(uint16_t addr, uint16_t value);

/** Default config values used when EEPROM is empty (v0.9+ loads real) */
uint8_t  MB_REG_GetSlaveId(void);
uint16_t MB_REG_GetBaudIdx(void);

#endif /* __MODBUS_REGISTER_H */
