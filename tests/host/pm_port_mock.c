/*
 * pm_port_mock.c - a fully in-memory port used by the host tests.
 *
 * It proves the contract claim "the model does not need hardware": the exact
 * same pm_core sources are compiled for the host against this file.
 *
 * TX behaviour: start_tx() copies the chunk into a capture buffer and returns
 * immediately. The test decides when to call pm_uart_hw_tx_complete(), which
 * is what the real TX interrupt would do.
 */
#include "pm_port_mock.h"
#include "pm_uart.h"
#if defined(PM_ENABLE_SPI)
#include "pm_spi.h"
#endif
#if defined(PM_ENABLE_I2C)
#include "pm_i2c.h"
#endif
#if defined(PM_ENABLE_ADC)
#include "pm_adc.h"
#endif
#if defined(PM_ENABLE_PWM)
#include "pm_pwm.h"
#endif
#if defined(PM_ENABLE_TONE)
#include "pm_tone.h"
#endif
#if defined(PM_ENABLE_TRACK)
#include "pm_track.h"
#endif
#if defined(PM_ENABLE_ENCODER)
#include "pm_encoder.h"
#endif
#include <string.h>

static pm_gpio_level_t s_level[MOCK_GPIO_COUNT];
static uint8_t         s_configured[MOCK_GPIO_COUNT];

static uint8_t s_uart_open[MOCK_UART_COUNT];
static uint8_t s_tx_capture[MOCK_TX_CAPTURE_SIZE];
static size_t  s_tx_len;

/* ------------------------------------------------------------------ GPIO */

static pm_err_t mock_gpio_configure(void *ctx, pm_gpio_pin_t pin,
                                    const pm_gpio_cfg_t *cfg)
{
    (void)ctx;
    if (cfg == NULL) {
        return PM_ERR_NULL;
    }
    if (pin.id >= MOCK_GPIO_COUNT) {
        return PM_ERR_RANGE;
    }
    s_configured[pin.id] = 1u;
    s_level[pin.id] = cfg->initial_level;
    return PM_OK;
}

static pm_err_t mock_gpio_write(void *ctx, pm_gpio_pin_t pin, pm_gpio_level_t level)
{
    (void)ctx;
    if (pin.id >= MOCK_GPIO_COUNT) {
        return PM_ERR_RANGE;
    }
    s_level[pin.id] = level;
    return PM_OK;
}

static pm_err_t mock_gpio_read(void *ctx, pm_gpio_pin_t pin, pm_gpio_level_t *level)
{
    (void)ctx;
    if ((pin.id >= MOCK_GPIO_COUNT) || (level == NULL)) {
        return PM_ERR_RANGE;
    }
    *level = s_level[pin.id];
    return PM_OK;
}

static const pm_gpio_ops_t s_gpio_ops = {
    .configure = mock_gpio_configure,
    .write     = mock_gpio_write,
    .read      = mock_gpio_read,
};

/* ------------------------------------------------------------------ UART */

static pm_err_t mock_uart_open(void *ctx, pm_uart_t *u, const pm_uart_cfg_t *cfg)
{
    (void)ctx;
    (void)u;
    if (cfg->hw_id >= MOCK_UART_COUNT) {
        return PM_ERR_RANGE;
    }
    if (s_uart_open[cfg->hw_id] != 0u) {
        return PM_ERR_BUSY;
    }
    s_uart_open[cfg->hw_id] = 1u;
    return PM_OK;
}

static void mock_uart_close(void *ctx, pm_uart_t *u)
{
    (void)ctx;
    if (u->hw_id < MOCK_UART_COUNT) {
        s_uart_open[u->hw_id] = 0u;
    }
}

static pm_err_t mock_uart_start_tx(void *ctx, pm_uart_t *u,
                                   const uint8_t *data, uint16_t len)
{
    (void)ctx;
    (void)u;

    if ((data == NULL) || (len == 0u)) {
        return PM_ERR_PARAM;
    }
    if ((s_tx_len + len) > MOCK_TX_CAPTURE_SIZE) {
        return PM_ERR_NOMEM;
    }
    memcpy(&s_tx_capture[s_tx_len], data, len);
    s_tx_len += len;
    return PM_OK;
}

static void mock_uart_enable_rx(void *ctx, pm_uart_t *u)
{
    (void)ctx;
    (void)u;
}

static void mock_uart_disable_rx(void *ctx, pm_uart_t *u)
{
    (void)ctx;
    (void)u;
}

static const pm_uart_ops_t s_uart_ops = {
    .open       = mock_uart_open,
    .close      = mock_uart_close,
    .start_tx   = mock_uart_start_tx,
    .enable_rx  = mock_uart_enable_rx,
    .disable_rx = mock_uart_disable_rx,
};

#if defined(PM_ENABLE_SPI)
/* ------------------------------------------------------------------- SPI */

