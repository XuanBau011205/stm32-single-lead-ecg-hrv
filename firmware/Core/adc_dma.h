/**
 * @file adc_dma.h
 * @brief ADC and DMA driver for ECG sampling.
 */

#ifndef CORE_ADC_DMA_H
#define CORE_ADC_DMA_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t ADC_DMA_Init(void);
int32_t ADC_DMA_Start(void);
int32_t ADC_DMA_Stop(void);

#ifdef __cplusplus
}
#endif

#endif /* CORE_ADC_DMA_H */
