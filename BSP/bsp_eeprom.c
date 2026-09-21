/**
  ******************************************************************************
  * @file    bsp_eeprom.c
  * @brief   AT24C02 driver over I2C1 (PA15=SCL, PB9=SDA, AF4).
  *
  *  AT24C02 facts:
  *   - 256 x 8 bit, 8-byte write page, write cycle ~5 ms
  *   - device address 0xA0 (A2A1A0 = 000 on board)
  *   - no auto-increment across page boundary on write
  ******************************************************************************
  */
#include "bsp_eeprom.h"
#include "bsp_board_cfg.h"

/* add EEPROM pins to the board config header namespace */
#ifndef EEPROM_I2C
#define EEPROM_I2C              I2C1
#define EEPROM_I2C_CLK_ENABLE() __HAL_RCC_I2C1_CLK_ENABLE()
#define EEPROM_SCL_PORT         GPIOA
#define EEPROM_SCL_PIN          GPIO_PIN_15
#define EEPROM_SDA_PORT         GPIOB
#define EEPROM_SDA_PIN          GPIO_PIN_9
#define EEPROM_GPIO_CLK_ENABLE() \
    do { __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOB_CLK_ENABLE(); } while (0)
#define EEPROM_I2C_AF           GPIO_AF4_I2C1
#endif

#define EEPROM_DEV_ADDR         (0xA0U >> 1)   /* 7-bit: 0x50 */
#define EEPROM_PAGE_SIZE        8U
#define EEPROM_SIZE             256U
#define EEPROM_WRITE_CYCLE_MS   5U

static I2C_HandleTypeDef s_hi2c;

/**
  * @brief  Compute I2C_TIMINGR for standard mode (100 kHz, conservative
  *         ~90 kHz effective) from the actual PCLK1.
  *
  *  G4 TIMINGR layout (see stm32g474xx.h):
  *     PRESC[31:28] SCLDEL[23:20] SDADEL[19:16] SCLH[15:8] SCLL[7:0]
  *  SCLDEL/SDADEL are 4-bit, SCLH/SCLL are 8-bit.
  *
  *  Reference model (RM0440 I2C timing):
  *     tI2CCLK = (PRESC+1) / fPCLK1
  *     tSCLL   = (SCLL+1)  * tI2CCLK ; tSCLH = (SCLH+1) * tI2CCLK
  *  We pick PRESC so the I2C kernel clock is ~5 MHz, then set
  *  SCLH/SCLL for ~90 kHz (a safe margin below the 100 kHz limit).
  */
static uint32_t I2C_ComputeTiming(uint32_t pclk_hz)
{
    uint32_t presc;
    uint32_t scll, sclh;
    uint32_t sdad, scld;
    uint64_t t_iclk;   /* I2C kernel period, in ps for precision */

    if (pclk_hz == 0U)
    {
        pclk_hz = 150000000U;   /* fallback */
    }

    /* PRESC: kernel clock ~5 MHz  -> tI2CCLK = 200 ns @150MHz */
    presc = (pclk_hz / 5000000U) - 1U;
    if (presc > 0x0FU)
    {
        presc = 0x0FU;
    }
    t_iclk = 1000000000000ULL / pclk_hz * (uint64_t)(presc + 1U);

    /* SCL ~90 kHz -> half period ~5.56 us */
    scll = (uint32_t)((5560000ULL / t_iclk) - 1U);
    sclh = scll;
    if (scll > 0xFFU)
    {
        scll = 0xFFU;
        sclh = 0xFFU;
    }
    /* SDADEL ~ 0.6 us, SCLDEL ~ 1.2 us (both 4-bit fields) */
    sdad = (uint32_t)(600000ULL / t_iclk);
    scld = (uint32_t)(1200000ULL / t_iclk);
    if (sdad > 0x0FU) sdad = 0x0FU;
    if (scld > 0x0FU) scld = 0x0FU;

    return (presc << I2C_TIMINGR_PRESC_Pos) |
           (scld << I2C_TIMINGR_SCLDEL_Pos) |
           (sdad << I2C_TIMINGR_SDADEL_Pos) |
           (sclh << I2C_TIMINGR_SCLH_Pos) |
           (scll << I2C_TIMINGR_SCLL_Pos);
}

