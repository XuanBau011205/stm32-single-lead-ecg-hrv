cmake_minimum_required(VERSION 3.20)

# Project Definition
project(P05_ECG_Firmware C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# MCU Flags: ARM Cortex-M3 (no FPU), Thumb mode, Size optimization
set(MCU_FLAGS "-mcpu=cortex-m3 -mthumb")
set(OPT_FLAGS "-Os -ffunction-sections -fdata-sections -fno-common")
set(WARN_FLAGS "-Wall -Wextra -Wstrict-prototypes")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${MCU_FLAGS} ${OPT_FLAGS} ${WARN_FLAGS}")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${MCU_FLAGS} -x assembler-with-cpp")

# Preprocessor Definitions (STM32F103xB medium density, zero heap)
add_definitions(
    -DSTM32F103xB
    -DUSE_HAL_DRIVER
    -DZERO_HEAP_ARCH
)

# Linker Configuration
set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/STM32F103C8Tx_FLASH.ld")
set(LINKER_FLAGS "${MCU_FLAGS} -T${LINKER_SCRIPT} -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections -Wl,-Map=${PROJECT_NAME}.map,--cref")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${LINKER_FLAGS}")

# Include Directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/DSP
    ${CMAKE_CURRENT_SOURCE_DIR}/HIL
)

# Source Files
set(CORE_SOURCES
    Core/adc_dma.c
    Core/tim_trigger.c
    Core/spi_sd.c
    Core/uart_dbg.c
    Core/i2c_ina219.c
    Core/dwt_cycle.c
)

set(DSP_SOURCES
    DSP/iir_biquad_q15.c
    DSP/iir_biquad_float.c
    DSP/pan_tompkins.c
    DSP/parabolic_interp.c
    DSP/hrv.c
)

set(HIL_SOURCES
    HIL/hil_protocol.c
)

# Executable target
add_executable(${PROJECT_NAME}.elf
    ${CORE_SOURCES}
    ${DSP_SOURCES}
    ${HIL_SOURCES}
)

# Post-build targets for binary generation and size printout
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}.elf>
    COMMENT "Building ${PROJECT_NAME}.hex, ${PROJECT_NAME}.bin and printing section sizes"
)
