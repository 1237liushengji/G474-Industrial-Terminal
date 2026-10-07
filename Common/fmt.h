/**
  ******************************************************************************
  * @file    fmt.h
  * @brief   Integer-only fixed-point formatting (docs/03 §5.2).
  ******************************************************************************
  */
#ifndef __FMT_H
#define __FMT_H

#include <stdint.h>

/**
  * @brief  Format raw/10^s as a decimal string, pure integer arithmetic.
  * @param  out  destination buffer (>= 16 bytes recommended)
  * @param  raw  scaled value (two's complement, negatives supported)
  * @param  s    decimal digits, 0/1/2 (other values are treated as 0)
  * @retval bytes written (excluding the NUL), or -1 on bad arguments
  *
  *  286,1 -> "28.6"    1208,2 -> "12.08"    0,1 -> "0.0"    -50,1 -> "-5.0"
  *
  *  Rationale: newlib's %f drags in the floating-point formatter (several KB
  *  of flash, unbounded latency).  Every scale!=0 field of the MQTT payload
  *  goes through this function; the PC side compares strings for EXACT
  *  equality (no float tolerance - tolerance would hide scale bugs).
  */
int fmt_fixed(char *out, int32_t raw, uint8_t s);

#endif /* __FMT_H */