static uint8_t        s_spi_open[MOCK_SPI_COUNT];
static uint8_t        s_spi_dma[MOCK_SPI_COUNT];
static uint8_t        s_spi_auto[MOCK_SPI_COUNT];
static uint8_t        s_spi_fail[MOCK_SPI_COUNT];
static uint16_t       s_spi_progress[MOCK_SPI_COUNT];
static pm_spi_t      *s_spi_owner[MOCK_SPI_COUNT];
static const uint8_t *s_spi_tx[MOCK_SPI_COUNT];
static uint8_t       *s_spi_rx[MOCK_SPI_COUNT];
static uint16_t       s_spi_len[MOCK_SPI_COUNT];
static uint8_t        s_spi_capture[MOCK_SPI_COUNT][MOCK_SPI_CAPTURE_SIZE];
static size_t         s_spi_capture_len[MOCK_SPI_COUNT];

static void mock_spi_loopback(uint8_t hw_id)
{
    if (s_spi_len[hw_id] != 0u) {
        memcpy(s_spi_rx[hw_id], s_spi_tx[hw_id], s_spi_len[hw_id]);
    }
}

static pm_err_t mock_spi_open(void *ctx, pm_spi_t *s, const pm_spi_cfg_t *cfg,
                              uint32_t *actual_hz)
{
    (void)ctx;
    (void)s;
    if (cfg->hw_id >= MOCK_SPI_COUNT) {
        return PM_ERR_RANGE;
    }
    if (s_spi_open[cfg->hw_id] != 0u) {
        return PM_ERR_BUSY;
    }
    s_spi_open[cfg->hw_id] = 1u;
    s_spi_dma[cfg->hw_id]  = cfg->use_dma ? 1u : 0u;
    *actual_hz = cfg->clock_hz;
    return PM_OK;
}

static void mock_spi_close(void *ctx, pm_spi_t *s)
{
    (void)ctx;
    if (s->hw_id < MOCK_SPI_COUNT) {
        s_spi_open[s->hw_id] = 0u;
    }
}

static pm_err_t mock_spi_start(void *ctx, pm_spi_t *s, const uint8_t *tx,
                               uint8_t *rx, uint16_t len)
{
    uint8_t id;

    (void)ctx;

    if ((s == NULL) || (tx == NULL) || (rx == NULL) || (len == 0u)) {
        return PM_ERR_PARAM;
    }
    id = s->hw_id;
    if (id >= MOCK_SPI_COUNT) {
        return PM_ERR_RANGE;
    }

    s_spi_owner[id] = s;
    s_spi_tx[id]    = tx;
    s_spi_rx[id]    = rx;
    s_spi_len[id]   = len;

    s_spi_capture_len[id] = (len > MOCK_SPI_CAPTURE_SIZE) ? MOCK_SPI_CAPTURE_SIZE : len;
    memcpy(s_spi_capture[id], tx, s_spi_capture_len[id]);

    if (s_spi_fail[id] != 0u) {
        pm_spi_hw_error(s, 0x0008u, s_spi_progress[id]);
    } else if (s_spi_auto[id] != 0u) {
        mock_spi_loopback(id);
        pm_spi_hw_done(s, len);
    }
    return PM_OK;
}

static pm_err_t mock_spi_abort(void *ctx, pm_spi_t *s, uint16_t *done)
{
    (void)ctx;
    if ((s == NULL) || (done == NULL)) {
        return PM_ERR_NULL;
    }
    if (s->hw_id >= MOCK_SPI_COUNT) {
        return PM_ERR_RANGE;
    }
    *done = s_spi_progress[s->hw_id];
    return PM_OK;
}

static const pm_spi_ops_t s_spi_ops = {
    .open  = mock_spi_open,
    .close = mock_spi_close,
    .start = mock_spi_start,
    .abort = mock_spi_abort,
};

#endif /* PM_ENABLE_SPI */

#if defined(PM_ENABLE_I2C)
/* ------------------------------------------------------------------- I2C */

static uint8_t        s_i2c_open[MOCK_I2C_COUNT];
static uint8_t        s_i2c_auto[MOCK_I2C_COUNT];
static uint8_t        s_i2c_fail[MOCK_I2C_COUNT];
static uint16_t       s_i2c_progress[MOCK_I2C_COUNT];
static pm_i2c_t      *s_i2c_owner[MOCK_I2C_COUNT];
static uint8_t        s_i2c_addr[MOCK_I2C_COUNT];
static uint16_t       s_i2c_wr_len[MOCK_I2C_COUNT];
static uint16_t       s_i2c_rd_len[MOCK_I2C_COUNT];
static uint8_t       *s_i2c_rd[MOCK_I2C_COUNT];
static uint8_t        s_i2c_capture[MOCK_I2C_COUNT][MOCK_I2C_CAPTURE_SIZE];
static size_t         s_i2c_capture_len[MOCK_I2C_COUNT];

static void mock_i2c_fill(uint8_t id)
{
    uint16_t i;

    if (s_i2c_rd[id] == NULL) {
        return;
    }
    for (i = 0u; i < s_i2c_rd_len[id]; i++) {
        s_i2c_rd[id][i] = (uint8_t)(0xC0u + i);
    }
}

