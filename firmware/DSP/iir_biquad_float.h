/**
 * @file iir_biquad_float.h
 * @brief Floating-point IIR biquad filter for baseline comparison.
 */

#ifndef DSP_IIR_BIQUAD_FLOAT_H
#define DSP_IIR_BIQUAD_FLOAT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IIR_Biquad_Float IIR_Biquad_Float_t; // TODO: define in .c khi vào bước DSP

void IIR_Biquad_Float_Init(IIR_Biquad_Float_t *filter);
float IIR_Biquad_Float_Process(IIR_Biquad_Float_t *filter, float sample);
void IIR_Biquad_Float_ProcessBlock(IIR_Biquad_Float_t *filter, float *samples, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* DSP_IIR_BIQUAD_FLOAT_H */
