/*
 * pm_port.h - the port registry: the single seam between the model and a chip.
 *
 * Contract:
 *  - There is exactly one implementation of pm_port_get() in a build, chosen
 *    at link time (stm32f1 backend, or the host mock).
 *  - Operations are const tables; no allocation happens at runtime.
 *  - `ctx` is owned by the port and is never dereferenced by upper layers.
 *  - Vendor types (GPIO_TypeDef, UART_HandleTypeDef, ...) must not appear in
 *    this header or in any header above it.
 */
#ifndef PM_PORT_H
#define PM_PORT_H

#include "pm_common.h"
#include "pm_err.h"

/* ------------------------------------------------------------------ GPIO */

typedef enum {
    PM_GPIO_DIR_IN = 0,
    PM_GPIO_DIR_OUT = 1
} pm_gpio_dir_t;

typedef enum {
    PM_GPIO_PULL_NONE = 0,
    PM_GPIO_PULL_UP,
    PM_GPIO_PULL_DOWN
} pm_gpio_pull_t;

typedef enum {
    PM_GPIO_LEVEL_LOW = 0,
    PM_GPIO_LEVEL_HIGH = 1
} pm_gpio_level_t;

typedef enum {
    PM_GPIO_SPEED_LOW = 0,
    PM_GPIO_SPEED_MEDIUM,
    PM_GPIO_SPEED_HIGH
} pm_gpio_speed_t;

/* A logical pin, not a hardware port/pin pair. The board maps id -> hardware. */
typedef struct {
    uint16_t id;
} pm_gpio_pin_t;

typedef struct {
    pm_gpio_dir_t   dir;
    pm_gpio_pull_t  pull;
    pm_gpio_speed_t speed;
    pm_gpio_level_t initial_level;   /* applied on configure when dir == OUT */
} pm_gpio_cfg_t;

typedef struct {
    pm_err_t (*configure)(void *ctx, pm_gpio_pin_t pin, const pm_gpio_cfg_t *cfg);
    pm_err_t (*write)(void *ctx, pm_gpio_pin_t pin, pm_gpio_level_t level);
    pm_err_t (*read)(void *ctx, pm_gpio_pin_t pin, pm_gpio_level_t *level);
} pm_gpio_ops_t;

/* ------------------------------------------------------------------ UART */

typedef enum {
    PM_UART_PARITY_NONE = 0,
    PM_UART_PARITY_EVEN,
    PM_UART_PARITY_ODD
} pm_uart_parity_t;

typedef enum {
    PM_UART_STOP_1 = 0,
    PM_UART_STOP_2
} pm_uart_stop_t;

typedef struct {
    uint8_t hw_id;            /* logical UART instance, board-defined */
    uint32_t baudrate;
    pm_uart_parity_t parity;
    pm_uart_stop_t   stop_bits;
    uint8_t          data_bits;   /* 8 only in this release */

    uint8_t  *rx_storage;     /* caller-owned, static */
    uint16_t  rx_size;        /* power of two, >= 32 */
    uint8_t  *tx_storage;     /* caller-owned, static */
    uint16_t  tx_size;        /* power of two, >= 64 */
} pm_uart_cfg_t;

typedef struct {
    uint32_t rx_bytes;      /* total bytes received */
    uint32_t tx_bytes;      /* total bytes transmitted */
    uint32_t rx_dropped;    /* bytes lost because the RX ring was full */
    uint32_t tx_dropped;    /* bytes rejected by pm_uart_write */
    uint32_t hw_errors;     /* overrun / framing / noise / parity events */
    uint16_t last_hw_error; /* backend-specific bit mask, 0 if none */
} pm_uart_stats_t;

typedef struct pm_uart pm_uart_t;

typedef struct {
    /* Down-calls: model -> port. All are called with interrupts enabled. */
    pm_err_t (*open)(void *ctx, pm_uart_t *u, const pm_uart_cfg_t *cfg);
    void     (*close)(void *ctx, pm_uart_t *u);
    pm_err_t (*start_tx)(void *ctx, pm_uart_t *u, const uint8_t *data, uint16_t len);
    void     (*enable_rx)(void *ctx, pm_uart_t *u);
    void     (*disable_rx)(void *ctx, pm_uart_t *u);
} pm_uart_ops_t;

/* ------------------------------------------------------------------- SPI */

