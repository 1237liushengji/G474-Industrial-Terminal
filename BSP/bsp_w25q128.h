/**
  ******************************************************************************
  * @file    bsp_w25q128.h
  * @brief   W25Q128JV SPI NOR flash driver (16 MB).
  * @note    Pins: CS=PA4 SCK=PA5 MISO=PA6 MOSI=PA7 (SPI1, AF5)
  ******************************************************************************
  */
#ifndef __BSP_W25Q128_H
#define __BSP_W25Q128_H

#include <stdint.h>

#define W25Q_SECTOR_SIZE   4096U
#define W25Q_PAGE_SIZE     256U
#define W25Q_CAPACITY      (16UL * 1024UL * 1024UL)

/** Init SPI1 + CS GPIO. Call once. @retval 0 ok, -1 error */
int32_t W25Q_Init(void);

/** Read JEDEC ID (0x9F). @retval 24-bit ID, 0 on failure */
uint32_t W25Q_ReadID(void);

/** Read bytes. */
void W25Q_Read(uint32_t addr, uint8_t *buf, uint32_t len);

/**
  * @brief  Write bytes (handles page boundaries + erase NOT included).
  * @note   Target sectors must be erased first (W25Q_EraseSector).
  */
void W25Q_Write(uint32_t addr, const uint8_t *buf, uint32_t len);

/** Erase one 4 KB sector (addr must be sector aligned). */
void W25Q_EraseSector(uint32_t addr);

/** Read the write-in-progress status bit. @retval 1 busy */
uint8_t W25Q_IsBusy(void);

#endif /* __BSP_W25Q128_H */
