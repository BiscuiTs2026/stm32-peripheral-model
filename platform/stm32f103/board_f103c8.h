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
 *   TIM4 CH3     = PB8: servo
 * PB9 has no PWM channel: the board buzzer is driven by the TIM3 software
 * tone (see board_tone_id_t), so TIM4 keeps only the servo.
 * Channels that share a timer share the period (see pm_pwm.h). */
typedef enum {
    BOARD_PWM_MOTOR_A = 0,  /* PA0, TIM2_CH1 */
    BOARD_PWM_MOTOR_B,      /* PA1, TIM2_CH2 */
    BOARD_PWM_SERVO,        /* PB8, TIM4_CH3 */
    BOARD_PWM_COUNT
} board_pwm_id_t;

/* Logical timer ids, used by pm_timer.h claims and by the PWM timer table.
 * The value IS the timer number (2 = TIM2 ... 4 = TIM4), matching the
 * reference manual and board logs. TIM2 has no remap option for PA0/PA1 on
 * this package, so motor PWM and a free-running TIM2 counter cannot coexist
 * (docs/00-contract.md section 20). */
typedef enum {
    BOARD_TIM_2 = 2,
    BOARD_TIM_3 = 3,
    BOARD_TIM_4 = 4
} board_timer_id_t;

/* Software-tone outputs. PB9 carries the tone only; there is no PWM channel
 * on it (BOARD_PWM_BUZZER was removed). If a board ever maps a PWM channel
 * to the same pin, the port rejects whichever backend opens second. */
typedef enum {
    BOARD_TONE_BUZZER = 0,  /* PB9 via TIM3 update interrupt */
    BOARD_TONE_COUNT
} board_tone_id_t;

/* Line-tracking inputs. PB14 is SPI2_MISO, so the sensor and SPI2 are
 * mutually exclusive (docs/00-contract.md section 18). */
typedef enum {
    BOARD_TRACK_LEFT = 0,   /* PB14, internal pull-up */
    BOARD_TRACK_COUNT
} board_track_id_t;

/* Quadrature encoders. The EC11 push button is wired to PB15 (HARDWARE.md
 * 旋转编码器 row) and is part of this module as an optional capability
 * (pm_encoder_cfg_t.has_button). PB15 has no external pull-up; the port uses
 * the internal one. Its AF is SPI2_MOSI, so the button and BOARD_SPI_2 cannot
 * be used at the same time. */
typedef enum {
    BOARD_ENCODER_LEFT = 0, /* PA8 = A, PA9 = B, PB15 = button */
    BOARD_ENCODER_COUNT
} board_encoder_id_t;

#endif /* BOARD_F103C8_H */