typedef enum {
    PM_SPI_MODE_0 = 0,   /* CPOL = 0, CPHA = 0 */
    PM_SPI_MODE_1,       /* CPOL = 0, CPHA = 1 */
    PM_SPI_MODE_2,       /* CPOL = 1, CPHA = 0 */
    PM_SPI_MODE_3        /* CPOL = 1, CPHA = 1 */
} pm_spi_mode_t;

typedef enum {
    PM_SPI_MSB_FIRST = 0,
    PM_SPI_LSB_FIRST = 1
} pm_spi_bit_order_t;

typedef enum {
    PM_SPI_NSS_SOFT = 0,  /* chip select driven by an application GPIO */
    PM_SPI_NSS_HARD = 1   /* NSS pin driven by the peripheral; unsupported on
                           * this board (PA4/PB12 already taken), the F1 port
                           * returns PM_ERR_PARAM rather than faking it */
} pm_spi_nss_t;

typedef struct {
    uint8_t            hw_id;      /* logical SPI instance, board-defined */
    uint32_t           clock_hz;   /* requested SCK rate; the port rounds down */
    pm_spi_mode_t      mode;
    pm_spi_bit_order_t bit_order;
    pm_spi_nss_t       nss;
    bool               use_dma;    /* request; open() fails if unsupported */
} pm_spi_cfg_t;

typedef struct {
    uint32_t tx_bytes;    /* bytes clocked out */
    uint32_t rx_bytes;    /* bytes clocked in (same as tx in full duplex) */
    uint32_t transfers;   /* transfers completed without error */
    uint32_t aborted;     /* transfers stopped because the deadline expired */
    uint32_t errors;      /* hardware error events */
    uint16_t last_error;  /* backend-specific error mask, 0 if none */
} pm_spi_stats_t;

typedef struct pm_spi pm_spi_t;

typedef struct {
    /* Down-calls: model -> port. All are called with interrupts enabled.
     * open() reports the effective SCK rate after the prescaler is applied. */
    pm_err_t (*open)(void *ctx, pm_spi_t *s, const pm_spi_cfg_t *cfg,
                     uint32_t *actual_hz);
    void     (*close)(void *ctx, pm_spi_t *s);
    pm_err_t (*start)(void *ctx, pm_spi_t *s, const uint8_t *tx, uint8_t *rx,
                      uint16_t len);
    /* Stops an in-flight transfer and reports how many bytes were clocked. */
    pm_err_t (*abort)(void *ctx, pm_spi_t *s, uint16_t *done);
} pm_spi_ops_t;

/* ------------------------------------------------------------------- I2C */

typedef struct {
    uint8_t  hw_id;      /* logical I2C instance, board-defined */
    uint32_t clock_hz;   /* requested SCL rate; the port clamps/rounds */
} pm_i2c_cfg_t;

/* Normalized I2C error bits carried in pm_i2c_stats_t.last_error and passed to
 * pm_i2c_hw_error(). The port maps its vendor error code into this set so the
 * core can classify without any vendor header. The values deliberately match
 * the STM32F1 HAL I2C error code layout, which lets the F1 port pass
 * hi2c->ErrorCode unchanged; other ports must translate.
 *   AF      : the addressed device did not acknowledge (absent / wrong address)
 *   BERR    : misplaced START or STOP
 *   ARLO    : lost arbitration (another master on the bus)
 *   OVR     : overrun, a byte was dropped
 *   TIMEOUT : the peripheral raised its own timeout
 *   LOCAL   : port-detected failure, no peripheral error code (e.g. no ACK
 *             polling timeout); contract 12.4 */
#define PM_I2C_EERR_BERR    0x0001u
#define PM_I2C_EERR_ARLO    0x0002u
#define PM_I2C_EERR_AF      0x0004u
#define PM_I2C_EERR_OVR     0x0008u
#define PM_I2C_EERR_DMA     0x0010u
#define PM_I2C_EERR_TIMEOUT 0x0020u
#define PM_I2C_EERR_LOCAL   0x8000u

typedef struct {
    uint32_t tx_bytes;    /* bytes shifted out (including register addresses) */
    uint32_t rx_bytes;    /* bytes shifted in */
    uint32_t transfers;   /* transfers completed without error */
    uint32_t aborted;     /* transfers stopped because the deadline expired */
    uint32_t errors;      /* hardware error events, NACK included */
    uint16_t last_error;  /* PM_I2C_EERR_* mask, 0 if none */
} pm_i2c_stats_t;

