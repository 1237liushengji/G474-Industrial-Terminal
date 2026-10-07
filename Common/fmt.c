/**
  ******************************************************************************
  * @file    fmt.c
  * @brief   Integer-only fixed-point formatting (docs/03 §5.2).
  ******************************************************************************
  */
#include "fmt.h"

/** Unsigned decimal, no padding. Returns digits written. */
static int PutU32(char *out, uint32_t v)
{
    char tmp[10];
    int  i = 0;
    int  n = 0;

    if (v == 0U)
    {
        tmp[i] = '0';
        i++;
    }
    while (v > 0U)
    {
        tmp[i] = (char)('0' + (v % 10U));
        i++;
        v /= 10U;
    }
    while (i > 0)
    {
        i--;
        out[n] = tmp[i];
        n++;
    }
    return n;
}

int fmt_fixed(char *out, int32_t raw, uint8_t s)
{
    uint32_t mag;
    int      n = 0;

    if (out == 0)
    {
        return -1;
    }
    if (s > 2U)
    {
        s = 0U;
    }

    if (raw < 0)
    {
        out[n] = '-';
        n++;
        /* two's-complement magnitude: safe for INT32_MIN as well */
        mag = (uint32_t)(~((uint32_t)raw)) + 1U;
    }
    else
    {
        mag = (uint32_t)raw;
    }

    if (s == 0U)
    {
        n += PutU32(&out[n], mag);
    }
    else if (s == 1U)
    {
        n += PutU32(&out[n], mag / 10U);
        out[n] = '.';
        n++;
        out[n] = (char)('0' + (mag % 10U));
        n++;
    }
    else /* s == 2 */
    {
        uint32_t fp = mag % 100U;

        n += PutU32(&out[n], mag / 100U);
        out[n] = '.';
        n++;
        out[n] = (char)('0' + (fp / 10U));
        n++;
        out[n] = (char)('0' + (fp % 10U));
        n++;
    }

    out[n] = '\0';
    return n;
}
