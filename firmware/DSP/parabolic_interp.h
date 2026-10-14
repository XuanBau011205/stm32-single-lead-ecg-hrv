/**
 * @file parabolic_interp.h
 * @brief Parabolic sub-sample peak interpolation.
 */

#ifndef DSP_PARABOLIC_INTERP_H
#define DSP_PARABOLIC_INTERP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t Parabolic_Interpolate_Q15(int16_t y_minus1, int16_t y_0, int16_t y_plus1, int16_t *delta_q15);
int32_t Parabolic_Interpolate_Float(float y_minus1, float y_0, float y_plus1, float *delta_float);

#ifdef __cplusplus
}
#endif

#endif /* DSP_PARABOLIC_INTERP_H */