typedef struct pm_i2c pm_i2c_t;

typedef struct {
    /* Down-calls: model -> port. All are called with interrupts enabled.
     * `addr7` is the 7-bit device address. The write_read variant keeps the
     * bus owned: START, write, repeated START, read, STOP. */
    pm_err_t (*open)(void *ctx, pm_i2c_t *i, const pm_i2c_cfg_t *cfg,
                     uint32_t *actual_hz);
    void     (*close)(void *ctx, pm_i2c_t *i);
    pm_err_t (*start_write)(void *ctx, pm_i2c_t *i, uint8_t addr7,
                            const uint8_t *data, uint16_t len);
    pm_err_t (*start_read)(void *ctx, pm_i2c_t *i, uint8_t addr7,
                           uint8_t *data, uint16_t len);
    pm_err_t (*start_write_read)(void *ctx, pm_i2c_t *i, uint8_t addr7,
                                 const uint8_t *wr, uint16_t wr_len,
                                 uint8_t *rd, uint16_t rd_len);
    /* Stops an in-flight transfer and reports how many bytes were moved. */
    pm_err_t (*abort)(void *ctx, pm_i2c_t *i, uint16_t *done);
} pm_i2c_ops_t;

/* ------------------------------------------------------------------- ADC */

typedef enum {
    PM_ADC_SAMPLE_1CYCLE_5 = 0,   /* ADC clock cycles: 1.5 */
    PM_ADC_SAMPLE_7CYCLES_5,
    PM_ADC_SAMPLE_13CYCLES_5,
    PM_ADC_SAMPLE_28CYCLES_5,
    PM_ADC_SAMPLE_41CYCLES_5,
    PM_ADC_SAMPLE_55CYCLES_5,
    PM_ADC_SAMPLE_71CYCLES_5,
    PM_ADC_SAMPLE_239CYCLES_5
} pm_adc_sample_t;

typedef struct {
    uint8_t         hw_id;        /* logical ADC instance, board-defined */
    uint8_t         channel;      /* logical channel, board-defined */
    pm_adc_sample_t sample_time;
    uint16_t        vref_mv;      /* 1000..5000, used by pm_adc_to_mv */
} pm_adc_cfg_t;

typedef struct {
    uint32_t conversions;   /* samples completed without error */
    uint32_t errors;        /* hardware error events */
    uint32_t aborted;       /* samples stopped because the deadline expired */
    uint16_t last_value;    /* raw sample of the last completed conversion */
    uint16_t last_error;    /* backend-specific error mask, 0 if none */
} pm_adc_stats_t;

typedef struct pm_adc pm_adc_t;

typedef struct {
    /* Down-calls: model -> port. All are called with interrupts enabled. */
    pm_err_t (*open)(void *ctx, pm_adc_t *a, const pm_adc_cfg_t *cfg);
    void     (*close)(void *ctx, pm_adc_t *a);
    pm_err_t (*start)(void *ctx, pm_adc_t *a);   /* one software-triggered sample */
    pm_err_t (*abort)(void *ctx, pm_adc_t *a);   /* stop an in-flight sample */
} pm_adc_ops_t;

/* ------------------------------------------------------------------- PWM */

typedef struct {
    uint8_t  hw_id;          /* logical PWM channel, board-defined */
    uint32_t freq_hz;        /* 1 .. 1000000 */
    uint16_t duty_permille;  /* 0 .. 1000 */
} pm_pwm_cfg_t;

typedef struct pm_pwm pm_pwm_t;

typedef struct {
    /* Down-calls: model -> port. All are called with interrupts enabled.
     * open() reports the effective frequency after the prescaler is applied.
     * set_freq() returns PM_ERR_BUSY when another channel on the same timer
     * is already open with a different period. */
    pm_err_t (*open)(void *ctx, pm_pwm_t *p, const pm_pwm_cfg_t *cfg,
                     uint32_t *actual_hz);
    void     (*close)(void *ctx, pm_pwm_t *p);
    pm_err_t (*set_freq)(void *ctx, pm_pwm_t *p, uint32_t freq_hz,
                         uint32_t *actual_hz);
    pm_err_t (*set_duty)(void *ctx, pm_pwm_t *p, uint16_t permille);
    pm_err_t (*enable)(void *ctx, pm_pwm_t *p, bool on);
} pm_pwm_ops_t;

