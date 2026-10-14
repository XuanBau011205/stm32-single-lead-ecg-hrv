/**
 * @file spi_sd.c
 * @brief SPI microSD driver stub implementation.
 */

#include "spi_sd.h"

int32_t SPI_SD_Init(void)
{
    // TODO
    return 0;
}

void SPI_SD_PowerControl(bool enable)
{
    // TODO
    (void)enable;
}

int32_t SPI_SD_WriteBlock(uint32_t sector, const uint8_t *buffer, size_t size)
{
    // TODO
    (void)sector;
    (void)buffer;
    (void)size;
    return 0;
}

int32_t SPI_SD_ReadBlock(uint32_t sector, uint8_t *buffer, size_t size)
{
    // TODO
    (void)sector;
    (void)buffer;
    (void)size;
    return 0;
}
