cmake_minimum_required(VERSION 3.20)
project(peripheral_model C ASM)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(PM_ROOT     ${CMAKE_CURRENT_SOURCE_DIR})
set(PM_PLATFORM ${PM_ROOT}/platform/stm32f103)

# ---------------------------------------------------------------------------
# Optional peripheral modules.
#
# A module participates only when both its public header and its core source
# exist. This keeps the historical batches (v0.1 GPIO+UART, v0.2 +SPI,
# v0.3 +I2C, v0.4 +ADC/PWM) independently buildable without keeping hand-made
# copies of older CMakeLists: the files themselves are the switch.
set(PM_MODULE_NAMES  spi i2c adc pwm)
set(PM_MODULE_SOURCES "")
set(PM_MODULE_DEFS "")
foreach(mod ${PM_MODULE_NAMES})
    string(TOUPPER ${mod} PM_MOD_UPPER)
    if(EXISTS ${PM_ROOT}/include/pm/pm_${mod}.h AND
       EXISTS ${PM_ROOT}/src/core/pm_${mod}.c)
        list(APPEND PM_MODULE_SOURCES src/core/pm_${mod}.c)
        list(APPEND PM_MODULE_DEFS PM_ENABLE_${PM_MOD_UPPER}=1)
        message(STATUS "pm: module '${mod}' enabled")
    else()
        message(STATUS "pm: module '${mod}' not present, skipped")
    endif()
endforeach()

if(PM_MODULE_DEFS)
    add_compile_definitions(${PM_MODULE_DEFS})
endif()

set(PM_CPU_FLAGS  -mcpu=cortex-m3 -mthumb -mfloat-abi=soft)
set(PM_WARN_FLAGS -Wall -Wextra -Wshadow -Wundef
                  -Werror=implicit-function-declaration)
set(PM_OPT_FLAGS  -Os -g3 -ffunction-sections -fdata-sections)

# ------------------------------------------------------------------------ HAL
add_library(pm_hal STATIC
    third_party/stm32f1xx_hal/src/stm32f1xx_hal.c
    third_party/stm32f1xx_hal/src/stm32f1xx_hal_adc.c
    third_party/stm32f1xx_hal/src/stm32f1xx_hal_adc_ex.c
    third_party/stm32f1xx_hal/src/stm32f1xx_hal_cortex.c
    third_party/stm32f1xx_hal/src/stm32f1xx_hal_dma.c
    third_party/stm32f1xx_hal/src/stm32f1xx_hal_flash.c
    third_party/stm32f1xx_hal/src/stm32f1xx_hal_flash_ex.c
    third_party/stm32f1xx_hal/src/stm32f1xx_hal_gpio.c
    third_party/stm32f1xx_hal/src/stm32f1xx_hal_gpio_ex.c
    third_party/stm32f1xx_hal/src/stm32f1xx_hal_i2c.c
    third_party/stm32f1xx_hal/src/stm32f1xx_hal_pwr.c
    third_party/stm32f1xx_hal/src/stm32f1xx_hal_rcc.c
    third_party/stm32f1xx_hal/src/stm32f1xx_hal_rcc_ex.c
    third_party/stm32f1xx_hal/src/stm32f1xx_hal_spi.c
    third_party/stm32f1xx_hal/src/stm32f1xx_hal_tim.c
    third_party/stm32f1xx_hal/src/stm32f1xx_hal_tim_ex.c
    third_party/stm32f1xx_hal/src/stm32f1xx_hal_uart.c
)
target_include_directories(pm_hal
    PUBLIC  ${PM_ROOT}/third_party/stm32f1xx_hal/inc
            ${PM_ROOT}/third_party/cmsis/device/include
            ${PM_ROOT}/third_party/cmsis/include
    PRIVATE ${PM_PLATFORM}
)
target_compile_definitions(pm_hal PUBLIC
    USE_HAL_DRIVER
    STM32F103xB
)
# Vendor code is compiled without our warning policy on purpose.
target_compile_options(pm_hal PRIVATE
    ${PM_CPU_FLAGS} -Os -g -ffunction-sections -fdata-sections -w)

