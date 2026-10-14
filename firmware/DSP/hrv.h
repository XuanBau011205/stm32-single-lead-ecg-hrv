/**
 * @file hrv.h
 * @brief Time-domain Heart Rate Variability (HRV) analysis.
 */

#ifndef DSP_HRV_H
#define DSP_HRV_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HRV_Context HRV_Context_t; // TODO: define in .c khi vào bước DSP
typedef struct HRV_Metrics HRV_Metrics_t; // TODO: define in .c khi vào bước DSP

void HRV_Init(HRV_Context_t *ctx);
bool HRV_ProcessInterval(HRV_Context_t *ctx, uint16_t rr_interval_ms, HRV_Metrics_t *metrics);

#ifdef __cplusplus
}
#endif

#endif /* DSP_HRV_H */
