/*
 * pm_port_stm32f1.h - internal header of the STM32F1 port.
 *
 * Vendor types are allowed here and only here (plus the .c files of this
 * directory). Nothing above the port may include this header.
 */
#ifndef PM_PORT_STM32F1_H
#define PM_PORT_STM32F1_H

#include "pm_port.h"
#include "stm32f1xx.h"

typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
} pm_f1_gpio_map_t;

typedef struct {
    const pm_f1_gpio_map_t *map;
    uint16_t                count;
} pm_f1_gpio_ctx_t;

/* Defined by the board file (platform/<chip>/board_*.c). */
extern const pm_f1_gpio_ctx_t pm_f1_gpio_ctx;

/* SPI pin map: SCK/MISO/MOSI are fixed alternate-function pins per instance;
 * the board owns the pin numbers, the port owns the peripheral setup. */
typedef struct {
    GPIO_TypeDef *sck_port;
    uint16_t      sck_pin;
    GPIO_TypeDef *miso_port;
    uint16_t      miso_pin;
    GPIO_TypeDef *mosi_port;
    uint16_t      mosi_pin;
} pm_f1_spi_pins_t;

typedef struct {
    const pm_f1_spi_pins_t *pins;
    uint8_t                 count;
} pm_f1_spi_ctx_t;

extern const pm_f1_spi_ctx_t pm_f1_spi_ctx;

/* I2C pin map: SCL/SDA are open-drain alternate-function pins; pull-ups live
 * on the bus (on-board modules) and the port adds the internal weak ones. */
typedef struct {
    GPIO_TypeDef *scl_port;
    uint16_t      scl_pin;
    GPIO_TypeDef *sda_port;
    uint16_t      sda_pin;
} pm_f1_i2c_pins_t;

typedef struct {
    const pm_f1_i2c_pins_t *pins;
    uint8_t                 count;
} pm_f1_i2c_ctx_t;

extern const pm_f1_i2c_ctx_t pm_f1_i2c_ctx;

/* ADC channel map: the board owns port/pin, the port owns the ADC setup. */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
    uint32_t      hal_channel;
} pm_f1_adc_channel_t;

typedef struct {
    const pm_f1_adc_channel_t *channels;
    uint8_t                    count;
} pm_f1_adc_ctx_t;

extern const pm_f1_adc_ctx_t pm_f1_adc_ctx;

/* PWM channel map: one timer compare output per logical channel.
 * `timer_index` selects the shared timer state inside the port, so channels
 * listed with the same index share one period. */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
    TIM_TypeDef  *inst;
    uint32_t      channel;       /* TIM_CHANNEL_x */
    uint8_t       timer_index;
} pm_f1_pwm_pins_t;

typedef struct {
    const pm_f1_pwm_pins_t *pins;
    uint8_t                 count;
} pm_f1_pwm_ctx_t;

extern const pm_f1_pwm_ctx_t pm_f1_pwm_ctx;

/* Tone pin map: one GPIO flipped by the timer's update interrupt. */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
    TIM_TypeDef  *inst;
    uint8_t       timer_index;   /* board_timer_id_t */
} pm_f1_tone_pins_t;

typedef struct {
    const pm_f1_tone_pins_t *pins;
    uint8_t                  count;
} pm_f1_tone_ctx_t;

extern const pm_f1_tone_ctx_t pm_f1_tone_ctx;

/* Tracking input. `spi_hw_id` is the SPI instance that owns the same pin
 * (0xFF when the pin is exclusive); pm_track refuses to open while that
 * instance is open. */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
    uint8_t       spi_hw_id;
} pm_f1_track_pins_t;

typedef struct {
    const pm_f1_track_pins_t *pins;
    uint8_t                   count;
} pm_f1_track_ctx_t;

extern const pm_f1_track_ctx_t pm_f1_track_ctx;

/* Quadrature encoder pin pair. Both pins use the same EXTI line group.
 * The optional push button has no interrupt path: it is polled through
 * read_button(). Leave button_port/button_pin at 0 when the module has no SW
 * pin (this board: PB15 is SPI2_MOSI). */
typedef struct {
    GPIO_TypeDef *a_port;
    uint16_t      a_pin;
    GPIO_TypeDef *b_port;
    uint16_t      b_pin;
    IRQn_Type     irqn;
    GPIO_TypeDef *button_port;
    uint16_t      button_pin;
} pm_f1_encoder_pins_t;

typedef struct {
    const pm_f1_encoder_pins_t *pins;
    uint8_t                     count;
} pm_f1_encoder_ctx_t;

extern const pm_f1_encoder_ctx_t pm_f1_encoder_ctx;

#endif /* PM_PORT_STM32F1_H */
