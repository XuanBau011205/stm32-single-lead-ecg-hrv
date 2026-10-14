/**
 * @file iir_biquad_q15.h
 * @brief Fixed-point Q15 IIR biquad filter.
 */

#ifndef DSP_IIR_BIQUAD_Q15_H
#define DSP_IIR_BIQUAD_Q15_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IIR_Biquad_Q15 IIR_Biquad_Q15_t; // TODO: define in .c khi vào bước DSP

void IIR_Biquad_Q15_Init(IIR_Biquad_Q15_t *filter);
int16_t IIR_Biquad_Q15_Process(IIR_Biquad_Q15_t *filter, int16_t sample);
void IIR_Biquad_Q15_ProcessBlock(IIR_Biquad_Q15_t *filter, int16_t *samples, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* DSP_IIR_BIQUAD_Q15_H */
