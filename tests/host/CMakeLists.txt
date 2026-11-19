cmake_minimum_required(VERSION 3.20)
project(pm_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Static CRT so the test binary runs without a Visual Studio environment.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Optional modules: mirror the firmware rule (header + core source present).
set(PM_TEST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(PM_TEST_SOURCES
    test_main.c
    pm_osal_host.c
    pm_port_mock.c
    ../../src/core/pm_err.c
    ../../src/core/pm_event.c
    ../../src/core/pm_ringbuf.c
    ../../src/core/pm_gpio.c
    ../../src/core/pm_uart.c
    ../../src/common/pm_pwm_math.c
    ../../src/common/pm_timer.c
)
set(PM_TEST_DEFS "")
foreach(mod spi i2c adc pwm tone track encoder)
    string(TOUPPER ${mod} PM_MOD_UPPER)
    if(EXISTS ${PM_TEST_ROOT}/include/pm/pm_${mod}.h AND
       EXISTS ${PM_TEST_ROOT}/src/core/pm_${mod}.c)
        list(APPEND PM_TEST_SOURCES ${PM_TEST_ROOT}/src/core/pm_${mod}.c)
        list(APPEND PM_TEST_DEFS PM_ENABLE_${PM_MOD_UPPER}=1)
    endif()
endforeach()

add_executable(pm_host_tests ${PM_TEST_SOURCES})

if(PM_TEST_DEFS)
    target_compile_definitions(pm_host_tests PRIVATE ${PM_TEST_DEFS})
endif()

target_include_directories(pm_host_tests PRIVATE
    ../../include/pm
    ../../include
)

if(MSVC)
    target_compile_options(pm_host_tests PRIVATE /W4)
else()
    target_compile_options(pm_host_tests PRIVATE -Wall -Wextra -Wshadow -Werror)
endif()