/* ------------------------------------------------------------------ TONE */

typedef struct {
    uint8_t  hw_id;
    uint32_t freq_hz;
    uint16_t duty_permille;
} pm_tone_cfg_t;

typedef struct pm_tone pm_tone_t;

typedef struct {
    /* open() claims the timer through pm_timer.h and starts the tick.
     * off() stops the tick and leaves the pin at its idle level. */
    pm_err_t (*open)(void *ctx, pm_tone_t *t, const pm_tone_cfg_t *cfg);
    void     (*close)(void *ctx, pm_tone_t *t);
    pm_err_t (*set)(void *ctx, pm_tone_t *t, uint32_t freq_hz,
                    uint16_t duty_permille);
    pm_err_t (*off)(void *ctx, pm_tone_t *t);
} pm_tone_ops_t;

/* ----------------------------------------------------------------- TRACK */

typedef struct {
    uint8_t hw_id;
    uint8_t active_level;   /* pm_gpio_level_t value */
} pm_track_cfg_t;

typedef struct pm_track pm_track_t;

typedef struct {
    /* read() returns the raw electrical level (true = high). The polarity
     * decision belongs to the core. */
    pm_err_t (*open)(void *ctx, pm_track_t *t, const pm_track_cfg_t *cfg);
    void     (*close)(void *ctx, pm_track_t *t);
    pm_err_t (*read)(void *ctx, pm_track_t *t, bool *high);
} pm_track_ops_t;

/* --------------------------------------------------------------- ENCODER */

typedef struct {
    uint8_t hw_id;
    uint8_t invert;
    uint8_t has_button;   /* 0 = no push button on this module (the norm) */
} pm_encoder_cfg_t;

typedef struct pm_encoder pm_encoder_t;

typedef struct {
    /* open() configures both inputs and the edge interrupt, and reports the
     * AB state it found so the core can adopt it without counting a step.
     * Edges are delivered to the core with pm_encoder_hw_edge().
     * When cfg->has_button != 0, open() also configures the button input and
     * the port must provide read_button(); an unsupported combination is
     * rejected by the core with PM_ERR_DISABLED. */
    pm_err_t (*open)(void *ctx, pm_encoder_t *e, const pm_encoder_cfg_t *cfg,
                     uint8_t *initial_ab);
    void     (*close)(void *ctx, pm_encoder_t *e);

    /* Optional. Only called when the button was configured. Returns the
     * logical state (true = pressed); the port owns the electrical
     * convention because it chose the pull. NULL when there is no button. */
    pm_err_t (*read_button)(void *ctx, pm_encoder_t *e, bool *pressed);
} pm_encoder_ops_t;

/* ----------------------------------------------------------- port registry */

typedef struct {
    const pm_gpio_ops_t *gpio;
    void                *gpio_ctx;
    uint16_t             gpio_pin_count;   /* valid ids are [0, count) */

    const pm_uart_ops_t *uart;
    void                *uart_ctx;
    uint8_t              uart_count;       /* valid hw_id are [0, count) */

    const pm_spi_ops_t  *spi;
    void                *spi_ctx;
    uint8_t              spi_count;        /* valid hw_id are [0, count) */

    const pm_i2c_ops_t  *i2c;
    void                *i2c_ctx;
    uint8_t              i2c_count;        /* valid hw_id are [0, count) */

    const pm_adc_ops_t  *adc;
    void                *adc_ctx;
    uint8_t              adc_count;        /* valid hw_id are [0, count) */

    const pm_pwm_ops_t  *pwm;
    void                *pwm_ctx;
    uint8_t              pwm_count;        /* valid hw_id are [0, count) */

    const pm_tone_ops_t *tone;
    void                *tone_ctx;
    uint8_t              tone_count;

    const pm_track_ops_t *track;
    void                 *track_ctx;
    uint8_t               track_count;

    const pm_encoder_ops_t *encoder;
    void                   *encoder_ctx;
    uint8_t                 encoder_count;
} pm_port_t;

const pm_port_t *pm_port_get(void);

#endif /* PM_PORT_H */