static void mock_i2c_finish(uint8_t id)
{
    if ((id < MOCK_I2C_COUNT) && (s_i2c_owner[id] != NULL)) {
        mock_i2c_fill(id);
        pm_i2c_hw_done(s_i2c_owner[id], s_i2c_wr_len[id], s_i2c_rd_len[id]);
    }
}

static pm_err_t mock_i2c_begin(uint8_t id, pm_i2c_t *i, uint8_t addr7,
                               const uint8_t *wr, uint16_t wr_len,
                               uint8_t *rd, uint16_t rd_len)
{
    if ((i == NULL) || (id >= MOCK_I2C_COUNT)) {
        return PM_ERR_RANGE;
    }
    if (((wr_len != 0u) && (wr == NULL)) || ((rd_len != 0u) && (rd == NULL))) {
        return PM_ERR_PARAM;
    }

    s_i2c_owner[id] = i;
    s_i2c_addr[id]  = addr7;
    s_i2c_rd[id]    = rd;
    s_i2c_wr_len[id] = wr_len;
    s_i2c_rd_len[id] = rd_len;

    if (wr_len != 0u) {
        s_i2c_capture_len[id] =
            (wr_len > MOCK_I2C_CAPTURE_SIZE) ? MOCK_I2C_CAPTURE_SIZE : wr_len;
        memcpy(s_i2c_capture[id], wr, s_i2c_capture_len[id]);
    } else {
        s_i2c_capture_len[id] = 0u;
    }

    if (s_i2c_fail[id] != 0u) {
        pm_i2c_hw_error(i, 0x0004u, s_i2c_progress[id]);   /* AF / NACK */
    } else if (s_i2c_auto[id] != 0u) {
        mock_i2c_finish(id);
    }
    return PM_OK;
}

static pm_err_t mock_i2c_open(void *ctx, pm_i2c_t *i, const pm_i2c_cfg_t *cfg,
                              uint32_t *actual_hz)
{
    (void)ctx;
    (void)i;

    if (cfg->hw_id >= MOCK_I2C_COUNT) {
        return PM_ERR_RANGE;
    }
    if (s_i2c_open[cfg->hw_id] != 0u) {
        return PM_ERR_BUSY;
    }
    s_i2c_open[cfg->hw_id] = 1u;
    *actual_hz = cfg->clock_hz;
    return PM_OK;
}

static void mock_i2c_close(void *ctx, pm_i2c_t *i)
{
    (void)ctx;

    if ((i != NULL) && (i->hw_id < MOCK_I2C_COUNT)) {
        s_i2c_open[i->hw_id] = 0u;
        s_i2c_owner[i->hw_id] = NULL;
    }
}

static pm_err_t mock_i2c_start_write(void *ctx, pm_i2c_t *i, uint8_t addr7,
                                     const uint8_t *data, uint16_t len)
{
    (void)ctx;
    return mock_i2c_begin(i->hw_id, i, addr7, data, len, NULL, 0u);
}

static pm_err_t mock_i2c_start_read(void *ctx, pm_i2c_t *i, uint8_t addr7,
                                    uint8_t *data, uint16_t len)
{
    (void)ctx;
    return mock_i2c_begin(i->hw_id, i, addr7, NULL, 0u, data, len);
}

static pm_err_t mock_i2c_start_write_read(void *ctx, pm_i2c_t *i, uint8_t addr7,
                                          const uint8_t *wr, uint16_t wr_len,
                                          uint8_t *rd, uint16_t rd_len)
{
    (void)ctx;
    return mock_i2c_begin(i->hw_id, i, addr7, wr, wr_len, rd, rd_len);
}

static pm_err_t mock_i2c_abort(void *ctx, pm_i2c_t *i, uint16_t *done)
{
    (void)ctx;

    if ((i == NULL) || (done == NULL)) {
        return PM_ERR_NULL;
    }
    if (i->hw_id >= MOCK_I2C_COUNT) {
        return PM_ERR_RANGE;
    }
    *done = s_i2c_progress[i->hw_id];
    s_i2c_owner[i->hw_id] = NULL;
    return PM_OK;
}

static const pm_i2c_ops_t s_i2c_ops = {
    .open             = mock_i2c_open,
    .close            = mock_i2c_close,
    .start_write      = mock_i2c_start_write,
    .start_read       = mock_i2c_start_read,
    .start_write_read = mock_i2c_start_write_read,
    .abort            = mock_i2c_abort,
};

/* ---------------------------------------------------------- port registry */

#endif /* PM_ENABLE_I2C */

#if defined(PM_ENABLE_ADC)
/* ------------------------------------------------------------------- ADC */

static uint8_t    s_adc_open[MOCK_ADC_COUNT];
static uint8_t    s_adc_auto;
static uint8_t    s_adc_fail;
static uint8_t    s_adc_channel[MOCK_ADC_COUNT];
static uint16_t   s_adc_value;
static pm_adc_t  *s_adc_owner[MOCK_ADC_COUNT];

