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
