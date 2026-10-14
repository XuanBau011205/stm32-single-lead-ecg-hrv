/**
 * @file pan_tompkins.c
 * @brief Pan-Tompkins QRS detection stub implementation.
 */

#include "pan_tompkins.h"

void PanTompkins_Init(PanTompkins_Context_t *ctx)
{
    // TODO
    (void)ctx;
}

bool PanTompkins_ProcessSample(PanTompkins_Context_t *ctx, int16_t sample, uint32_t *r_peak_index)
{
    // TODO
    (void)ctx;
    (void)sample;
    (void)r_peak_index;
    return false;
}
