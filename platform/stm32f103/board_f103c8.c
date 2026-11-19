/*
 * board_f103c8.c - board pin table for the KEYSKING STM32F103C8T6 board.
 *
 * Only the port layer sees GPIOA/GPIOB. Upper layers use board_gpio_id_t.
 */
#include "board_f103c8.h"
#include "pm_port_stm32f1.h"

static const pm_f1_gpio_map_t s_gpio_map[BOARD_GPIO_COUNT] = {
    [BOARD_GPIO_LED_BLUE]  = { GPIOA, GPIO_PIN_6 },
    [BOARD_GPIO_LED_GREEN] = { GPIOA, GPIO_PIN_7 },
    [BOARD_GPIO_LED_RED]   = { GPIOB, GPIO_PIN_0 },
    [BOARD_GPIO_KEY1]      = { GPIOB, GPIO_PIN_12 },
    [BOARD_GPIO_KEY2]      = { GPIOB, GPIO_PIN_13 },
};

const pm_f1_gpio_ctx_t pm_f1_gpio_ctx = {
    .map   = s_gpio_map,
    .count = (uint16_t)BOARD_GPIO_COUNT,
};

static const pm_f1_spi_pins_t s_spi_pins[BOARD_SPI_COUNT] = {
    [BOARD_SPI_1] = { GPIOA, GPIO_PIN_5, GPIOA, GPIO_PIN_6, GPIOA, GPIO_PIN_7 },
    [BOARD_SPI_2] = { GPIOB, GPIO_PIN_13, GPIOB, GPIO_PIN_14, GPIOB, GPIO_PIN_15 },
};

const pm_f1_spi_ctx_t pm_f1_spi_ctx = {
    .pins  = s_spi_pins,
    .count = (uint8_t)BOARD_SPI_COUNT,
};

static const pm_f1_i2c_pins_t s_i2c_pins[BOARD_I2C_COUNT] = {
    [BOARD_I2C_1] = { GPIOB, GPIO_PIN_6, GPIOB, GPIO_PIN_7 },
};

const pm_f1_i2c_ctx_t pm_f1_i2c_ctx = {
    .pins  = s_i2c_pins,
    .count = (uint8_t)BOARD_I2C_COUNT,
};

static const pm_f1_adc_channel_t s_adc_channels[BOARD_ADC_COUNT] = {
    [BOARD_ADC_NTC] = { GPIOA, GPIO_PIN_4, ADC_CHANNEL_4 },
    [BOARD_ADC_POT] = { GPIOA, GPIO_PIN_5, ADC_CHANNEL_5 },
};

const pm_f1_adc_ctx_t pm_f1_adc_ctx = {
    .channels = s_adc_channels,
    .count    = (uint8_t)BOARD_ADC_COUNT,
};

/* timer_index uses board_timer_id_t (must match the port's timer table). */
static const pm_f1_pwm_pins_t s_pwm_pins[BOARD_PWM_COUNT] = {
    [BOARD_PWM_MOTOR_A] = { GPIOA, GPIO_PIN_0, TIM2, TIM_CHANNEL_1,
                            BOARD_TIM_2 },
    [BOARD_PWM_MOTOR_B] = { GPIOA, GPIO_PIN_1, TIM2, TIM_CHANNEL_2,
                            BOARD_TIM_2 },
    [BOARD_PWM_SERVO]   = { GPIOB, GPIO_PIN_8, TIM4, TIM_CHANNEL_3,
                            BOARD_TIM_4 },
    [BOARD_PWM_BUZZER]  = { GPIOB, GPIO_PIN_9, TIM4, TIM_CHANNEL_4,
                            BOARD_TIM_4 },
};

const pm_f1_pwm_ctx_t pm_f1_pwm_ctx = {
    .pins  = s_pwm_pins,
    .count = (uint8_t)BOARD_PWM_COUNT,
};

/* Software tone: PB9 toggled from the TIM3 update interrupt. */
static const pm_f1_tone_pins_t s_tone_pins[BOARD_TONE_COUNT] = {
    [BOARD_TONE_BUZZER] = { GPIOB, GPIO_PIN_9, TIM3, BOARD_TIM_3 },
};

const pm_f1_tone_ctx_t pm_f1_tone_ctx = {
    .pins  = s_tone_pins,
    .count = (uint8_t)BOARD_TONE_COUNT,
};

/* Tracking input: PB14 with the internal pull-up, shared with SPI2_MISO. */
static const pm_f1_track_pins_t s_track_pins[BOARD_TRACK_COUNT] = {
    [BOARD_TRACK_LEFT] = { GPIOB, GPIO_PIN_14, BOARD_SPI_2 },
};

const pm_f1_track_ctx_t pm_f1_track_ctx = {
    .pins  = s_track_pins,
    .count = (uint8_t)BOARD_TRACK_COUNT,
};

/* Encoder: A = PA8, B = PA9, both edges on EXTI9_5. */
static const pm_f1_encoder_pins_t s_encoder_pins[BOARD_ENCODER_COUNT] = {
    [BOARD_ENCODER_LEFT] = { GPIOA, GPIO_PIN_8, GPIOA, GPIO_PIN_9,
                             EXTI9_5_IRQn },
};

const pm_f1_encoder_ctx_t pm_f1_encoder_ctx = {
    .pins  = s_encoder_pins,
    .count = (uint8_t)BOARD_ENCODER_COUNT,
};
