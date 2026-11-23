cmake_minimum_required(VERSION 3.20)

# P05 ECG & HRV Firmware (STM32F103C8T6, ARM Cortex-M3, Zero-Heap C++17)
project(P05_ECG_Firmware LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MCU Flags: ARM Cortex-M3 (no FPU), Thumb mode, Size optimization, Zero exceptions/RTTI
set(MCU_FLAGS "-mcpu=cortex-m3 -mthumb")
set(OPT_FLAGS "-Os -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fno-threadsafe-statics")
set(WARN_FLAGS "-Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${MCU_FLAGS} ${OPT_FLAGS} ${WARN_FLAGS}")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${MCU_FLAGS} -x assembler-with-cpp")

add_definitions(
    -DSTM32F103xB
    -DZERO_HEAP_ARCH
)

set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/STM32F103C8Tx_FLASH.ld")
set(LINKER_FLAGS "${MCU_FLAGS} -T\"${LINKER_SCRIPT}\" -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections -Wl,-Map=${PROJECT_NAME}.map,--cref")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${LINKER_FLAGS}")

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/DSP
    ${CMAKE_CURRENT_SOURCE_DIR}/HIL
)

set(CORE_SOURCES
    Core/startup_stm32f103.cpp
    Core/main.cpp
    Core/sensor_storage_boundary.cpp
    Core/sd_block_formatter.cpp
    Core/platform_controller.cpp
    Core/stm32f103_i2c1.cpp
    Core/stm32f103_platform.cpp
)

set(DSP_SOURCES
    DSP/iir_filter.cpp
    DSP/parabolic_interp.cpp
    DSP/pan_tompkins.cpp
    DSP/hrv_analyzer.cpp
)

set(HIL_SOURCES
    HIL/boundary_parser.cpp
    HIL/hil_session.cpp
)

add_executable(${PROJECT_NAME}.elf
    ${CORE_SOURCES}
    ${DSP_SOURCES}
    ${HIL_SOURCES}
)

add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}.elf>
    COMMENT "Building ${PROJECT_NAME}.hex, ${PROJECT_NAME}.bin and printing Flash/SRAM sizes"
)