static pm_err_t mock_adc_open(void *ctx, pm_adc_t *a, const pm_adc_cfg_t *cfg)
{
    (void)ctx;

    if ((a == NULL) || (cfg == NULL)) {
        return PM_ERR_NULL;
    }
    if (cfg->hw_id >= MOCK_ADC_COUNT) {
        return PM_ERR_RANGE;
    }
    if (cfg->channel >= MOCK_ADC_CHANNELS) {
        return PM_ERR_RANGE;
    }
    if (s_adc_open[cfg->hw_id] != 0u) {
        return PM_ERR_BUSY;
    }

    s_adc_open[cfg->hw_id]    = 1u;
    s_adc_channel[cfg->hw_id] = cfg->channel;
    s_adc_owner[cfg->hw_id]   = a;
    return PM_OK;
}

static void mock_adc_close(void *ctx, pm_adc_t *a)
{
    (void)ctx;

    if ((a != NULL) && (a->hw_id < MOCK_ADC_COUNT)) {
        s_adc_open[a->hw_id]  = 0u;
        s_adc_owner[a->hw_id] = NULL;
    }
}

static pm_err_t mock_adc_start(void *ctx, pm_adc_t *a)
{
    (void)ctx;

    if ((a == NULL) || (a->hw_id >= MOCK_ADC_COUNT)) {
        return PM_ERR_RANGE;
    }

    if (s_adc_fail != 0u) {
        pm_adc_hw_error(a, 0x0040u);
    } else if (s_adc_auto != 0u) {
        pm_adc_hw_done(a, s_adc_value);
    }
    return PM_OK;
}

static pm_err_t mock_adc_abort(void *ctx, pm_adc_t *a)
{
    (void)ctx;

    if ((a == NULL) || (a->hw_id >= MOCK_ADC_COUNT)) {
        return PM_ERR_RANGE;
    }
    return PM_OK;
}

static const pm_adc_ops_t s_adc_ops = {
    .open  = mock_adc_open,
    .close = mock_adc_close,
    .start = mock_adc_start,
    .abort = mock_adc_abort,
};

#endif /* PM_ENABLE_ADC */

#if defined(PM_ENABLE_PWM)
/* ------------------------------------------------------------------- PWM */

static uint8_t  s_pwm_open[MOCK_PWM_COUNT];
static uint8_t  s_pwm_enabled[MOCK_PWM_COUNT];
static uint32_t s_pwm_freq[MOCK_PWM_COUNT];
static uint16_t s_pwm_duty[MOCK_PWM_COUNT];

static pm_err_t mock_pwm_open(void *ctx, pm_pwm_t *p, const pm_pwm_cfg_t *cfg,
                              uint32_t *actual_hz)
{
    (void)ctx;
    (void)p;

    if (cfg->hw_id >= MOCK_PWM_COUNT) {
        return PM_ERR_RANGE;
    }
    if (s_pwm_open[cfg->hw_id] != 0u) {
        return PM_ERR_BUSY;
    }

    s_pwm_open[cfg->hw_id]    = 1u;
    s_pwm_enabled[cfg->hw_id] = 1u;
    s_pwm_freq[cfg->hw_id]    = cfg->freq_hz;
    s_pwm_duty[cfg->hw_id]    = cfg->duty_permille;
    *actual_hz                = cfg->freq_hz;
    return PM_OK;
}

static void mock_pwm_close(void *ctx, pm_pwm_t *p)
{
    (void)ctx;

    if ((p != NULL) && (p->hw_id < MOCK_PWM_COUNT)) {
        s_pwm_open[p->hw_id]    = 0u;
        s_pwm_enabled[p->hw_id] = 0u;
        s_pwm_freq[p->hw_id]    = 0u;
        s_pwm_duty[p->hw_id]    = 0u;
    }
}

static pm_err_t mock_pwm_set_freq(void *ctx, pm_pwm_t *p, uint32_t freq_hz,
                                  uint32_t *actual_hz)
{
    (void)ctx;

    if ((p == NULL) || (actual_hz == NULL) || (p->hw_id >= MOCK_PWM_COUNT)) {
        return PM_ERR_RANGE;
    }
    s_pwm_freq[p->hw_id] = freq_hz;
    *actual_hz = freq_hz;
    return PM_OK;
}

static pm_err_t mock_pwm_set_duty(void *ctx, pm_pwm_t *p, uint16_t permille)
{
    (void)ctx;

    if ((p == NULL) || (p->hw_id >= MOCK_PWM_COUNT)) {
        return PM_ERR_RANGE;
    }
    s_pwm_duty[p->hw_id] = permille;
    return PM_OK;
}