int32_t EEPROM_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    EEPROM_GPIO_CLK_ENABLE();
    EEPROM_I2C_CLK_ENABLE();

    /* SCL: open-drain AF, pull-up (board may also have external PU) */
    gpio.Pin       = EEPROM_SCL_PIN;
    gpio.Mode      = GPIO_MODE_AF_OD;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = EEPROM_I2C_AF;
    HAL_GPIO_Init(EEPROM_SCL_PORT, &gpio);

    gpio.Pin       = EEPROM_SDA_PIN;
    HAL_GPIO_Init(EEPROM_SDA_PORT, &gpio);

    s_hi2c.Instance             = EEPROM_I2C;
    s_hi2c.Init.Timing           = I2C_ComputeTiming(HAL_RCC_GetPCLK1Freq());
    s_hi2c.Init.OwnAddress1      = 0U;
    s_hi2c.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
    s_hi2c.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
    s_hi2c.Init.OwnAddress2      = 0U;
    s_hi2c.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
    s_hi2c.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;

    if (HAL_I2C_Init(&s_hi2c) != HAL_OK)
    {
        return -1;
    }
    return 0;
}

static int32_t EEPROM_PollWriteDone(void)
{
    /* The AT24C02 starts its internal write cycle right after the page
       write; during that cycle it does not ACK. Poll for ACK with a
       generous timeout instead of a blind fixed delay - this both
       shortens the wait on fast chips and is safe on slow/cold ones. */
    uint32_t deadline = HAL_GetTick() + 50U;   /* 50 ms worst case */

    HAL_Delay(1U);   /* let the write cycle at least start */
    while (HAL_I2C_IsDeviceReady(&s_hi2c, EEPROM_DEV_ADDR, 1, 10U)
           != HAL_OK)
    {
        if (HAL_GetTick() >= deadline)
        {
            return -1;   /* device never ACKed - hardware problem */
        }
    }
    return 0;
}

int32_t EEPROM_WriteBytes(uint16_t addr, const uint8_t *data, uint16_t len)
{
    uint16_t done = 0U;

    if ((addr >= EEPROM_SIZE) || ((uint32_t)addr + len > EEPROM_SIZE))
    {
        return -1;
    }

    while (done < len)
    {
        /* bytes remaining in current 8-byte page */
        uint16_t page_room = (uint16_t)(EEPROM_PAGE_SIZE - (addr % EEPROM_PAGE_SIZE));
        uint16_t chunk = len - done;
        HAL_StatusTypeDef st;

        if (chunk > page_room)
        {
            chunk = page_room;
        }

        st = HAL_I2C_Mem_Write(&s_hi2c, EEPROM_DEV_ADDR, addr,
                               I2C_MEMADD_SIZE_8BIT,
                               (uint8_t *)(data + done), chunk, 100U);
        if (st != HAL_OK)
        {
            return -1;
        }
        if (EEPROM_PollWriteDone() != 0)
        {
            return -1;   /* write cycle never finished */
        }

        addr += chunk;
        done += chunk;
    }
    return 0;
}

int32_t EEPROM_ReadBytes(uint16_t addr, uint8_t *data, uint16_t len)
{
    if ((addr >= EEPROM_SIZE) || ((uint32_t)addr + len > EEPROM_SIZE))
    {
        return -1;
    }
    if (HAL_I2C_Mem_Read(&s_hi2c, EEPROM_DEV_ADDR, addr,
                         I2C_MEMADD_SIZE_8BIT, data, len, 100U) != HAL_OK)
    {
        return -1;
    }
    return 0;
}
