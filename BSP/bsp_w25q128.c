/**
  ******************************************************************************
  * @file    bsp_w25q128.c
  * @brief   W25Q128JV driver over SPI2 - this is the ON-BOARD flash (U3).
  *          CS = PB12 (GPIO soft NSS), SCK = PB13, MISO = PB14, MOSI = PB15, AF5.
  *          BSP self-init, HAL SPI polling mode.
  * @note    Pins and instance come from app_board_cfg.h (FLASH_* macros).
  *          The original Modbus project drove this driver on SPI1/PA4-PA7, where
  *          there is no flash chip at all - which is why it never passed its
  *          on-hardware acceptance.  The chip really sits on SPI2.
  *
  *          A JEDEC ID check gates the whole driver: if the detected ID does not
  *          match FLASH_EXPECTED_JEDEC the driver goes inert (reads return 0xFF,
  *          writes and erases become no-ops, W25Q_IsPresent() returns 0) so callers
  *          such as Storage/log.c disable themselves instead of writing to an
  *          unknown device.
  ******************************************************************************
  */
#include "bsp_w25q128.h"
#include "app_board_cfg.h"

/* W25Q commands */
#define W25Q_CMD_WRITE_ENABLE   0x06U
#define W25Q_CMD_WRITE_DISABLE  0x04U
#define W25Q_CMD_READ_STATUS    0x05U
#define W25Q_CMD_READ_DATA      0x03U
#define W25Q_CMD_PAGE_PROGRAM   0x02U
#define W25Q_CMD_SECTOR_ERASE   0x20U
#define W25Q_CMD_JEDEC_ID       0x9FU

static SPI_HandleTypeDef s_hspi;
static uint8_t           s_present = 0U;   /* set by W25Q_Init after the JEDEC check */

/*====================================================================*/
/* Init                                                                */
/*====================================================================*/
int32_t W25Q_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    uint32_t         id;

    FLASH_GPIO_CLK_ENABLE();
    FLASH_SPI_CLK_ENABLE();

    /* CS: GPIO output, idle high */
    gpio.Pin   = FLASH_CS_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(FLASH_CS_PORT, &gpio);
    HAL_GPIO_WritePin(FLASH_CS_PORT, FLASH_CS_PIN, GPIO_PIN_SET);

    /* SCK, MISO, MOSI : all on the same port in this design */
    gpio.Pin       = FLASH_SCK_PIN | FLASH_MISO_PIN | FLASH_MOSI_PIN;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = FLASH_SPI_AF;
    HAL_GPIO_Init(FLASH_SCK_PORT, &gpio);

    s_hspi.Instance               = FLASH_SPI_PERIPH;
    s_hspi.Init.Mode              = SPI_MODE_MASTER;
    s_hspi.Init.Direction         = SPI_DIRECTION_2LINES;
    s_hspi.Init.DataSize          = SPI_DATASIZE_8BIT;
    s_hspi.Init.CLKPolarity       = SPI_POLARITY_LOW;
    s_hspi.Init.CLKPhase          = SPI_PHASE_1EDGE;
    s_hspi.Init.NSS               = SPI_NSS_SOFT;
    /* The kernel clock is 150 MHz on this board (not 170) -> 150/16 = 9.375 MHz.
     * W25Q128 is good for 104 MHz reads, so there is plenty of margin. */
    s_hspi.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
    s_hspi.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    s_hspi.Init.TIMode            = SPI_TIMODE_DISABLE;
    s_hspi.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    s_hspi.Init.CRCPolynomial     = 7U;
    if (HAL_SPI_Init(&s_hspi) != HAL_OK)
    {
        s_present = 0U;
        return -1;
    }

    /* Identify the chip before trusting it: a wrong SPI bus, a missing chip or
     * an unpopulated footprint all show up here as a mismatched JEDEC ID. */
    id = W25Q_ReadID();
    if (id != FLASH_EXPECTED_JEDEC)
    {
        s_present = 0U;
        return -2;
    }

    s_present = 1U;
    return 0;
}

/*====================================================================*/
/* Low level                                                           */
/*====================================================================*/
static void CS_LOW(void)  { HAL_GPIO_WritePin(FLASH_CS_PORT, FLASH_CS_PIN, GPIO_PIN_RESET); }
static void CS_HIGH(void) { HAL_GPIO_WritePin(FLASH_CS_PORT, FLASH_CS_PIN, GPIO_PIN_SET); }

static uint8_t Xfer(uint8_t byte)
{
    uint8_t rx = 0U;
    HAL_SPI_TransmitReceive(&s_hspi, &byte, &rx, 1U, 100U);
    return rx;
}

static void WriteEnable(void)
{
    CS_LOW();
    Xfer(W25Q_CMD_WRITE_ENABLE);
    CS_HIGH();
}

uint8_t W25Q_IsBusy(void)
{
    uint8_t status;
    CS_LOW();
    Xfer(W25Q_CMD_READ_STATUS);
    status = Xfer(0xFFU);
    CS_HIGH();
    return (uint8_t)((status & 0x01U) ? 1U : 0U);
}

static void WaitIdle(void)
{
    uint32_t guard = 1000000U;
    while (W25Q_IsBusy() && (guard-- > 0U))
    {
    }
}

/*====================================================================*/
/* Public API                                                          */
/*====================================================================*/
uint32_t W25Q_ReadID(void)
{
    uint32_t id = 0U;

    CS_LOW();
    Xfer(W25Q_CMD_JEDEC_ID);
    id = (uint32_t)Xfer(0xFFU) << 16U;
    id |= (uint32_t)Xfer(0xFFU) << 8U;
    id |= (uint32_t)Xfer(0xFFU);
    CS_HIGH();
    return id;
}

uint8_t W25Q_IsPresent(void)
{
    return s_present;
}

void W25Q_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint32_t i;

    if (s_present == 0U)
    {
        for (i = 0U; i < len; i++)
        {
            buf[i] = 0xFFU;     /* what an erased cell would return */
        }
        return;
    }

    CS_LOW();
    Xfer(W25Q_CMD_READ_DATA);
    Xfer((uint8_t)(addr >> 16U));
    Xfer((uint8_t)(addr >> 8U));
    Xfer((uint8_t)(addr & 0xFFU));
    for (i = 0; i < len; i++)
    {
        buf[i] = Xfer(0xFFU);
    }
    CS_HIGH();
}

void W25Q_Write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint32_t done = 0U;

    if (s_present == 0U)
    {
        return;
    }

    while (done < len)
    {
        uint32_t page_room = W25Q_PAGE_SIZE - (addr % W25Q_PAGE_SIZE);
        uint32_t chunk = len - done;
        uint32_t i;

        if (chunk > page_room)
        {
            chunk = page_room;
        }

        WriteEnable();
        CS_LOW();
        Xfer(W25Q_CMD_PAGE_PROGRAM);
        Xfer((uint8_t)(addr >> 16U));
        Xfer((uint8_t)(addr >> 8U));
        Xfer((uint8_t)(addr & 0xFFU));
        for (i = 0; i < chunk; i++)
        {
            Xfer(buf[done + i]);
        }
        CS_HIGH();
        WaitIdle();

        addr += chunk;
        done += chunk;
    }
}

void W25Q_EraseSector(uint32_t addr)
{
    if (s_present == 0U)
    {
        return;
    }

    WriteEnable();
    CS_LOW();
    Xfer(W25Q_CMD_SECTOR_ERASE);
    Xfer((uint8_t)(addr >> 16U));
    Xfer((uint8_t)(addr >> 8U));
    Xfer((uint8_t)(addr & 0xFFU));
    CS_HIGH();
    WaitIdle();
}
