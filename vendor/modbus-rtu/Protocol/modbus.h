/**
  ******************************************************************************
  * @file    modbus.h
  * @brief   Common Modbus RTU definitions (platform independent)
  * @note    Function codes, exception codes, frame limits per spec V1.1b3
  ******************************************************************************
  */
#ifndef __MODBUS_H
#define __MODBUS_H

#include <stdint.h>

/*====================================================================*/
/* Function codes (implemented)                                        */
/*====================================================================*/
#define MB_FC_READ_COILS            0x01
#define MB_FC_READ_INPUT_REGS       0x04
#define MB_FC_WRITE_SINGLE_COIL     0x05
#define MB_FC_READ_HOLDING_REGS     0x03
#define MB_FC_WRITE_SINGLE_REG      0x06
#define MB_FC_WRITE_MULTI_REGS      0x10

/*====================================================================*/
/* Exception codes                                                     */
/*====================================================================*/
#define MB_EX_ILLEGAL_FUNCTION      0x01
#define MB_EX_ILLEGAL_ADDRESS       0x02
#define MB_EX_ILLEGAL_VALUE         0x03
#define MB_EX_SLAVE_FAILURE         0x04

/*====================================================================*/
/* Addresses                                                           */
/*====================================================================*/
#define MB_ADDR_BROADCAST           0x00
#define MB_ADDR_MIN                 0x01
#define MB_ADDR_MAX                 0xF7   /* 247 */

/*====================================================================*/
/* Frame / buffer limits                                               */
/*====================================================================*/
#define MB_RTU_MAX_PDU_LEN          253U
#define MB_RTU_MAX_FRAME_LEN        (1U + MB_RTU_MAX_PDU_LEN + 2U)  /* 256 */
#define MB_RTU_CRC_LEN              2U
#define MB_RTU_HEADER_LEN           2U      /* addr + func */

/* Broadcast write to slave 0 is executed but never answered (V1.x).   */
#define MB_FEATURE_BROADCAST_WRITE  0U

/*====================================================================*/
/* Frame timing helpers (us)                                           */
/* t3.5 = 3.5 char times; char = 11 bits (start+8data+stop, no parity) */
/*   t3.5 [us] = 3.5 * 11 * 1e6 / baud = 38.5e6 / baud                */
/*====================================================================*/
static inline uint32_t MB_T35_US(uint32_t baud)
{
    return (baud == 0U) ? 10000U : (uint32_t)(38500000UL / baud);
}

#endif /* __MODBUS_H */
