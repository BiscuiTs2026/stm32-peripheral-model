# CMake toolchain file for arm-none-eabi-gcc (STM32F1).
#
# Toolchain location, in priority order:
#   1. -DTOOLCHAIN_ROOT=<dir>/bin on the cmake command line
#   2. the TOOLCHAIN_ROOT environment variable
#   3. the default: no prefix, so CMake resolves arm-none-eabi-* from PATH
#
# A normal arm-none-eabi installation therefore needs no extra options; a
# bundled toolchain (STM32Cube, xPack, ...) can be pointed at explicitly.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT DEFINED TOOLCHAIN_ROOT AND DEFINED ENV{TOOLCHAIN_ROOT})
  set(TOOLCHAIN_ROOT "$ENV{TOOLCHAIN_ROOT}")
endif()

if(DEFINED TOOLCHAIN_ROOT)
  set(PM_EXE_SUFFIX "")
  if(CMAKE_HOST_WIN32)
    set(PM_EXE_SUFFIX ".exe")
  endif()
  set(CMAKE_C_COMPILER   "${TOOLCHAIN_ROOT}/arm-none-eabi-gcc${PM_EXE_SUFFIX}")
  set(CMAKE_ASM_COMPILER "${TOOLCHAIN_ROOT}/arm-none-eabi-gcc${PM_EXE_SUFFIX}")
  set(CMAKE_CXX_COMPILER "${TOOLCHAIN_ROOT}/arm-none-eabi-g++${PM_EXE_SUFFIX}")
  set(CMAKE_OBJCOPY      "${TOOLCHAIN_ROOT}/arm-none-eabi-objcopy${PM_EXE_SUFFIX}")
  set(CMAKE_SIZE         "${TOOLCHAIN_ROOT}/arm-none-eabi-size${PM_EXE_SUFFIX}")
else()
  set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
  set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
  set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
  set(CMAKE_OBJCOPY      arm-none-eabi-objcopy)
  set(CMAKE_SIZE         arm-none-eabi-size)
endif()

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
