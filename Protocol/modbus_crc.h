/**
  ******************************************************************************
  * @file    modbus_crc.h
  * @brief   CRC16 (Modbus) implementation - self contained, platform free.
  * @note    Polynomial 0x8005 reflected = 0xA001, init 0xFFFF.
  *          Frame CRC bytes are appended low byte first.
  ******************************************************************************
  */
#ifndef __MODBUS_CRC_H
#define __MODBUS_CRC_H

#include <stdint.h>

/**
  * @brief  Bit-by-bit CRC16 (Modbus). Slow, small, reference implementation.
  * @param  data  buffer (address + PDU, WITHOUT crc)
  * @param  len
  * @retval CRC value (16-bit)
  */
uint16_t MODBUS_CRC16_Bitwise(const uint8_t *data, uint16_t len);

/**
  * @brief  Table-driven CRC16 (Modbus). Fast version, preferred on MCU.
  */
uint16_t MODBUS_CRC16_Table(const uint8_t *data, uint16_t len);

/** Alias used by protocol layer (select fastest). */
#define MODBUS_CRC16(data, len)   MODBUS_CRC16_Table((data), (len))

/*---------------------------------------------------------------------------
 * Streaming (incremental) table-driven CRC - v0.3.
 * Used by Common/dp_table.c to hash the contract description string chunk by
 * chunk (row by row) without materialising the whole string in RAM.
 * Identical result to MODBUS_CRC16_Table(whole_buffer, total_len):
 *     crc = MODBUS_CRC16_Init();
 *     crc = MODBUS_CRC16_Update(crc, chunk1, len1);
 *     crc = MODBUS_CRC16_Update(crc, chunk2, len2);
 *     value = crc;                       // MODBUS_CRC16_Init/Update/Final
 *-------------------------------------------------------------------------*/

/** @brief Seed value for a new stream (0xFFFF). */
uint16_t MODBUS_CRC16_Init(void);

/** @brief Feed a chunk into the stream; returns the running value. */
uint16_t MODBUS_CRC16_Update(uint16_t crc, const uint8_t *data, uint16_t len);

/** @brief Feed one byte into the stream; returns the running value. */
uint16_t MODBUS_CRC16_UpdateByte(uint16_t crc, uint8_t byte);

/**
  * @brief  Append CRC (low byte first) to a frame buffer.
  * @param  frame  buffer containing [addr][func][data], room for +2 bytes
  * @param  len    length WITHOUT crc
  * @retval new total length (= len + 2)
  */
uint16_t MODBUS_CRC_Append(uint8_t *frame, uint16_t len);

/**
  * @brief  Check CRC of a received frame.
  * @param  frame  buffer [addr][func][data][crcLo][crcHi]
  * @param  len    total length INCLUDING crc
  * @retval 1 valid, 0 invalid
  */
uint8_t MODBUS_CRC_Check(const uint8_t *frame, uint16_t len);

#endif /* __MODBUS_CRC_H */
