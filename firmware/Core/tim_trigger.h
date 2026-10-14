/**
 * @file tim_trigger.h
 * @brief Hardware timer trigger for periodic ADC sampling.
 */

#ifndef CORE_TIM_TRIGGER_H
#define CORE_TIM_TRIGGER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t TIM_Trigger_Init(uint32_t sampling_rate_hz);
int32_t TIM_Trigger_Start(void);
int32_t TIM_Trigger_Stop(void);

#ifdef __cplusplus
}
#endif

#endif /* CORE_TIM_TRIGGER_H */
