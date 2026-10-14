/**
 * @file uart_dbg.h
 * @brief UART driver for debug telemetry and communication.
 */

#ifndef CORE_UART_DBG_H
#define CORE_UART_DBG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t UART_DBG_Init(uint32_t baudrate);
int32_t UART_DBG_Send(const uint8_t *data, size_t length);
int32_t UART_DBG_Receive(uint8_t *data, size_t max_length);

#ifdef __cplusplus
}
#endif

#endif /* CORE_UART_DBG_H */