# ---------------------------------------------------------------- core model
add_library(pm_core STATIC
    src/core/pm_err.c
    src/core/pm_event.c
    src/core/pm_ringbuf.c
    src/core/pm_gpio.c
    src/core/pm_uart.c
    ${PM_MODULE_SOURCES}
)
target_include_directories(pm_core PUBLIC ${PM_ROOT}/include/pm ${PM_ROOT}/include)
target_compile_options(pm_core PRIVATE
    ${PM_CPU_FLAGS} ${PM_WARN_FLAGS} ${PM_OPT_FLAGS})

# ---------------------------------------------------------------------- OSAL
add_library(pm_osal STATIC
    src/osal/pm_osal_port_cm3.c
)
target_include_directories(pm_osal
    PUBLIC  ${PM_ROOT}/include/pm ${PM_ROOT}/include
    PRIVATE ${PM_ROOT}/third_party/cmsis/device/include
            ${PM_ROOT}/third_party/cmsis/include
)
target_compile_definitions(pm_osal PRIVATE
    STM32F103xB
)
target_compile_options(pm_osal PRIVATE
    ${PM_CPU_FLAGS} ${PM_WARN_FLAGS} ${PM_OPT_FLAGS})

# -------------------------------------------------------- STM32F1 port + board
add_library(pm_port_stm32f1 STATIC
    src/port/stm32f1/pm_port_stm32f1.c
    ${PM_PLATFORM}/board_f103c8.c
    ${PM_PLATFORM}/pm_platform.c
)
target_include_directories(pm_port_stm32f1
    PUBLIC  ${PM_ROOT}/include/pm ${PM_ROOT}/include
            ${PM_PLATFORM}
    PRIVATE ${PM_ROOT}/src/port/stm32f1
)
target_compile_options(pm_port_stm32f1 PRIVATE
    ${PM_CPU_FLAGS} ${PM_WARN_FLAGS} ${PM_OPT_FLAGS})
target_link_libraries(pm_port_stm32f1 PUBLIC pm_core pm_osal pm_hal)

# ------------------------------------------------------------------- firmware
add_executable(pm_f103_demo
    src/app/main.c
    platform/stm32f103/startup_stm32f103xb.s
    third_party/cmsis/device/source/system_stm32f1xx.c
)
target_include_directories(pm_f103_demo PRIVATE
    ${PM_ROOT}/include/pm ${PM_ROOT}/include
    ${PM_PLATFORM}
)
target_compile_options(pm_f103_demo PRIVATE
    ${PM_CPU_FLAGS} ${PM_WARN_FLAGS} ${PM_OPT_FLAGS})
target_compile_definitions(pm_f103_demo PRIVATE
    USE_HAL_DRIVER
    STM32F103xB
)
# pm_core calls pm_port_get() and the port calls back into pm_core, so the
# static libraries are linked as a group instead of relying on a strict order.
target_link_libraries(pm_f103_demo PRIVATE
    -Wl,--start-group
    pm_port_stm32f1
    pm_core
    pm_osal
    pm_hal
    -Wl,--end-group
)

target_link_options(pm_f103_demo PRIVATE
    ${PM_CPU_FLAGS}
    -T${PM_PLATFORM}/stm32f103c8.ld
    --specs=nano.specs
    --specs=nosys.specs
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/pm_f103_demo.map
    -Wl,--print-memory-usage
)

add_custom_command(TARGET pm_f103_demo POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:pm_f103_demo>
            $<TARGET_FILE_DIR:pm_f103_demo>/pm_f103_demo.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:pm_f103_demo>
            $<TARGET_FILE_DIR:pm_f103_demo>/pm_f103_demo.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:pm_f103_demo>
    COMMENT "Generating .bin/.hex and reporting size"
)