static pm_err_t mock_pwm_enable(void *ctx, pm_pwm_t *p, bool on)
{
    (void)ctx;

    if ((p == NULL) || (p->hw_id >= MOCK_PWM_COUNT)) {
        return PM_ERR_RANGE;
    }
    s_pwm_enabled[p->hw_id] = on ? 1u : 0u;
    return PM_OK;
}

static const pm_pwm_ops_t s_pwm_ops = {
    .open     = mock_pwm_open,
    .close    = mock_pwm_close,
    .set_freq = mock_pwm_set_freq,
    .set_duty = mock_pwm_set_duty,
    .enable   = mock_pwm_enable,
};

#endif /* PM_ENABLE_PWM */

#if defined(PM_ENABLE_TONE)
/* ------------------------------------------------------------------ TONE */

static uint8_t  s_tone_open[MOCK_TONE_COUNT];
static uint8_t  s_tone_active[MOCK_TONE_COUNT];
static uint32_t s_tone_freq[MOCK_TONE_COUNT];
static uint16_t s_tone_duty[MOCK_TONE_COUNT];

static pm_err_t mock_tone_open(void *ctx, pm_tone_t *t, const pm_tone_cfg_t *cfg)
{
    (void)ctx;
    (void)t;

    if ((cfg == NULL) || (cfg->hw_id >= MOCK_TONE_COUNT)) {
        return PM_ERR_RANGE;
    }
    if (s_tone_open[cfg->hw_id] != 0u) {
        return PM_ERR_BUSY;
    }

    s_tone_open[cfg->hw_id]   = 1u;
    s_tone_active[cfg->hw_id] = 1u;
    s_tone_freq[cfg->hw_id]   = cfg->freq_hz;
    s_tone_duty[cfg->hw_id]   = cfg->duty_permille;
    return PM_OK;
}

static void mock_tone_close(void *ctx, pm_tone_t *t)
{
    (void)ctx;

    if ((t != NULL) && (t->hw_id < MOCK_TONE_COUNT)) {
        s_tone_open[t->hw_id]   = 0u;
        s_tone_active[t->hw_id] = 0u;
        s_tone_freq[t->hw_id]   = 0u;
        s_tone_duty[t->hw_id]   = 0u;
    }
}

static pm_err_t mock_tone_set(void *ctx, pm_tone_t *t, uint32_t freq_hz,
                              uint16_t duty_permille)
{
    (void)ctx;

    if ((t == NULL) || (t->hw_id >= MOCK_TONE_COUNT) ||
        (s_tone_open[t->hw_id] == 0u)) {
        return PM_ERR_STATE;
    }
    s_tone_freq[t->hw_id]   = freq_hz;
    s_tone_duty[t->hw_id]   = duty_permille;
    s_tone_active[t->hw_id] = 1u;
    return PM_OK;
}

static pm_err_t mock_tone_off(void *ctx, pm_tone_t *t)
{
    (void)ctx;

    if ((t == NULL) || (t->hw_id >= MOCK_TONE_COUNT) ||
        (s_tone_open[t->hw_id] == 0u)) {
        return PM_ERR_STATE;
    }
    s_tone_active[t->hw_id] = 0u;
    return PM_OK;
}

static const pm_tone_ops_t s_tone_ops = {
    .open  = mock_tone_open,
    .close = mock_tone_close,
    .set   = mock_tone_set,
    .off   = mock_tone_off,
};

#endif /* PM_ENABLE_TONE */

#if defined(PM_ENABLE_TRACK)
/* ----------------------------------------------------------------- TRACK */

static uint8_t         s_track_open[MOCK_TRACK_COUNT];
static pm_gpio_level_t s_track_level[MOCK_TRACK_COUNT] = { PM_GPIO_LEVEL_HIGH };

static pm_err_t mock_track_open(void *ctx, pm_track_t *t,
                                const pm_track_cfg_t *cfg)
{
    (void)ctx;
    (void)t;

    if ((cfg == NULL) || (cfg->hw_id >= MOCK_TRACK_COUNT)) {
        return PM_ERR_RANGE;
    }
    if (s_track_open[cfg->hw_id] != 0u) {
        return PM_ERR_BUSY;
    }
    s_track_open[cfg->hw_id] = 1u;
    return PM_OK;
}

static void mock_track_close(void *ctx, pm_track_t *t)
{
    (void)ctx;

    if ((t != NULL) && (t->hw_id < MOCK_TRACK_COUNT)) {
        s_track_open[t->hw_id] = 0u;
    }
}

static pm_err_t mock_track_read(void *ctx, pm_track_t *t, bool *high)
{
    (void)ctx;

    if ((t == NULL) || (high == NULL) || (t->hw_id >= MOCK_TRACK_COUNT)) {
        return PM_ERR_RANGE;
    }
    if (s_track_open[t->hw_id] == 0u) {
        return PM_ERR_STATE;
    }
    *high = (s_track_level[t->hw_id] == PM_GPIO_LEVEL_HIGH);
    return PM_OK;
}

static const pm_track_ops_t s_track_ops = {
    .open  = mock_track_open,
    .close = mock_track_close,
    .read  = mock_track_read,
};

