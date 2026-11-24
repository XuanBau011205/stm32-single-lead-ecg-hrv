cmake_minimum_required(VERSION 3.20)

project(P05_Host_Tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(test_boundary_parser
    test_boundary_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../HIL/boundary_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Core/sensor_storage_boundary.cpp
)

target_include_directories(test_boundary_parser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../HIL
    ${CMAKE_CURRENT_SOURCE_DIR}/../Core
)

target_compile_options(test_boundary_parser PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wconversion
    -Wshadow
    -fno-exceptions
    -fno-rtti
)

enable_testing()
add_test(NAME boundary_parser_unit_tests COMMAND test_boundary_parser)

add_executable(test_dsp_core
    test_dsp_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../DSP/iir_filter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../DSP/parabolic_interp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../DSP/pan_tompkins.cpp
)

target_include_directories(test_dsp_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../HIL
    ${CMAKE_CURRENT_SOURCE_DIR}/../DSP
)

target_compile_options(test_dsp_core PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wconversion
    -Wshadow
    -fno-exceptions
    -fno-rtti
)

add_test(NAME dsp_core_unit_tests COMMAND test_dsp_core)

add_executable(test_hrv_analyzer
    test_hrv_analyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../DSP/hrv_analyzer.cpp
)

target_include_directories(test_hrv_analyzer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../HIL
    ${CMAKE_CURRENT_SOURCE_DIR}/../DSP
)

target_compile_options(test_hrv_analyzer PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wconversion
    -Wshadow
    -fno-exceptions
    -fno-rtti
)

add_test(NAME hrv_analyzer_unit_tests COMMAND test_hrv_analyzer)

add_executable(test_hil_session
    test_hil_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../HIL/boundary_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../HIL/hil_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../DSP/iir_filter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../DSP/parabolic_interp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../DSP/pan_tompkins.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../DSP/hrv_analyzer.cpp
)

target_include_directories(test_hil_session PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../HIL
    ${CMAKE_CURRENT_SOURCE_DIR}/../DSP
)

target_compile_options(test_hil_session PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wconversion
    -Wshadow
    -fno-exceptions
    -fno-rtti
)

add_test(NAME hil_session_integration_tests COMMAND test_hil_session)

add_executable(test_platform_adapter
    test_platform_adapter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Core/sd_block_formatter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Core/sensor_storage_boundary.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Core/platform_controller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../HIL/boundary_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../HIL/hil_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../DSP/iir_filter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../DSP/parabolic_interp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../DSP/pan_tompkins.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../DSP/hrv_analyzer.cpp
)

target_include_directories(test_platform_adapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Core
    ${CMAKE_CURRENT_SOURCE_DIR}/../HIL
    ${CMAKE_CURRENT_SOURCE_DIR}/../DSP
)

target_compile_options(test_platform_adapter PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wconversion
    -Wshadow
    -fno-exceptions
    -fno-rtti
)

add_test(NAME platform_adapter_mock_tests COMMAND test_platform_adapter)

# R-PLT-01 / OPEN-22 static HAL/LL include check
add_test(
    NAME plt01_hal_include_policy
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/check_hal_includes.cmake"
)

# R-BND-04 static analysis check (cppcheck)
find_program(CPPCHECK_EXE cppcheck)
if(CPPCHECK_EXE)
    add_test(
        NAME bnd04_cppcheck_static_analysis
        COMMAND ${CPPCHECK_EXE}
            --error-exitcode=1
            --enable=warning,performance,portability
            --std=c++17
            --suppress=missingIncludeSystem
            -I "${CMAKE_CURRENT_SOURCE_DIR}/../Core"
            -I "${CMAKE_CURRENT_SOURCE_DIR}/../DSP"
            -I "${CMAKE_CURRENT_SOURCE_DIR}/../HIL"
            "${CMAKE_CURRENT_SOURCE_DIR}/../Core"
            "${CMAKE_CURRENT_SOURCE_DIR}/../DSP"
            "${CMAKE_CURRENT_SOURCE_DIR}/../HIL"
    )
endif()
