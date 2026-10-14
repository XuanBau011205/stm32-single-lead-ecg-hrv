/**
 * @file dwt_cycle.h
 * @brief DWT cycle counter for execution time profiling.
 */

#ifndef CORE_DWT_CYCLE_H
#define CORE_DWT_CYCLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void DWT_Cycle_Init(void);
uint32_t DWT_Cycle_Get(void);
void DWT_Cycle_Reset(void);

#ifdef __cplusplus
}
#endif

#endif /* CORE_DWT_CYCLE_H */
