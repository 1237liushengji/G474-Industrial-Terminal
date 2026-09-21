/**
  ******************************************************************************
  * @file    bsp_w25q128.c
  * @brief   W25Q128JV driver over SPI1 (PA4 CS / PA5 SCK / PA6 MISO / PA7 MOSI)
  *          BSP self-init, HAL SPI polling mode.
  ******************************************************************************
  */
#include "bsp_w25q128.h"
#include "bsp_board_cfg.h"

/* W25Q commands */
#define W25Q_CMD_WRITE_ENABLE   0x06U
#define W25Q_CMD_WRITE_DISABLE  0x04U
#define W25Q_CMD_READ_STATUS    0x05U
#define W25Q_CMD_READ_DATA      0x03U
#define W25Q_CMD_PAGE_PROGRAM   0x02U
#define W25Q_CMD_SECTOR_ERASE   0x20U
#define W25Q_CMD_JEDEC_ID       0x9FU

static SPI_HandleTypeDef s_hspi;

/*====================================================================*/
/* Init                                                                */
/*====================================================================*/
int32_t W25Q_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    /* GPIO clocks + SPI1 clock */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_SPI1_CLK_ENABLE();

    /* CS: PA4 output high */
    gpio.Pin  = FLASH_CS_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(FLASH_CS_PORT, &gpio);
    HAL_GPIO_WritePin(FLASH_CS_PORT, FLASH_CS_PIN, GPIO_PIN_SET);

    /* SCK PA5, MISO PA6, MOSI PA7 : AF5 */
    gpio.Pin = FLASH_SCK_PIN | FLASH_MISO_PIN | FLASH_MOSI_PIN;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(FLASH_SCK_PORT, &gpio);

    s_hspi.Instance = SPI1;
    s_hspi.Init.Mode = SPI_MODE_MASTER;
    s_hspi.Init.Direction = SPI_DIRECTION_2LINES;
    s_hspi.Init.DataSize = SPI_DATASIZE_8BIT;
    s_hspi.Init.CLKPolarity = SPI_POLARITY_LOW;
    s_hspi.Init.CLKPhase = SPI_PHASE_1EDGE;
    s_hspi.Init.NSS = SPI_NSS_SOFT;
    s_hspi.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16; /* 170/16 ~10.6MHz */
    s_hspi.Init.FirstBit = SPI_FIRSTBIT_MSB;
    s_hspi.Init.TIMode = SPI_TIMODE_DISABLE;
    s_hspi.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    s_hspi.Init.CRCPolynomial = 7U;
    if (HAL_SPI_Init(&s_hspi) != HAL_OK)
    {
        return -1;
    }
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

void W25Q_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint32_t i;

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
    WriteEnable();
    CS_LOW();
    Xfer(W25Q_CMD_SECTOR_ERASE);
    Xfer((uint8_t)(addr >> 16U));
    Xfer((uint8_t)(addr >> 8U));
    Xfer((uint8_t)(addr & 0xFFU));
    CS_HIGH();
    WaitIdle();
}
