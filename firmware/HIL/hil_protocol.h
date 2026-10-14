/**
 * @file hil_protocol.h
 * @brief Hardware-in-the-Loop (HIL) UART framing protocol.
 */

#ifndef HIL_HIL_PROTOCOL_H
#define HIL_HIL_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HIL_Context HIL_Context_t; // TODO: define in .c khi vào bước DSP

void HIL_Protocol_Init(HIL_Context_t *ctx);
bool HIL_Protocol_ParseByte(HIL_Context_t *ctx, uint8_t byte, int16_t *out_sample);
int32_t HIL_Protocol_SendRPeak(uint32_t sample_index, uint16_t rr_interval_ms);

#ifdef __cplusplus
}
#endif

#endif /* HIL_HIL_PROTOCOL_H */
