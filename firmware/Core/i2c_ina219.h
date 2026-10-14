/**
 * @file i2c_ina219.h
 * @brief I2C driver for INA219 current and power monitor.
 */

#ifndef CORE_I2C_INA219_H
#define CORE_I2C_INA219_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t INA219_Init(void);
int32_t INA219_ReadBusVoltage(uint32_t *voltage_mv);
int32_t INA219_ReadCurrent(int32_t *current_ua);
int32_t INA219_PowerDown(void);

#ifdef __cplusplus
}
#endif

#endif /* CORE_I2C_INA219_H */
