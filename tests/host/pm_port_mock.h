/*
 * pm_port_mock.h - test controls for the mock port.
 */
#ifndef PM_PORT_MOCK_H
#define PM_PORT_MOCK_H

#include "pm_port.h"

#define MOCK_GPIO_COUNT 4u
#define MOCK_UART_COUNT 2u
#define MOCK_SPI_COUNT  2u
#define MOCK_I2C_COUNT  2u
#define MOCK_ADC_COUNT  1u
#define MOCK_ADC_CHANNELS 2u
#define MOCK_PWM_COUNT  4u
#define MOCK_TONE_COUNT 1u
#define MOCK_TRACK_COUNT 1u
#define MOCK_ENCODER_COUNT 1u
#define MOCK_TX_CAPTURE_SIZE 4096u
#define MOCK_SPI_CAPTURE_SIZE 256u
#define MOCK_I2C_CAPTURE_SIZE 256u

void            mock_reset(void);

int             mock_gpio_is_configured(uint16_t id);
pm_gpio_level_t mock_gpio_level(uint16_t id);

int             mock_uart_is_open(uint8_t hw_id);
size_t          mock_uart_tx_len(void);
const uint8_t  *mock_uart_tx_data(void);

/* SPI: start() captures the TX bytes. The test then decides what the "ISR"
 * does: complete the transfer (loopback into RX) or raise an error. */
int             mock_spi_is_open(uint8_t hw_id);
int             mock_spi_uses_dma(uint8_t hw_id);
size_t          mock_spi_capture_len(uint8_t hw_id);
const uint8_t  *mock_spi_capture(uint8_t hw_id);
void            mock_spi_set_auto_complete(uint8_t hw_id, int on);
void            mock_spi_set_fail(uint8_t hw_id, int on);
void            mock_spi_set_progress(uint8_t hw_id, uint16_t bytes);
void            mock_spi_complete(uint8_t hw_id);

/* I2C: start_*() captures the write phase; a read phase is filled with
 * 0xC0, 0xC1, ... when it completes. */
int             mock_i2c_is_open(uint8_t hw_id);
int             mock_i2c_last_addr(uint8_t hw_id);
size_t          mock_i2c_capture_len(uint8_t hw_id);
const uint8_t  *mock_i2c_capture(uint8_t hw_id);
void            mock_i2c_set_auto_complete(uint8_t hw_id, int on);
void            mock_i2c_set_fail(uint8_t hw_id, int on);
void            mock_i2c_set_progress(uint8_t hw_id, uint16_t bytes);
void            mock_i2c_complete(uint8_t hw_id);

/* ADC: start() either fails, completes immediately with the configured raw
 * value, or leaves the conversion in flight for mock_adc_complete(). */
int             mock_adc_is_open(uint8_t hw_id);
int             mock_adc_last_channel(uint8_t hw_id);
void            mock_adc_set_value(uint16_t raw);
void            mock_adc_set_auto_complete(int on);
void            mock_adc_set_fail(int on);
void            mock_adc_complete(void);

/* PWM: the mock records what the model asked for and reports the requested
 * frequency as the effective one. */
int             mock_pwm_is_open(uint8_t hw_id);
uint32_t        mock_pwm_frequency(uint8_t hw_id);
uint16_t        mock_pwm_duty(uint8_t hw_id);
int             mock_pwm_is_enabled(uint8_t hw_id);

/* Tone: the mock records the last parameters and whether the tick is on. */
int             mock_tone_is_open(uint8_t hw_id);
int             mock_tone_is_active(uint8_t hw_id);
uint32_t        mock_tone_frequency(uint8_t hw_id);
uint16_t        mock_tone_duty(uint8_t hw_id);

/* Track: set the level the next read returns. */
void            mock_track_set_level(uint8_t hw_id, pm_gpio_level_t level);
int             mock_track_is_open(uint8_t hw_id);

/* Encoder: the AB state the port reports at open. Edges are injected with
 * pm_encoder_hw_edge() directly by the test. */
void            mock_encoder_set_initial_ab(uint8_t hw_id, uint8_t ab);
int             mock_encoder_is_open(uint8_t hw_id);

#endif /* PM_PORT_MOCK_H */
