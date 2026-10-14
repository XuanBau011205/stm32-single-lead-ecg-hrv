/**
 * @file hil_protocol.c
 * @brief HIL UART framing protocol stub implementation.
 */

#include "hil_protocol.h"

void HIL_Protocol_Init(HIL_Context_t *ctx)
{
    // TODO
    (void)ctx;
}

bool HIL_Protocol_ParseByte(HIL_Context_t *ctx, uint8_t byte, int16_t *out_sample)
{
    // TODO
    (void)ctx;
    (void)byte;
    (void)out_sample;
    return false;
}

int32_t HIL_Protocol_SendRPeak(uint32_t sample_index, uint16_t rr_interval_ms)
{
    // TODO
    (void)sample_index;
    (void)rr_interval_ms;
    return 0;
}
