/**
 * @file pan_tompkins.h
 * @brief Pan-Tompkins QRS detection pipeline.
 */

#ifndef DSP_PAN_TOMPKINS_H
#define DSP_PAN_TOMPKINS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PanTompkins_Context PanTompkins_Context_t; // TODO: define in .c khi vào bước DSP

void PanTompkins_Init(PanTompkins_Context_t *ctx);
bool PanTompkins_ProcessSample(PanTompkins_Context_t *ctx, int16_t sample, uint32_t *r_peak_index);

#ifdef __cplusplus
}
#endif

#endif /* DSP_PAN_TOMPKINS_H */