#endif /* PM_ENABLE_TRACK */

#if defined(PM_ENABLE_ENCODER)
/* --------------------------------------------------------------- ENCODER */

static uint8_t s_encoder_open[MOCK_ENCODER_COUNT];
static uint8_t s_encoder_ab[MOCK_ENCODER_COUNT];

static pm_err_t mock_encoder_open(void *ctx, pm_encoder_t *e,
                                  const pm_encoder_cfg_t *cfg,
                                  uint8_t *initial_ab)
{
    (void)ctx;
    (void)e;

    if ((cfg == NULL) || (initial_ab == NULL) ||
        (cfg->hw_id >= MOCK_ENCODER_COUNT)) {
        return PM_ERR_RANGE;
    }
    if (s_encoder_open[cfg->hw_id] != 0u) {
        return PM_ERR_BUSY;
    }
    s_encoder_open[cfg->hw_id] = 1u;
    *initial_ab = s_encoder_ab[cfg->hw_id];
    return PM_OK;
}

static void mock_encoder_close(void *ctx, pm_encoder_t *e)
{
    (void)ctx;

    if ((e != NULL) && (e->hw_id < MOCK_ENCODER_COUNT)) {
        s_encoder_open[e->hw_id] = 0u;
    }
}

static const pm_encoder_ops_t s_encoder_ops = {
    .open  = mock_encoder_open,
    .close = mock_encoder_close,
};

#endif /* PM_ENABLE_ENCODER */

static const pm_port_t s_port = {
    .gpio           = &s_gpio_ops,
    .gpio_ctx       = NULL,
    .gpio_pin_count = (uint16_t)MOCK_GPIO_COUNT,
    .uart           = &s_uart_ops,
    .uart_ctx       = NULL,
    .uart_count     = (uint8_t)MOCK_UART_COUNT,
#if defined(PM_ENABLE_SPI)
    .spi            = &s_spi_ops,
    .spi_ctx        = NULL,
    .spi_count      = (uint8_t)MOCK_SPI_COUNT,
#endif
#if defined(PM_ENABLE_I2C)
    .i2c            = &s_i2c_ops,
    .i2c_ctx        = NULL,
    .i2c_count      = (uint8_t)MOCK_I2C_COUNT,
#endif
#if defined(PM_ENABLE_ADC)
    .adc            = &s_adc_ops,
    .adc_ctx        = NULL,
    .adc_count      = (uint8_t)MOCK_ADC_COUNT,
#endif
#if defined(PM_ENABLE_PWM)
    .pwm            = &s_pwm_ops,
    .pwm_ctx        = NULL,
    .pwm_count      = (uint8_t)MOCK_PWM_COUNT,
#endif
#if defined(PM_ENABLE_TONE)
    .tone           = &s_tone_ops,
    .tone_ctx       = NULL,
    .tone_count     = (uint8_t)MOCK_TONE_COUNT,
#endif
#if defined(PM_ENABLE_TRACK)
    .track          = &s_track_ops,
    .track_ctx      = NULL,
    .track_count    = (uint8_t)MOCK_TRACK_COUNT,
#endif
#if defined(PM_ENABLE_ENCODER)
    .encoder        = &s_encoder_ops,
    .encoder_ctx    = NULL,
    .encoder_count  = (uint8_t)MOCK_ENCODER_COUNT,
#endif
};

const pm_port_t *pm_port_get(void)
{
    return &s_port;
}

/* ------------------------------------------------------------ test controls */

