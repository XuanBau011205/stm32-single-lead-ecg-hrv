/**
 * @file spi_sd.h
 * @brief SPI driver for microSD card interface.
 */

#ifndef CORE_SPI_SD_H
#define CORE_SPI_SD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t SPI_SD_Init(void);
void SPI_SD_PowerControl(bool enable);
int32_t SPI_SD_WriteBlock(uint32_t sector, const uint8_t *buffer, size_t size);
int32_t SPI_SD_ReadBlock(uint32_t sector, uint8_t *buffer, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* CORE_SPI_SD_H */
