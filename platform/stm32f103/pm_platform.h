/*
 * pm_platform.h - minimal platform bring-up for the STM32F103 example.
 */
#ifndef PM_PLATFORM_H
#define PM_PLATFORM_H

#include "pm_common.h"

/* Powers up the tick source (SysTick 1 ms) and the clock tree
 * (HSE 8 MHz -> PLL -> 72 MHz). Falls back to HSI 8 MHz if the crystal
 * does not start. Call once, before pm_osal_init(). */
void pm_platform_init(void);

/* Actual SYSCLK in Hz after pm_platform_init(): 72 MHz on HSE, 8 MHz on
 * the HSI fallback. Read from SystemCoreClock, which HAL keeps in sync. */
uint32_t pm_platform_core_clock(void);

#endif /* PM_PLATFORM_H */