void mock_reset(void)
{
    memset(s_level, 0, sizeof(s_level));
    memset(s_configured, 0, sizeof(s_configured));
    memset(s_uart_open, 0, sizeof(s_uart_open));
    memset(s_tx_capture, 0, sizeof(s_tx_capture));
    s_tx_len = 0u;

#if defined(PM_ENABLE_SPI)
    memset(s_spi_open, 0, sizeof(s_spi_open));
    memset(s_spi_dma, 0, sizeof(s_spi_dma));
    memset(s_spi_auto, 0, sizeof(s_spi_auto));
    memset(s_spi_fail, 0, sizeof(s_spi_fail));
    memset(s_spi_progress, 0, sizeof(s_spi_progress));
    memset(s_spi_owner, 0, sizeof(s_spi_owner));
    memset(s_spi_len, 0, sizeof(s_spi_len));
    memset(s_spi_capture, 0, sizeof(s_spi_capture));
    memset(s_spi_capture_len, 0, sizeof(s_spi_capture_len));
#endif

#if defined(PM_ENABLE_I2C)
    memset(s_i2c_open, 0, sizeof(s_i2c_open));
    memset(s_i2c_auto, 0, sizeof(s_i2c_auto));
    memset(s_i2c_fail, 0, sizeof(s_i2c_fail));
    memset(s_i2c_progress, 0, sizeof(s_i2c_progress));
    memset(s_i2c_owner, 0, sizeof(s_i2c_owner));
    memset(s_i2c_addr, 0, sizeof(s_i2c_addr));
    memset(s_i2c_wr_len, 0, sizeof(s_i2c_wr_len));
    memset(s_i2c_rd_len, 0, sizeof(s_i2c_rd_len));
    memset(s_i2c_rd, 0, sizeof(s_i2c_rd));
    memset(s_i2c_capture, 0, sizeof(s_i2c_capture));
    memset(s_i2c_capture_len, 0, sizeof(s_i2c_capture_len));
#endif

#if defined(PM_ENABLE_ADC)
    memset(s_adc_open, 0, sizeof(s_adc_open));
    memset(s_adc_channel, 0, sizeof(s_adc_channel));
    memset(s_adc_owner, 0, sizeof(s_adc_owner));
    s_adc_auto  = 0u;
    s_adc_fail  = 0u;
    s_adc_value = 0u;
#endif

#if defined(PM_ENABLE_PWM)
    memset(s_pwm_open, 0, sizeof(s_pwm_open));
    memset(s_pwm_enabled, 0, sizeof(s_pwm_enabled));
    memset(s_pwm_freq, 0, sizeof(s_pwm_freq));
    memset(s_pwm_duty, 0, sizeof(s_pwm_duty));
#endif

#if defined(PM_ENABLE_TONE)
    memset(s_tone_open, 0, sizeof(s_tone_open));
    memset(s_tone_active, 0, sizeof(s_tone_active));
    memset(s_tone_freq, 0, sizeof(s_tone_freq));
    memset(s_tone_duty, 0, sizeof(s_tone_duty));
#endif

#if defined(PM_ENABLE_TRACK)
    memset(s_track_open, 0, sizeof(s_track_open));
    {
        uint8_t i;

        for (i = 0u; i < MOCK_TRACK_COUNT; i++) {
            s_track_level[i] = PM_GPIO_LEVEL_HIGH;
        }
    }
#endif

#if defined(PM_ENABLE_ENCODER)
    memset(s_encoder_open, 0, sizeof(s_encoder_open));
    memset(s_encoder_ab, 0, sizeof(s_encoder_ab));
#endif
}

int mock_gpio_is_configured(uint16_t id)
{
    return (id < MOCK_GPIO_COUNT) ? (int)s_configured[id] : 0;
}

pm_gpio_level_t mock_gpio_level(uint16_t id)
{
    return (id < MOCK_GPIO_COUNT) ? s_level[id] : PM_GPIO_LEVEL_LOW;
}

int mock_uart_is_open(uint8_t hw_id)
{
    return (hw_id < MOCK_UART_COUNT) ? (int)s_uart_open[hw_id] : 0;
}

size_t mock_uart_tx_len(void)
{
    return s_tx_len;
}

const uint8_t *mock_uart_tx_data(void)
{
    return s_tx_capture;
}

#if defined(PM_ENABLE_SPI)
int mock_spi_is_open(uint8_t hw_id)
{
    return (hw_id < MOCK_SPI_COUNT) ? (int)s_spi_open[hw_id] : 0;
}

int mock_spi_uses_dma(uint8_t hw_id)
{
    return (hw_id < MOCK_SPI_COUNT) ? (int)s_spi_dma[hw_id] : 0;
}

size_t mock_spi_capture_len(uint8_t hw_id)
{
    return (hw_id < MOCK_SPI_COUNT) ? s_spi_capture_len[hw_id] : 0u;
}

const uint8_t *mock_spi_capture(uint8_t hw_id)
{
    return (hw_id < MOCK_SPI_COUNT) ? s_spi_capture[hw_id] : NULL;
}

void mock_spi_set_auto_complete(uint8_t hw_id, int on)
{
    if (hw_id < MOCK_SPI_COUNT) {
        s_spi_auto[hw_id] = (on != 0) ? 1u : 0u;
    }
}

void mock_spi_set_fail(uint8_t hw_id, int on)
{
    if (hw_id < MOCK_SPI_COUNT) {
        s_spi_fail[hw_id] = (on != 0) ? 1u : 0u;
    }
}

void mock_spi_set_progress(uint8_t hw_id, uint16_t bytes)
{
    if (hw_id < MOCK_SPI_COUNT) {
        s_spi_progress[hw_id] = bytes;
    }
}

void mock_spi_complete(uint8_t hw_id)
{
    if ((hw_id >= MOCK_SPI_COUNT) || (s_spi_owner[hw_id] == NULL)) {
        return;
    }
    mock_spi_loopback(hw_id);
    pm_spi_hw_done(s_spi_owner[hw_id], s_spi_len[hw_id]);
}

#endif /* PM_ENABLE_SPI */

#if defined(PM_ENABLE_I2C)
int mock_i2c_is_open(uint8_t hw_id)
{
    return (hw_id < MOCK_I2C_COUNT) ? (int)s_i2c_open[hw_id] : 0;
}

