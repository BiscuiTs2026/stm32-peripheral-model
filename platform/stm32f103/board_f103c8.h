/*
 * board_f103c8.h - board description for the KEYSKING STM32F103C8T6 board.
 *
 * This is the ONLY file that has to change when the same library is moved to
 * another board. Pin numbers come from HARDWARE.md section 3.
 */
#ifndef BOARD_F103C8_H
#define BOARD_F103C8_H

/* Logical GPIO ids. Order defines the id used by pm_gpio_*(). */
typedef enum {
    BOARD_GPIO_LED_BLUE = 0,   /* PA6 */
    BOARD_GPIO_LED_GREEN,      /* PA7 */
    BOARD_GPIO_LED_RED,        /* PB0 */
    BOARD_GPIO_KEY1,           /* PB12, external 10k pull-up */
    BOARD_GPIO_KEY2,           /* PB13, no external pull-up */
    BOARD_GPIO_COUNT
} board_gpio_id_t;

/* Logical UART ids. USART1=0, USART2=1, USART3=2. */
typedef enum {
    BOARD_UART_DEBUG = 1,      /* USART2 on PA2/PA3, onboard CH343P */
    BOARD_UART_COUNT = 3
} board_uart_id_t;

/* Logical SPI ids. SPI1=0, SPI2=1.
 * This board exposes no free SPI header:
 *   SPI1 = PA5 SCK / PA6 MISO / PA7 MOSI  (shared with the pot and the
 *          blue/green LEDs)
 *   SPI2 = PB13 SCK / PB14 MISO / PB15 MOSI (shared with KEY2, the tracking
 *          sensor and the encoder button)
 * Free the pins of the chosen instance before driving real traffic. */
typedef enum {
    BOARD_SPI_1 = 0,   /* PA5 SCK, PA6 MISO, PA7 MOSI */
    BOARD_SPI_2,       /* PB13 SCK, PB14 MISO, PB15 MOSI */
    BOARD_SPI_COUNT
} board_spi_id_t;

/* Logical I2C ids.
 *   I2C1 = PB6 SCL / PB7 SDA: OLED (0x7A), AHT20 (0x70) and the external
 *          I2C header share this bus.
 * I2C2 is not exposed: PB10/PB11 are the USART3 / BLE header on this board. */
typedef enum {
    BOARD_I2C_1 = 0,
    BOARD_I2C_COUNT
} board_i2c_id_t;

/* Logical ADC channels.
 *   ADC1_IN4 = PA4: NTC divider (10k, beta 3950)
 *   ADC1_IN5 = PA5: potentiometer (also SPI1 SCK; free it before using SPI1) */
typedef enum {
    BOARD_ADC_NTC = 0,      /* PA4, ADC1_IN4 */
    BOARD_ADC_POT,          /* PA5, ADC1_IN5 */
    BOARD_ADC_COUNT
} board_adc_id_t;

/* Logical PWM channels.
 *   TIM2 CH1/CH2 = PA0/PA1: DRV8833 motor A/B
 *   TIM4 CH3/CH4 = PB8/PB9: servo / passive buzzer
 * Channels that share a timer share the period (see pm_pwm.h). */
typedef enum {
    BOARD_PWM_MOTOR_A = 0,  /* PA0, TIM2_CH1 */
    BOARD_PWM_MOTOR_B,      /* PA1, TIM2_CH2 */
    BOARD_PWM_SERVO,        /* PB8, TIM4_CH3 */
    BOARD_PWM_BUZZER,       /* PB9, TIM4_CH4 */
    BOARD_PWM_COUNT
} board_pwm_id_t;

#endif /* BOARD_F103C8_H */
