# Static R-PLT-01 / OPEN-22 check: fail if stm32f1xx_hal* or stm32f1xx_ll* is included
# outside the sole approved exception Core/stm32f103_i2c1.cpp.
set(_fw_root "${CMAKE_CURRENT_LIST_DIR}/..")
file(GLOB_RECURSE _fw_sources
    RELATIVE "${_fw_root}"
    "${_fw_root}/Core/*.hpp"
    "${_fw_root}/Core/*.cpp"
    "${_fw_root}/Core/*.h"
    "${_fw_root}/Core/*.c"
    "${_fw_root}/DSP/*.hpp"
    "${_fw_root}/DSP/*.cpp"
    "${_fw_root}/HIL/*.hpp"
    "${_fw_root}/HIL/*.cpp"
)

set(_violations "")
foreach(_rel IN LISTS _fw_sources)
    file(STRINGS "${_fw_root}/${_rel}" _matches REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"].*stm32f1xx_(hal|ll)")
    if(_matches)
        if(NOT _rel STREQUAL "Core/stm32f103_i2c1.cpp")
            list(APPEND _violations "${_rel}: ${_matches}")
        endif()
    endif()
endforeach()

if(_violations)
    message(FATAL_ERROR "R-PLT-01 / OPEN-22 violation (disallowed HAL/LL include): ${_violations}")
else()
    message(STATUS "R-PLT-01 / OPEN-22 check passed: only Core/stm32f103_i2c1.cpp may include stm32f1xx_hal/ll.")
endif()