int mock_i2c_last_addr(uint8_t hw_id)
{
    return (hw_id < MOCK_I2C_COUNT) ? (int)s_i2c_addr[hw_id] : -1;
}

size_t mock_i2c_capture_len(uint8_t hw_id)
{
    return (hw_id < MOCK_I2C_COUNT) ? s_i2c_capture_len[hw_id] : 0u;
}

const uint8_t *mock_i2c_capture(uint8_t hw_id)
{
    return (hw_id < MOCK_I2C_COUNT) ? s_i2c_capture[hw_id] : NULL;
}

void mock_i2c_set_auto_complete(uint8_t hw_id, int on)
{
    if (hw_id < MOCK_I2C_COUNT) {
        s_i2c_auto[hw_id] = (on != 0) ? 1u : 0u;
    }
}

void mock_i2c_set_fail(uint8_t hw_id, int on)
{
    if (hw_id < MOCK_I2C_COUNT) {
        s_i2c_fail[hw_id] = (on != 0) ? 1u : 0u;
    }
}

void mock_i2c_set_progress(uint8_t hw_id, uint16_t bytes)
{
    if (hw_id < MOCK_I2C_COUNT) {
        s_i2c_progress[hw_id] = bytes;
    }
}

void mock_i2c_complete(uint8_t hw_id)
{
    mock_i2c_finish(hw_id);
}

#endif /* PM_ENABLE_I2C */

#if defined(PM_ENABLE_ADC)
int mock_adc_is_open(uint8_t hw_id)
{
    return (hw_id < MOCK_ADC_COUNT) ? (int)s_adc_open[hw_id] : 0;
}

int mock_adc_last_channel(uint8_t hw_id)
{
    return (hw_id < MOCK_ADC_COUNT) ? (int)s_adc_channel[hw_id] : -1;
}

void mock_adc_set_value(uint16_t raw)
{
    s_adc_value = raw;
}

void mock_adc_set_auto_complete(int on)
{
    s_adc_auto = (on != 0) ? 1u : 0u;
}

void mock_adc_set_fail(int on)
{
    s_adc_fail = (on != 0) ? 1u : 0u;
}

void mock_adc_complete(void)
{
    if (s_adc_owner[0] != NULL) {
        pm_adc_hw_done(s_adc_owner[0], s_adc_value);
    }
}

#endif /* PM_ENABLE_ADC */

#if defined(PM_ENABLE_PWM)
int mock_pwm_is_open(uint8_t hw_id)
{
    return (hw_id < MOCK_PWM_COUNT) ? (int)s_pwm_open[hw_id] : 0;
}

uint32_t mock_pwm_frequency(uint8_t hw_id)
{
    return (hw_id < MOCK_PWM_COUNT) ? s_pwm_freq[hw_id] : 0u;
}

uint16_t mock_pwm_duty(uint8_t hw_id)
{
    return (hw_id < MOCK_PWM_COUNT) ? s_pwm_duty[hw_id] : 0u;
}

int mock_pwm_is_enabled(uint8_t hw_id)
{
    return (hw_id < MOCK_PWM_COUNT) ? (int)s_pwm_enabled[hw_id] : 0;
}

#endif /* PM_ENABLE_PWM */

#if defined(PM_ENABLE_TONE)
int mock_tone_is_open(uint8_t hw_id)
{
    return (hw_id < MOCK_TONE_COUNT) ? (int)s_tone_open[hw_id] : 0;
}

int mock_tone_is_active(uint8_t hw_id)
{
    return (hw_id < MOCK_TONE_COUNT) ? (int)s_tone_active[hw_id] : 0;
}

uint32_t mock_tone_frequency(uint8_t hw_id)
{
    return (hw_id < MOCK_TONE_COUNT) ? s_tone_freq[hw_id] : 0u;
}

uint16_t mock_tone_duty(uint8_t hw_id)
{
    return (hw_id < MOCK_TONE_COUNT) ? s_tone_duty[hw_id] : 0u;
}

#endif /* PM_ENABLE_TONE */

#if defined(PM_ENABLE_TRACK)
void mock_track_set_level(uint8_t hw_id, pm_gpio_level_t level)
{
    if (hw_id < MOCK_TRACK_COUNT) {
        s_track_level[hw_id] = level;
    }
}

int mock_track_is_open(uint8_t hw_id)
{
    return (hw_id < MOCK_TRACK_COUNT) ? (int)s_track_open[hw_id] : 0;
}

#endif /* PM_ENABLE_TRACK */

#if defined(PM_ENABLE_ENCODER)
void mock_encoder_set_initial_ab(uint8_t hw_id, uint8_t ab)
{
    if (hw_id < MOCK_ENCODER_COUNT) {
        s_encoder_ab[hw_id] = (uint8_t)(ab & 0x03u);
    }
}

int mock_encoder_is_open(uint8_t hw_id)
{
    return (hw_id < MOCK_ENCODER_COUNT) ? (int)s_encoder_open[hw_id] : 0;
}

#endif /* PM_ENABLE_ENCODER */
