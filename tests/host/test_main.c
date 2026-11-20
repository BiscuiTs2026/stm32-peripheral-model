/*
 * test_main.c - host-side tests for the vendor-free core.
 *
 * The same src/core/*.c files that are compiled into the firmware are
 * compiled here against a mock port and a host OSAL. No hardware involved.
 */
#include <stdio.h>
#include <string.h>

#include "pm_err.h"
#include "pm_ringbuf.h"
#include "pm_event.h"
#include "pm_gpio.h"
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
#if defined(PM_ENABLE_RELAY)
#include "pm_relay.h"
#endif
#if defined(PM_ENABLE_MOTOR)
#include "pm_motor.h"
#endif
#if defined(PM_ENABLE_SERVO)
#include "pm_servo.h"
#endif
#include "pm_timer.h"
#include "pm_port_mock.h"

void host_time_advance(uint32_t ms);
void host_time_reset(void);

static int s_failures;
static int s_checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        s_checks++;                                                          \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            s_failures++;                                                    \
        }                                                                    \
    } while (0)

/* --------------------------------------------------------------- ringbuf */

static void test_ringbuf(void)
{
    uint8_t storage[8];
    pm_ringbuf_t rb;
    uint8_t out[8];
    uint8_t src[16];

    CHECK(pm_rb_init(NULL, storage, 8) == PM_ERR_NULL);
    CHECK(pm_rb_init(&rb, NULL, 8) == PM_ERR_NULL);
    CHECK(pm_rb_init(&rb, storage, 3) == PM_ERR_PARAM);      /* not power of 2 */
    CHECK(pm_rb_init(&rb, storage, 0) == PM_ERR_PARAM);
    CHECK(pm_rb_init(&rb, storage, 8) == PM_OK);

    CHECK(pm_rb_free(&rb) == 7);                             /* one slot reserved */
    memset(src, 0xA5, sizeof(src));
    CHECK(pm_rb_write(&rb, src, 5) == 5);
    CHECK(pm_rb_used(&rb) == 5);
    CHECK(pm_rb_free(&rb) == 2);

    CHECK(pm_rb_write(&rb, src, 5) == 2);                    /* only 2 slots left */
    CHECK(pm_rb_dropped(&rb) == 3);

    CHECK(pm_rb_read(&rb, out, sizeof(out)) == 7);
    CHECK(out[0] == 0xA5);
    CHECK(pm_rb_used(&rb) == 0);
    CHECK(pm_rb_read(&rb, out, sizeof(out)) == 0);

    /* Wrap-around: write/read several times so head passes the end. */
    CHECK(pm_rb_write(&rb, src, 6) == 6);
    CHECK(pm_rb_read(&rb, out, 4) == 4);
    CHECK(pm_rb_write(&rb, src, 5) == 5);
    CHECK(pm_rb_read(&rb, out, 8) == 7);
    CHECK(pm_rb_used(&rb) == 0);

    /* consume() advances the tail without copying. */
    CHECK(pm_rb_write(&rb, src, 7) == 7);
    pm_rb_consume(&rb, 3);
    CHECK(pm_rb_used(&rb) == 4);
    pm_rb_consume(&rb, 100);                                 /* clamps */
    CHECK(pm_rb_used(&rb) == 0);
}

/* ----------------------------------------------------------------- evtq */

static void test_evtq(void)
{
    pm_evtq_t q;
    pm_evt_t e;

    pm_evtq_init(&q);

    pm_evtq_push(&q, PM_EVT_UART_RX_DATA, 1u, 0u);
    pm_evtq_push(&q, PM_EVT_UART_RX_DATA, 1u, 0u);           /* de-duplicated */
    CHECK(pm_evtq_pop(&q, &e) == true);
    CHECK(e.id == PM_EVT_UART_RX_DATA);
    CHECK(pm_evtq_pop(&q, &e) == false);

    pm_evtq_push(&q, PM_EVT_UART_RX_DATA, 1u, 0u);
    pm_evtq_push(&q, PM_EVT_UART_RX_DATA, 2u, 0u);           /* different src */
    CHECK(pm_evtq_pop(&q, &e) == true);
    CHECK(e.src == 1u);
    CHECK(pm_evtq_pop(&q, &e) == true);
    CHECK(e.src == 2u);

    /* Fill beyond depth: the newest is dropped and counted.
     * Capacity is PM_EVTQ_DEPTH-1 because one slot separates empty from full. */
    for (uint8_t i = 0u; i < PM_EVTQ_DEPTH + 4u; i++) {
        pm_evtq_push(&q, PM_EVT_UART_ERROR, i, 0u);
    }
    CHECK(pm_evtq_dropped(&q) == 5u);
    {
        uint8_t n = 0u;
        while (pm_evtq_pop(&q, &e)) {
            n++;
        }
        CHECK(n == PM_EVTQ_DEPTH - 1u);                      /* one slot reserved */
    }
}

/* ----------------------------------------------------------------- gpio */

static void test_gpio(void)
{
    pm_gpio_pin_t pin0;
    pm_gpio_pin_t bad;
    pm_gpio_level_t level;
    pm_gpio_cfg_t cfg;

    mock_reset();
    pin0.id = 0u;
    bad.id  = 99u;

    memset(&cfg, 0, sizeof(cfg));
    cfg.dir = PM_GPIO_DIR_OUT;
    cfg.initial_level = PM_GPIO_LEVEL_HIGH;

    CHECK(pm_gpio_configure(pin0, NULL) == PM_ERR_NULL);
    CHECK(pm_gpio_configure(bad, &cfg) == PM_ERR_RANGE);
    CHECK(pm_gpio_configure(pin0, &cfg) == PM_OK);
    CHECK(mock_gpio_is_configured(0u) == 1);
    CHECK(mock_gpio_level(0u) == PM_GPIO_LEVEL_HIGH);

    CHECK(pm_gpio_write(pin0, PM_GPIO_LEVEL_LOW) == PM_OK);
    CHECK(pm_gpio_read(pin0, &level) == PM_OK);
    CHECK(level == PM_GPIO_LEVEL_LOW);

    CHECK(pm_gpio_toggle(pin0) == PM_OK);
    CHECK(pm_gpio_read(pin0, &level) == PM_OK);
    CHECK(level == PM_GPIO_LEVEL_HIGH);

    CHECK(pm_gpio_read(pin0, NULL) == PM_ERR_NULL);
    CHECK(pm_gpio_read(bad, &level) == PM_ERR_RANGE);
    CHECK(level == PM_GPIO_LEVEL_LOW);      /* output defined on error */
}

/* ----------------------------------------------------------------- uart */

static pm_uart_t s_uart;
static uint8_t   s_rx[64];
static uint8_t   s_tx[64];

static void make_cfg(pm_uart_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->hw_id      = 1u;
    cfg->baudrate   = 115200u;
    cfg->parity     = PM_UART_PARITY_NONE;
    cfg->stop_bits  = PM_UART_STOP_1;
    cfg->data_bits  = 8u;
    cfg->rx_storage = s_rx;
    cfg->rx_size    = (uint16_t)sizeof(s_rx);
    cfg->tx_storage = s_tx;
    cfg->tx_size    = (uint16_t)sizeof(s_tx);
}

static int s_cb_count;
static pm_evt_id_t s_last_evt;

static void on_event(pm_uart_t *u, const pm_evt_t *evt, void *ctx)
{
    (void)u;
    (void)ctx;
    s_cb_count++;
    s_last_evt = evt->id;
}

static void test_uart_validation(void)
{
    pm_uart_cfg_t cfg;

    mock_reset();
    memset(&s_uart, 0, sizeof(s_uart));
    make_cfg(&cfg);

    CHECK(pm_uart_init(NULL, &cfg) == PM_ERR_NULL);
    CHECK(pm_uart_init(&s_uart, NULL) == PM_ERR_NULL);

    cfg.baudrate = 0u;
    CHECK(pm_uart_init(&s_uart, &cfg) == PM_ERR_PARAM);

    make_cfg(&cfg);
    cfg.data_bits = 9u;
    CHECK(pm_uart_init(&s_uart, &cfg) == PM_ERR_PARAM);

    make_cfg(&cfg);
    cfg.rx_size = 48u;                       /* not a power of two */
    CHECK(pm_uart_init(&s_uart, &cfg) == PM_ERR_PARAM);

    make_cfg(&cfg);
    cfg.hw_id = 9u;
    CHECK(pm_uart_init(&s_uart, &cfg) == PM_ERR_RANGE);

    make_cfg(&cfg);
    CHECK(pm_uart_init(&s_uart, &cfg) == PM_OK);
    CHECK(mock_uart_is_open(1u) == 1);

    /* A second init on the same hardware instance is a lifecycle error. */
    CHECK(pm_uart_init(&s_uart, &cfg) == PM_ERR_BUSY);

    CHECK(pm_uart_deinit(&s_uart) == PM_OK);
    CHECK(mock_uart_is_open(1u) == 0);
    CHECK(pm_uart_deinit(&s_uart) == PM_OK);     /* idempotent */
}

static void test_uart_tx(void)
{
    pm_uart_cfg_t cfg;
    pm_uart_stats_t stats;
    pm_evt_t evt;
    size_t written = 0u;
    static const uint8_t msg[] = "hello";

    mock_reset();
    memset(&s_uart, 0, sizeof(s_uart));
    host_time_reset();
    s_cb_count = 0;
    make_cfg(&cfg);

    CHECK(pm_uart_init(&s_uart, &cfg) == PM_OK);
    CHECK(pm_uart_write(&s_uart, msg, sizeof(msg) - 1u, &written,
                        PM_UART_TIMEOUT_NOW) == PM_OK);
    CHECK(written == 5u);
    CHECK(mock_uart_tx_len() == 5u);
    CHECK(memcmp(mock_uart_tx_data(), "hello", 5u) == 0);

    /* Nothing is consumed from the ring until the TX interrupt fires. */
    CHECK(pm_uart_get_stats(&s_uart, &stats) == PM_OK);
    CHECK(stats.tx_bytes == 0u);

    pm_uart_hw_tx_complete(&s_uart);
    CHECK(pm_uart_get_stats(&s_uart, &stats) == PM_OK);
    CHECK(stats.tx_bytes == 5u);

    CHECK(pm_uart_poll(&s_uart, &evt, NULL, NULL) == true);
    CHECK(evt.id == PM_EVT_UART_TX_DONE);
    CHECK(pm_uart_poll(&s_uart, &evt, NULL, NULL) == false);

    /* Callback path: runs on poll, never in the "ISR". */
    (void)pm_uart_write(&s_uart, msg, 1u, NULL, PM_UART_TIMEOUT_NOW);
    pm_uart_hw_tx_complete(&s_uart);
    CHECK(s_cb_count == 0);
    CHECK(pm_uart_poll(&s_uart, NULL, on_event, NULL) == true);
    CHECK(s_cb_count == 1);
    CHECK(s_last_evt == PM_EVT_UART_TX_DONE);
}

static void test_uart_rx(void)
{
    pm_uart_cfg_t cfg;
    pm_uart_stats_t stats;
    pm_evt_t evt;
    uint8_t buf[8];
    size_t got = 0u;

    mock_reset();
    memset(&s_uart, 0, sizeof(s_uart));
    make_cfg(&cfg);
    CHECK(pm_uart_init(&s_uart, &cfg) == PM_OK);

    CHECK(pm_uart_read(&s_uart, buf, sizeof(buf), &got) == PM_ERR_AGAIN);
    CHECK(got == 0u);

    pm_uart_hw_rx_byte(&s_uart, 'A');      /* pretending to be the RX ISR */
    pm_uart_hw_rx_byte(&s_uart, 'B');

    CHECK(pm_uart_poll(&s_uart, &evt, NULL, NULL) == true);
    CHECK(evt.id == PM_EVT_UART_RX_DATA);
    CHECK(pm_uart_poll(&s_uart, &evt, NULL, NULL) == false);   /* de-duplicated */

    CHECK(pm_uart_read(&s_uart, buf, sizeof(buf), &got) == PM_OK);
    CHECK(got == 2u);
    CHECK(buf[0] == 'A');
    CHECK(buf[1] == 'B');

    CHECK(pm_uart_get_stats(&s_uart, &stats) == PM_OK);
    CHECK(stats.rx_bytes == 2u);
    CHECK(stats.rx_dropped == 0u);

    CHECK(pm_uart_flush_rx(&s_uart) == PM_OK);
    CHECK(pm_uart_read(&s_uart, buf, sizeof(buf), &got) == PM_ERR_AGAIN);
}

static void test_uart_timeout(void)
{
    pm_uart_cfg_t cfg;
    pm_uart_stats_t stats;
    uint8_t big[200];
    size_t written = 0u;

    memset(big, 0x5A, sizeof(big));
    mock_reset();
    memset(&s_uart, 0, sizeof(s_uart));
    host_time_reset();
    make_cfg(&cfg);

    CHECK(pm_uart_init(&s_uart, &cfg) == PM_OK);

    /* Ring capacity is 63 bytes after reserving one slot. */
    CHECK(pm_uart_write(&s_uart, big, sizeof(big), &written,
                        PM_UART_TIMEOUT_NOW) == PM_ERR_AGAIN);
    CHECK(written == 63u);

    CHECK(pm_uart_get_stats(&s_uart, &stats) == PM_OK);
    CHECK(stats.tx_dropped == 200u - 63u);

    /* Blocking form: the mock never completes TX, so it must time out. */
    CHECK(pm_uart_write(&s_uart, big, sizeof(big), &written, 20u) == PM_ERR_TIMEOUT);
    CHECK(written == 0u);
}

#if defined(PM_ENABLE_SPI)
/* ------------------------------------------------------------------ spi */

static pm_spi_t s_spi;
static uint8_t  s_spi_tx[32];
static uint8_t  s_spi_rx[32];

static void make_spi_cfg(pm_spi_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->hw_id     = 1u;
    cfg->clock_hz  = 1000000u;
    cfg->mode      = PM_SPI_MODE_0;
    cfg->bit_order = PM_SPI_MSB_FIRST;
    cfg->nss       = PM_SPI_NSS_SOFT;
    cfg->use_dma   = false;
}

static void fill_spi_buffers(uint8_t len)
{
    uint8_t i;

    for (i = 0u; i < len; i++) {
        s_spi_tx[i] = (uint8_t)(0x10u + i);
        s_spi_rx[i] = 0u;
    }
}

static void test_spi_validation(void)
{
    pm_spi_cfg_t cfg;

    mock_reset();
    memset(&s_spi, 0, sizeof(s_spi));
    make_spi_cfg(&cfg);

    CHECK(pm_spi_init(NULL, &cfg) == PM_ERR_NULL);
    CHECK(pm_spi_init(&s_spi, NULL) == PM_ERR_NULL);

    cfg.clock_hz = 0u;
    CHECK(pm_spi_init(&s_spi, &cfg) == PM_ERR_PARAM);

    make_spi_cfg(&cfg);
    cfg.mode = (pm_spi_mode_t)7;
    CHECK(pm_spi_init(&s_spi, &cfg) == PM_ERR_PARAM);

    make_spi_cfg(&cfg);
    cfg.bit_order = (pm_spi_bit_order_t)3;
    CHECK(pm_spi_init(&s_spi, &cfg) == PM_ERR_PARAM);

    make_spi_cfg(&cfg);
    cfg.hw_id = 9u;
    CHECK(pm_spi_init(&s_spi, &cfg) == PM_ERR_RANGE);

    make_spi_cfg(&cfg);
    cfg.use_dma = true;
    CHECK(pm_spi_init(&s_spi, &cfg) == PM_OK);
    CHECK(mock_spi_is_open(1u) == 1);
    CHECK(mock_spi_uses_dma(1u) == 1);
    CHECK(pm_spi_clock_hz(&s_spi) == 1000000u);

    CHECK(pm_spi_init(&s_spi, &cfg) == PM_ERR_BUSY);

    CHECK(pm_spi_deinit(&s_spi) == PM_OK);
    CHECK(mock_spi_is_open(1u) == 0);
    CHECK(pm_spi_clock_hz(&s_spi) == 0u);
    CHECK(pm_spi_deinit(&s_spi) == PM_OK);       /* idempotent */
}

static void test_spi_async(void)
{
    pm_spi_cfg_t cfg;
    pm_spi_stats_t stats;
    pm_evt_t evt;
    size_t done = 99u;

    mock_reset();
    memset(&s_spi, 0, sizeof(s_spi));
    host_time_reset();
    make_spi_cfg(&cfg);

    CHECK(pm_spi_init(&s_spi, &cfg) == PM_OK);

    fill_spi_buffers(3u);
    CHECK(pm_spi_transfer(&s_spi, s_spi_tx, s_spi_rx, 3u, &done,
                          PM_SPI_TIMEOUT_NOW) == PM_ERR_AGAIN);
    CHECK(done == 0u);
    CHECK(mock_spi_capture_len(1u) == 3u);
    CHECK(memcmp(mock_spi_capture(1u), s_spi_tx, 3u) == 0);

    /* One transfer at a time. */
    CHECK(pm_spi_transfer(&s_spi, s_spi_tx, s_spi_rx, 3u, &done,
                          PM_SPI_TIMEOUT_NOW) == PM_ERR_BUSY);

    /* Nothing is "received" until the ISR completes the transfer. */
    CHECK(s_spi_rx[0] == 0u);

    mock_spi_complete(1u);
    CHECK(memcmp(s_spi_rx, s_spi_tx, 3u) == 0);

    CHECK(pm_spi_poll(&s_spi, &evt, NULL, NULL) == true);
    CHECK(evt.id == PM_EVT_SPI_DONE);
    CHECK(evt.src == 1u);
    CHECK(evt.arg == 3u);
    CHECK(pm_spi_poll(&s_spi, &evt, NULL, NULL) == false);

    CHECK(pm_spi_get_stats(&s_spi, &stats) == PM_OK);
    CHECK(stats.transfers == 1u);
    CHECK(stats.tx_bytes == 3u);
    CHECK(stats.rx_bytes == 3u);
    CHECK(stats.errors == 0u);

    /* The instance is reusable after completion. */
    CHECK(pm_spi_transfer(&s_spi, s_spi_tx, s_spi_rx, 2u, &done,
                          PM_SPI_TIMEOUT_NOW) == PM_ERR_AGAIN);
    mock_spi_complete(1u);
    CHECK(pm_spi_poll(&s_spi, &evt, NULL, NULL) == true);

    CHECK(pm_spi_transfer(&s_spi, s_spi_tx, s_spi_rx, 0u, &done,
                          PM_SPI_TIMEOUT_NOW) == PM_OK);
    CHECK(done == 0u);
    CHECK(pm_spi_transfer(&s_spi, NULL, s_spi_rx, 1u, &done, 0u) == PM_ERR_NULL);
    CHECK(pm_spi_transfer(&s_spi, s_spi_tx, NULL, 1u, &done, 0u) == PM_ERR_NULL);
    CHECK(pm_spi_transfer(&s_spi, s_spi_tx, s_spi_rx, 70000u, &done, 0u) == PM_ERR_PARAM);

    CHECK(pm_spi_deinit(&s_spi) == PM_OK);
}

static void test_spi_sync(void)
{
    pm_spi_cfg_t cfg;
    size_t done = 0u;

    mock_reset();
    memset(&s_spi, 0, sizeof(s_spi));
    host_time_reset();
    make_spi_cfg(&cfg);

    CHECK(pm_spi_init(&s_spi, &cfg) == PM_OK);

    /* The mock completes inside start(), like a very fast interrupt. */
    mock_spi_set_auto_complete(1u, 1);
    fill_spi_buffers(4u);
    CHECK(pm_spi_transfer(&s_spi, s_spi_tx, s_spi_rx, 4u, &done, 100u) == PM_OK);
    CHECK(done == 4u);
    CHECK(memcmp(s_spi_rx, s_spi_tx, 4u) == 0);

    CHECK(pm_spi_deinit(&s_spi) == PM_OK);
}

static void test_spi_timeout(void)
{
    pm_spi_cfg_t cfg;
    pm_spi_stats_t stats;
    size_t done = 0u;

    mock_reset();
    memset(&s_spi, 0, sizeof(s_spi));
    host_time_reset();
    make_spi_cfg(&cfg);

    CHECK(pm_spi_init(&s_spi, &cfg) == PM_OK);

    /* The mock never finishes; the model must abort and report progress. */
    mock_spi_set_progress(1u, 2u);
    fill_spi_buffers(8u);
    CHECK(pm_spi_transfer(&s_spi, s_spi_tx, s_spi_rx, 8u, &done, 5u) == PM_ERR_TIMEOUT);
    CHECK(done == 2u);

    CHECK(pm_spi_get_stats(&s_spi, &stats) == PM_OK);
    CHECK(stats.aborted == 1u);
    CHECK(stats.transfers == 0u);

    /* The object must be usable again after a timeout. */
    mock_spi_set_auto_complete(1u, 1);
    CHECK(pm_spi_transfer(&s_spi, s_spi_tx, s_spi_rx, 2u, &done, 100u) == PM_OK);
    CHECK(done == 2u);

    CHECK(pm_spi_deinit(&s_spi) == PM_OK);
}

static void test_spi_error(void)
{
    pm_spi_cfg_t cfg;
    pm_spi_stats_t stats;
    pm_evt_t evt;
    size_t done = 0u;

    mock_reset();
    memset(&s_spi, 0, sizeof(s_spi));
    host_time_reset();
    make_spi_cfg(&cfg);

    CHECK(pm_spi_init(&s_spi, &cfg) == PM_OK);

    mock_spi_set_fail(1u, 1);
    mock_spi_set_progress(1u, 1u);
    CHECK(pm_spi_transfer(&s_spi, s_spi_tx, s_spi_rx, 4u, &done, 100u) == PM_ERR_IO);
    CHECK(done == 1u);

    CHECK(pm_spi_poll(&s_spi, &evt, NULL, NULL) == true);
    CHECK(evt.id == PM_EVT_SPI_ERROR);
    CHECK(evt.arg == 0x0008u);

    CHECK(pm_spi_get_stats(&s_spi, &stats) == PM_OK);
    CHECK(stats.errors == 1u);
    CHECK(stats.last_error == 0x0008u);

    /* Recovery: the next transfer runs normally. */
    mock_spi_set_fail(1u, 0);
    mock_spi_set_auto_complete(1u, 1);
    CHECK(pm_spi_transfer(&s_spi, s_spi_tx, s_spi_rx, 2u, &done, 100u) == PM_OK);
    CHECK(done == 2u);

    CHECK(pm_spi_deinit(&s_spi) == PM_OK);
}

#endif /* PM_ENABLE_SPI */

#if defined(PM_ENABLE_I2C)
/* ------------------------------------------------------------------ i2c */

static pm_i2c_t s_i2c;
static uint8_t  s_i2c_wr[16];
static uint8_t  s_i2c_rd[16];

#define I2C_TEST_ADDR 0x70u   /* AHT20 */

static void make_i2c_cfg(pm_i2c_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->hw_id    = 1u;
    cfg->clock_hz = 100000u;
}

static void test_i2c_validation(void)
{
    pm_i2c_cfg_t cfg;

    mock_reset();
    memset(&s_i2c, 0, sizeof(s_i2c));
    make_i2c_cfg(&cfg);

    CHECK(pm_i2c_init(NULL, &cfg) == PM_ERR_NULL);
    CHECK(pm_i2c_init(&s_i2c, NULL) == PM_ERR_NULL);

    cfg.clock_hz = 9999u;
    CHECK(pm_i2c_init(&s_i2c, &cfg) == PM_ERR_PARAM);
    make_i2c_cfg(&cfg);
    cfg.clock_hz = 400001u;
    CHECK(pm_i2c_init(&s_i2c, &cfg) == PM_ERR_PARAM);

    make_i2c_cfg(&cfg);
    cfg.hw_id = 9u;
    CHECK(pm_i2c_init(&s_i2c, &cfg) == PM_ERR_RANGE);

    make_i2c_cfg(&cfg);
    CHECK(pm_i2c_init(&s_i2c, &cfg) == PM_OK);
    CHECK(mock_i2c_is_open(1u) == 1);
    CHECK(pm_i2c_clock_hz(&s_i2c) == 100000u);

    CHECK(pm_i2c_init(&s_i2c, &cfg) == PM_ERR_BUSY);

    /* Transfers reject bad arguments before touching the port. */
    CHECK(pm_i2c_write(NULL, I2C_TEST_ADDR, s_i2c_wr, 1u, NULL, 0u) == PM_ERR_NULL);
    CHECK(pm_i2c_write(&s_i2c, I2C_TEST_ADDR, NULL, 1u, NULL, 0u) == PM_ERR_NULL);
    CHECK(pm_i2c_write(&s_i2c, 0x80u, s_i2c_wr, 1u, NULL, 0u) == PM_ERR_PARAM);
    CHECK(pm_i2c_write(&s_i2c, I2C_TEST_ADDR, s_i2c_wr, 70000u, NULL, 0u) == PM_ERR_PARAM);
    CHECK(pm_i2c_read(&s_i2c, I2C_TEST_ADDR, NULL, 1u, NULL, 0u) == PM_ERR_NULL);
    CHECK(pm_i2c_write_read(&s_i2c, I2C_TEST_ADDR, s_i2c_wr, 0u, s_i2c_rd, 1u,
                            NULL, 0u) == PM_ERR_PARAM);

    CHECK(pm_i2c_deinit(&s_i2c) == PM_OK);
    CHECK(mock_i2c_is_open(1u) == 0);
    CHECK(pm_i2c_clock_hz(&s_i2c) == 0u);
    CHECK(pm_i2c_deinit(&s_i2c) == PM_OK);       /* idempotent */

    /* Not initialized -> state error, not a crash. */
    CHECK(pm_i2c_write(&s_i2c, I2C_TEST_ADDR, s_i2c_wr, 1u, NULL, 0u) == PM_ERR_STATE);
}

static void test_i2c_write_async(void)
{
    pm_i2c_cfg_t cfg;
    pm_i2c_stats_t stats;
    pm_evt_t evt;
    size_t done = 99u;

    mock_reset();
    memset(&s_i2c, 0, sizeof(s_i2c));
    host_time_reset();
    make_i2c_cfg(&cfg);

    CHECK(pm_i2c_init(&s_i2c, &cfg) == PM_OK);

    memset(s_i2c_wr, 0, sizeof(s_i2c_wr));
    s_i2c_wr[0] = 0xACu;
    s_i2c_wr[1] = 0x33u;

    CHECK(pm_i2c_write(&s_i2c, I2C_TEST_ADDR, s_i2c_wr, 2u, &done,
                       PM_I2C_TIMEOUT_NOW) == PM_ERR_AGAIN);
    CHECK(done == 0u);
    CHECK(mock_i2c_last_addr(1u) == (int)I2C_TEST_ADDR);
    CHECK(mock_i2c_capture_len(1u) == 2u);
    CHECK(memcmp(mock_i2c_capture(1u), s_i2c_wr, 2u) == 0);

    /* One transaction at a time. */
    CHECK(pm_i2c_write(&s_i2c, I2C_TEST_ADDR, s_i2c_wr, 2u, &done,
                       PM_I2C_TIMEOUT_NOW) == PM_ERR_BUSY);

    mock_i2c_complete(1u);

    CHECK(pm_i2c_poll(&s_i2c, &evt, NULL, NULL) == true);
    CHECK(evt.id == PM_EVT_I2C_DONE);
    CHECK(evt.src == 1u);
    CHECK(evt.arg == 2u);
    CHECK(pm_i2c_poll(&s_i2c, &evt, NULL, NULL) == false);

    CHECK(pm_i2c_get_stats(&s_i2c, &stats) == PM_OK);
    CHECK(stats.transfers == 1u);
    CHECK(stats.tx_bytes == 2u);
    CHECK(stats.rx_bytes == 0u);
    CHECK(stats.errors == 0u);

    CHECK(pm_i2c_deinit(&s_i2c) == PM_OK);
}

static void test_i2c_read_sync(void)
{
    pm_i2c_cfg_t cfg;
    pm_i2c_stats_t stats;
    size_t done = 0u;

    mock_reset();
    memset(&s_i2c, 0, sizeof(s_i2c));
    host_time_reset();
    make_i2c_cfg(&cfg);

    CHECK(pm_i2c_init(&s_i2c, &cfg) == PM_OK);

    memset(s_i2c_rd, 0, sizeof(s_i2c_rd));
    mock_i2c_set_auto_complete(1u, 1);
    CHECK(pm_i2c_read(&s_i2c, I2C_TEST_ADDR, s_i2c_rd, 3u, &done, 100u) == PM_OK);
    CHECK(done == 3u);
    CHECK(s_i2c_rd[0] == 0xC0u);
    CHECK(s_i2c_rd[1] == 0xC1u);
    CHECK(s_i2c_rd[2] == 0xC2u);

    CHECK(pm_i2c_get_stats(&s_i2c, &stats) == PM_OK);
    CHECK(stats.rx_bytes == 3u);

    CHECK(pm_i2c_deinit(&s_i2c) == PM_OK);
}

static void test_i2c_write_read(void)
{
    pm_i2c_cfg_t cfg;
    pm_i2c_stats_t stats;
    pm_evt_t evt;
    size_t done = 0u;

    mock_reset();
    memset(&s_i2c, 0, sizeof(s_i2c));
    host_time_reset();
    make_i2c_cfg(&cfg);

    CHECK(pm_i2c_init(&s_i2c, &cfg) == PM_OK);

    memset(s_i2c_rd, 0, sizeof(s_i2c_rd));
    s_i2c_wr[0] = 0x71u;                       /* AHT20 status register */

    CHECK(pm_i2c_write_read(&s_i2c, I2C_TEST_ADDR, s_i2c_wr, 1u, s_i2c_rd, 2u,
                            &done, PM_I2C_TIMEOUT_NOW) == PM_ERR_AGAIN);
    CHECK(mock_i2c_capture_len(1u) == 1u);
    CHECK(mock_i2c_capture(1u)[0] == 0x71u);

    mock_i2c_complete(1u);

    CHECK(pm_i2c_poll(&s_i2c, &evt, NULL, NULL) == true);
    CHECK(evt.id == PM_EVT_I2C_DONE);
    CHECK(evt.arg == 3u);                      /* 1 written + 2 read */

    CHECK(pm_i2c_get_stats(&s_i2c, &stats) == PM_OK);
    CHECK(stats.tx_bytes == 1u);
    CHECK(stats.rx_bytes == 2u);
    CHECK(s_i2c_rd[0] == 0xC0u);
    CHECK(s_i2c_rd[1] == 0xC1u);

    CHECK(pm_i2c_deinit(&s_i2c) == PM_OK);
}

static void test_i2c_timeout(void)
{
    pm_i2c_cfg_t cfg;
    pm_i2c_stats_t stats;
    size_t done = 0u;

    mock_reset();
    memset(&s_i2c, 0, sizeof(s_i2c));
    host_time_reset();
    make_i2c_cfg(&cfg);

    CHECK(pm_i2c_init(&s_i2c, &cfg) == PM_OK);

    /* The mock never completes; the model aborts and reports progress. */
    mock_i2c_set_progress(1u, 1u);
    CHECK(pm_i2c_write(&s_i2c, I2C_TEST_ADDR, s_i2c_wr, 8u, &done, 5u) == PM_ERR_TIMEOUT);
    CHECK(done == 1u);

    CHECK(pm_i2c_get_stats(&s_i2c, &stats) == PM_OK);
    CHECK(stats.aborted == 1u);
    CHECK(stats.transfers == 0u);

    /* Usable again after a timeout. */
    mock_i2c_set_auto_complete(1u, 1);
    CHECK(pm_i2c_write(&s_i2c, I2C_TEST_ADDR, s_i2c_wr, 2u, &done, 100u) == PM_OK);
    CHECK(done == 2u);

    CHECK(pm_i2c_deinit(&s_i2c) == PM_OK);
}

static void test_i2c_error(void)
{
    pm_i2c_cfg_t cfg;
    pm_i2c_stats_t stats;
    pm_evt_t evt;
    size_t done = 0u;

    mock_reset();
    memset(&s_i2c, 0, sizeof(s_i2c));
    host_time_reset();
    make_i2c_cfg(&cfg);

    CHECK(pm_i2c_init(&s_i2c, &cfg) == PM_OK);

    mock_i2c_set_fail(1u, 1);
    mock_i2c_set_progress(1u, 1u);
    CHECK(pm_i2c_write(&s_i2c, I2C_TEST_ADDR, s_i2c_wr, 4u, &done, 100u) == PM_ERR_IO);
    CHECK(done == 1u);

    CHECK(pm_i2c_poll(&s_i2c, &evt, NULL, NULL) == true);
    CHECK(evt.id == PM_EVT_I2C_ERROR);
    CHECK(evt.arg == 0x0004u);

    CHECK(pm_i2c_get_stats(&s_i2c, &stats) == PM_OK);
    CHECK(stats.errors == 1u);
    CHECK(stats.last_error == 0x0004u);

    /* Recovery: a device that answers again works. */
    mock_i2c_set_fail(1u, 0);
    mock_i2c_set_auto_complete(1u, 1);
    CHECK(pm_i2c_write(&s_i2c, I2C_TEST_ADDR, s_i2c_wr, 2u, &done, 100u) == PM_OK);
    CHECK(done == 2u);

    /* Optional classification: off by default, nothing moves. */
    {
        pm_i2c_err_counts_t ec;

        CHECK(pm_i2c_set_error_counting(NULL, true) == PM_ERR_NULL);
        CHECK(pm_i2c_get_error_counts(NULL, &ec) == PM_ERR_NULL);
        CHECK(pm_i2c_get_error_counts(&s_i2c, &ec) == PM_OK);
        CHECK((ec.af | ec.berr | ec.arlo | ec.ovr | ec.timeout | ec.other) == 0u);

        mock_i2c_set_auto_complete(1u, 0);
        mock_i2c_set_fail(1u, 1);
        mock_i2c_set_fail_mask(1u, PM_I2C_EERR_AF);
        CHECK(pm_i2c_write(&s_i2c, I2C_TEST_ADDR, s_i2c_wr, 4u, &done, 100u) ==
              PM_ERR_IO);                        /* disabled: historical code */
        CHECK(pm_i2c_get_error_counts(&s_i2c, &ec) == PM_OK);
        CHECK(ec.af == 0u);                      /* disabled: no counting */

        /* Enabled: AF -> NACK, BERR/ARLO/LOCAL -> BUS, timeout counted. */
        CHECK(pm_i2c_set_error_counting(&s_i2c, true) == PM_OK);
        CHECK(pm_i2c_write(&s_i2c, I2C_TEST_ADDR, s_i2c_wr, 4u, &done, 100u) ==
              PM_ERR_NACK);
        mock_i2c_set_fail_mask(1u, PM_I2C_EERR_BERR);
        CHECK(pm_i2c_write(&s_i2c, I2C_TEST_ADDR, s_i2c_wr, 4u, &done, 100u) ==
              PM_ERR_BUS);
        mock_i2c_set_fail_mask(1u, PM_I2C_EERR_ARLO);
        CHECK(pm_i2c_write(&s_i2c, I2C_TEST_ADDR, s_i2c_wr, 4u, &done, 100u) ==
              PM_ERR_BUS);
        mock_i2c_set_fail_mask(1u, PM_I2C_EERR_LOCAL);
        CHECK(pm_i2c_write(&s_i2c, I2C_TEST_ADDR, s_i2c_wr, 4u, &done, 100u) ==
              PM_ERR_BUS);
        CHECK(pm_i2c_get_error_counts(&s_i2c, &ec) == PM_OK);
        CHECK(ec.af == 1u);
        CHECK(ec.berr == 1u);
        CHECK(ec.arlo == 1u);
        CHECK(ec.other == 1u);

        /* Deadline expiry counts as a timeout and keeps PM_ERR_TIMEOUT. */
        mock_i2c_set_fail(1u, 0);
        CHECK(pm_i2c_write(&s_i2c, I2C_TEST_ADDR, s_i2c_wr, 8u, &done, 5u) ==
              PM_ERR_TIMEOUT);
        CHECK(pm_i2c_get_error_counts(&s_i2c, &ec) == PM_OK);
        CHECK(ec.timeout == 1u);

        mock_i2c_set_auto_complete(1u, 1);
        CHECK(pm_i2c_set_error_counting(&s_i2c, false) == PM_OK);
    }

    CHECK(pm_i2c_deinit(&s_i2c) == PM_OK);
    {
        pm_i2c_err_counts_t ec;

        CHECK(pm_i2c_get_error_counts(&s_i2c, &ec) == PM_ERR_STATE);
        CHECK(pm_i2c_set_error_counting(&s_i2c, true) == PM_ERR_STATE);
    }
}

#endif /* PM_ENABLE_I2C */

#if defined(PM_ENABLE_ADC)
/* ------------------------------------------------------------------ adc */

static pm_adc_t s_adc;

static void make_adc_cfg(pm_adc_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->hw_id       = 0u;
    cfg->channel     = 1u;
    cfg->sample_time = PM_ADC_SAMPLE_55CYCLES_5;
    cfg->vref_mv     = 3300u;
}

static void test_adc_validation(void)
{
    pm_adc_cfg_t cfg;
    uint16_t raw = 0u;

    mock_reset();
    memset(&s_adc, 0, sizeof(s_adc));
    make_adc_cfg(&cfg);

    CHECK(pm_adc_init(NULL, &cfg) == PM_ERR_NULL);
    CHECK(pm_adc_init(&s_adc, NULL) == PM_ERR_NULL);

    make_adc_cfg(&cfg);
    cfg.vref_mv = 999u;
    CHECK(pm_adc_init(&s_adc, &cfg) == PM_ERR_PARAM);
    cfg.vref_mv = 5001u;
    CHECK(pm_adc_init(&s_adc, &cfg) == PM_ERR_PARAM);
    make_adc_cfg(&cfg);
    cfg.sample_time = (pm_adc_sample_t)99;
    CHECK(pm_adc_init(&s_adc, &cfg) == PM_ERR_PARAM);

    make_adc_cfg(&cfg);
    cfg.hw_id = 5u;
    CHECK(pm_adc_init(&s_adc, &cfg) == PM_ERR_RANGE);
    make_adc_cfg(&cfg);
    cfg.channel = 9u;
    CHECK(pm_adc_init(&s_adc, &cfg) == PM_ERR_RANGE);

    make_adc_cfg(&cfg);
    CHECK(pm_adc_init(&s_adc, &cfg) == PM_OK);
    CHECK(mock_adc_is_open(0u) == 1);
    CHECK(mock_adc_last_channel(0u) == 1);

    CHECK(pm_adc_init(&s_adc, &cfg) == PM_ERR_BUSY);

    CHECK(pm_adc_read(NULL, &raw, 0u) == PM_ERR_NULL);
    CHECK(pm_adc_read(&s_adc, NULL, 0u) == PM_ERR_NULL);

    CHECK(pm_adc_deinit(&s_adc) == PM_OK);
    CHECK(mock_adc_is_open(0u) == 0);
    CHECK(pm_adc_deinit(&s_adc) == PM_OK);       /* idempotent */

    CHECK(pm_adc_read(&s_adc, &raw, 0u) == PM_ERR_STATE);

    /* mV conversion is a pure function of the configured reference. */
    CHECK(pm_adc_to_mv(NULL, 4095u) == 3300u);
}

static void test_adc_async(void)
{
    pm_adc_cfg_t cfg;
    pm_adc_stats_t stats;
    pm_evt_t evt;
    uint16_t raw = 0u;
    uint16_t mv = 0u;

    mock_reset();
    memset(&s_adc, 0, sizeof(s_adc));
    make_adc_cfg(&cfg);

    CHECK(pm_adc_init(&s_adc, &cfg) == PM_OK);
    CHECK(pm_adc_to_mv(&s_adc, 2048u) == 1650u);
    CHECK(pm_adc_to_mv(&s_adc, 0u) == 0u);

    mock_adc_set_value(2048u);
    CHECK(pm_adc_read(&s_adc, &raw, PM_ADC_TIMEOUT_NOW) == PM_ERR_AGAIN);
    CHECK(pm_adc_read(&s_adc, &raw, PM_ADC_TIMEOUT_NOW) == PM_ERR_BUSY);

    mock_adc_complete();

    CHECK(pm_adc_poll(&s_adc, &evt, NULL, NULL) == true);
    CHECK(evt.id == PM_EVT_ADC_DONE);
    CHECK(evt.arg == 2048u);
    CHECK(pm_adc_poll(&s_adc, &evt, NULL, NULL) == false);

    CHECK(pm_adc_get_stats(&s_adc, &stats) == PM_OK);
    CHECK(stats.conversions == 1u);
    CHECK(stats.last_value == 2048u);

    /* Synchronous read through the automatic completion path. */
    mock_adc_set_auto_complete(1);
    mock_adc_set_value(4095u);
    CHECK(pm_adc_read(&s_adc, &raw, 100u) == PM_OK);
    CHECK(raw == 4095u);
    CHECK(pm_adc_read_mv(&s_adc, &mv, 100u) == PM_OK);
    CHECK(mv == 3300u);

    CHECK(pm_adc_get_stats(&s_adc, &stats) == PM_OK);
    CHECK(stats.conversions == 3u);

    CHECK(pm_adc_deinit(&s_adc) == PM_OK);
    CHECK(mock_adc_is_open(0u) == 0);
}

static void test_adc_timeout(void)
{
    pm_adc_cfg_t cfg;
    pm_adc_stats_t stats;
    uint16_t raw = 0u;

    mock_reset();
    memset(&s_adc, 0, sizeof(s_adc));
    make_adc_cfg(&cfg);

    CHECK(pm_adc_init(&s_adc, &cfg) == PM_OK);

    CHECK(pm_adc_read(&s_adc, &raw, 5u) == PM_ERR_TIMEOUT);

    CHECK(pm_adc_get_stats(&s_adc, &stats) == PM_OK);
    CHECK(stats.aborted == 1u);
    CHECK(stats.conversions == 0u);

    /* Usable again after the timeout. */
    mock_adc_set_auto_complete(1);
    mock_adc_set_value(1234u);
    CHECK(pm_adc_read(&s_adc, &raw, 100u) == PM_OK);
    CHECK(raw == 1234u);

    CHECK(pm_adc_deinit(&s_adc) == PM_OK);
}

static void test_adc_error(void)
{
    pm_adc_cfg_t cfg;
    pm_adc_stats_t stats;
    pm_evt_t evt;
    uint16_t raw = 0u;

    mock_reset();
    memset(&s_adc, 0, sizeof(s_adc));
    make_adc_cfg(&cfg);

    CHECK(pm_adc_init(&s_adc, &cfg) == PM_OK);

    mock_adc_set_fail(1);
    CHECK(pm_adc_read(&s_adc, &raw, 100u) == PM_ERR_IO);

    CHECK(pm_adc_poll(&s_adc, &evt, NULL, NULL) == true);
    CHECK(evt.id == PM_EVT_ADC_ERROR);
    CHECK(evt.arg == 0x0040u);

    CHECK(pm_adc_get_stats(&s_adc, &stats) == PM_OK);
    CHECK(stats.errors == 1u);
    CHECK(stats.last_error == 0x0040u);

    /* Recovery: the next conversion completes normally. */
    mock_adc_set_fail(0);
    mock_adc_set_auto_complete(1);
    mock_adc_set_value(77u);
    CHECK(pm_adc_read(&s_adc, &raw, 100u) == PM_OK);
    CHECK(raw == 77u);

    CHECK(pm_adc_deinit(&s_adc) == PM_OK);
}

#endif /* PM_ENABLE_ADC */

#if defined(PM_ENABLE_PWM)
/* ------------------------------------------------------------------ pwm */

static pm_pwm_t s_pwm;

static void make_pwm_cfg(pm_pwm_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->hw_id         = 3u;      /* buzzer */
    cfg->freq_hz       = 2000u;
    cfg->duty_permille = 200u;    /* 20 % */
}

static void test_pwm_validation(void)
{
    pm_pwm_cfg_t cfg;

    mock_reset();
    memset(&s_pwm, 0, sizeof(s_pwm));
    make_pwm_cfg(&cfg);

    CHECK(pm_pwm_init(NULL, &cfg) == PM_ERR_NULL);
    CHECK(pm_pwm_init(&s_pwm, NULL) == PM_ERR_NULL);

    make_pwm_cfg(&cfg);
    cfg.freq_hz = 0u;
    CHECK(pm_pwm_init(&s_pwm, &cfg) == PM_ERR_PARAM);
    make_pwm_cfg(&cfg);
    cfg.freq_hz = PM_PWM_FREQ_MAX + 1u;
    CHECK(pm_pwm_init(&s_pwm, &cfg) == PM_ERR_PARAM);
    make_pwm_cfg(&cfg);
    cfg.duty_permille = 1001u;
    CHECK(pm_pwm_init(&s_pwm, &cfg) == PM_ERR_PARAM);
    make_pwm_cfg(&cfg);
    cfg.hw_id = MOCK_PWM_COUNT;
    CHECK(pm_pwm_init(&s_pwm, &cfg) == PM_ERR_RANGE);

    make_pwm_cfg(&cfg);
    CHECK(pm_pwm_init(&s_pwm, &cfg) == PM_OK);
    CHECK(mock_pwm_is_open(3u) == 1);
    CHECK(mock_pwm_frequency(3u) == 2000u);
    CHECK(mock_pwm_duty(3u) == 200u);
    CHECK(mock_pwm_is_enabled(3u) == 1);
    CHECK(pm_pwm_frequency_hz(&s_pwm) == 2000u);
    CHECK(pm_pwm_duty_permille(&s_pwm) == 200u);

    CHECK(pm_pwm_init(&s_pwm, &cfg) == PM_ERR_BUSY);

    CHECK(pm_pwm_deinit(&s_pwm) == PM_OK);
    CHECK(mock_pwm_is_open(3u) == 0);
    CHECK(pm_pwm_frequency_hz(&s_pwm) == 0u);
    CHECK(pm_pwm_duty_permille(&s_pwm) == 0u);
    CHECK(pm_pwm_deinit(&s_pwm) == PM_OK);       /* idempotent */

    CHECK(pm_pwm_set_duty(&s_pwm, 100u) == PM_ERR_STATE);
    CHECK(pm_pwm_set_frequency(&s_pwm, 1000u, NULL) == PM_ERR_STATE);
    CHECK(pm_pwm_set_pulse_us(&s_pwm, 1500u) == PM_ERR_STATE);
    CHECK(pm_pwm_enable(&s_pwm, true) == PM_ERR_STATE);
}

static void test_pwm_ops(void)
{
    pm_pwm_cfg_t cfg;
    uint32_t actual = 0u;

    mock_reset();
    memset(&s_pwm, 0, sizeof(s_pwm));
    make_pwm_cfg(&cfg);
    cfg.hw_id   = 2u;             /* servo */
    cfg.freq_hz = 50u;
    cfg.duty_permille = 75u;      /* 1.5 ms */

    CHECK(pm_pwm_init(&s_pwm, &cfg) == PM_OK);
    CHECK(pm_pwm_frequency_hz(&s_pwm) == 50u);
    CHECK(pm_pwm_duty_permille(&s_pwm) == 75u);

    CHECK(pm_pwm_set_duty(&s_pwm, 250u) == PM_OK);
    CHECK(mock_pwm_duty(2u) == 250u);
    CHECK(pm_pwm_duty_permille(&s_pwm) == 250u);
    CHECK(pm_pwm_set_duty(&s_pwm, 1001u) == PM_ERR_PARAM);

    CHECK(pm_pwm_set_frequency(&s_pwm, 100u, &actual) == PM_OK);
    CHECK(actual == 100u);
    CHECK(pm_pwm_frequency_hz(&s_pwm) == 100u);
    CHECK(mock_pwm_frequency(2u) == 100u);
    CHECK(pm_pwm_set_frequency(&s_pwm, 0u, &actual) == PM_ERR_PARAM);

    /* 1500 us at 100 Hz = 15 % = 150 permille. */
    CHECK(pm_pwm_set_pulse_us(&s_pwm, 1500u) == PM_OK);
    CHECK(pm_pwm_duty_permille(&s_pwm) == 150u);

    /* Longer than the period clamps to 1000. */
    CHECK(pm_pwm_set_pulse_us(&s_pwm, 20000u) == PM_OK);
    CHECK(pm_pwm_duty_permille(&s_pwm) == 1000u);

    CHECK(pm_pwm_enable(&s_pwm, false) == PM_OK);
    CHECK(mock_pwm_is_enabled(2u) == 0);
    CHECK(pm_pwm_enable(&s_pwm, true) == PM_OK);
    CHECK(mock_pwm_is_enabled(2u) == 1);

    CHECK(pm_pwm_deinit(&s_pwm) == PM_OK);
}

static void test_pwm_math(void)
{
    pm_pwm_timing_t t;

    CHECK(pm_pwm_timing_compute(72000000u, 1000u, NULL) == PM_ERR_NULL);
    CHECK(pm_pwm_timing_compute(0u, 1000u, &t) == PM_ERR_PARAM);
    CHECK(pm_pwm_timing_compute(72000000u, 0u, &t) == PM_ERR_PARAM);

    /* 50 Hz on 72 MHz: PSC 21 / ARR 65453, i.e. 22 * 65454 ticks. */
    CHECK(pm_pwm_timing_compute(72000000u, 50u, &t) == PM_OK);
    CHECK(t.psc == 21u);
    CHECK(t.arr == 65453u);
    CHECK(t.actual_hz == 50u);

    CHECK(pm_pwm_timing_compute(72000000u, 2000u, &t) == PM_OK);
    CHECK(t.psc == 0u);
    CHECK(t.arr == 35999u);
    CHECK(t.actual_hz == 2000u);

    CHECK(pm_pwm_timing_compute(72000000u, 1000000u, &t) == PM_OK);
    CHECK(t.psc == 0u);
    CHECK(t.arr == 71u);

    /* 100 Hz clock cannot produce 1 kHz: the self-check must refuse. */
    CHECK(pm_pwm_timing_compute(100u, 1000u, &t) == PM_ERR_PARAM);

    /* The board bug: PSC 0 + the servo ARR 19999 is 3.6 kHz, not 50 Hz.
       PSC 71 + ARR 19999 is the verified 50 Hz servo frame. */
    CHECK(pm_pwm_timing_matches(72000000u, 0u, 19999u, 50u, 10u) == false);
    CHECK(pm_pwm_timing_matches(72000000u, 71u, 19999u, 50u, 10u) == true);
    CHECK(pm_pwm_timing_matches(72000000u, 0u, 19999u, 3600u, 10u) == true);

    /* Boundary: each pair rounds to the requested integer frequency but is
     * not exact, so tolerance 0 must reject and 1 permille must accept.
     * (Round-first comparisons used to accept these at tolerance 0.) */
    CHECK(pm_pwm_timing_matches(72000000u, 407u, 391u, 450u, 0u) == false); /* 450.18 Hz */
    CHECK(pm_pwm_timing_matches(72000000u, 407u, 391u, 450u, 1u) == true);
    CHECK(pm_pwm_timing_matches(72000000u, 409u, 389u, 450u, 0u) == false); /* 450.28 Hz */
    CHECK(pm_pwm_timing_matches(72000000u, 409u, 389u, 450u, 1u) == true);
    CHECK(pm_pwm_timing_matches(72000000u, 389u, 368u, 500u, 0u) == false); /* 500.31 Hz */
    CHECK(pm_pwm_timing_matches(72000000u, 389u, 368u, 500u, 1u) == true);
    CHECK(pm_pwm_timing_matches(72000000u, 401u, 357u, 500u, 0u) == false); /* 500.29 Hz */
    CHECK(pm_pwm_timing_matches(72000000u, 401u, 357u, 500u, 1u) == true);

    CHECK(pm_pwm_compare_count(19999u, 75u) == 1500u);
    CHECK(pm_pwm_compare_count(19999u, 0u) == 0u);
    CHECK(pm_pwm_compare_count(19999u, 1000u) == 20000u);
    CHECK(pm_pwm_compare_count(65535u, 1000u) == 65535u);
}

#endif /* PM_ENABLE_PWM */

/* ------------------------------------------------------- timer arbitration */

static uint32_t s_probe_busy_id;
static int      s_probe_calls;

static bool timer_probe(void *ctx, uint8_t hw_id)
{
    PM_UNUSED(ctx);
    s_probe_calls++;
    return hw_id == (uint8_t)s_probe_busy_id;
}

static void test_timer_arbiter(void)
{
    pm_timer_claim_t pwm0;
    pm_timer_claim_t pwm1;
    pm_timer_claim_t other;

    pm_timer_reset();
    CHECK(pm_timer_claims(2u) == 0u);

    pwm0.hw_id    = 2u;             /* board numbering: 2 = TIM2 */
    pwm0.owner_id = 0u;
    pwm0.role     = PM_TIMER_ROLE_PWM;
    pwm0.psc      = 21u;
    pwm0.arr      = 65453u;
    pwm0.channels = 0x01u;

    CHECK(pm_timer_check(&pwm0) == PM_OK);
    CHECK(pm_timer_claim(&pwm0) == PM_OK);
    CHECK(pm_timer_claims(2u) == 1u);
    CHECK(pm_timer_claim(&pwm0) == PM_OK);          /* identical re-claim */
    CHECK(pm_timer_claims(2u) == 1u);

    /* Same period, different channel: allowed. */
    pwm1 = pwm0;
    pwm1.owner_id = 1u;
    pwm1.channels = 0x02u;
    CHECK(pm_timer_claim(&pwm1) == PM_OK);
    CHECK(pm_timer_claims(2u) == 2u);

    /* Same channel: conflict. */
    other = pwm1;
    other.channels = 0x01u;
    CHECK(pm_timer_claim(&other) == PM_ERR_CONFLICT);

    /* Same channel set, different ARR: conflict. */
    other = pwm1;
    other.channels = 0x04u;
    other.arr = 35999u;
    CHECK(pm_timer_claim(&other) == PM_ERR_CONFLICT);

    /* A tone on a timer that already carries PWM: conflict. */
    other = pwm1;
    other.owner_id = 7u;
    other.role = PM_TIMER_ROLE_TONE;
    other.channels = 0u;
    CHECK(pm_timer_claim(&other) == PM_ERR_CONFLICT);

    /* Free-running counter on another timer (3 = TIM3). */
    other.hw_id = 3u;
    other.owner_id = 5u;
    other.role = PM_TIMER_ROLE_COUNTER;
    other.psc = 0u;
    other.arr = PM_TIMER_ARR_FULL;
    other.channels = 0u;
    CHECK(pm_timer_claim(&other) == PM_OK);

    /* Counter with a partial ARR is not a counter. */
    {
        pm_timer_claim_t bad = other;

        bad.arr = 19999u;
        CHECK(pm_timer_claim(&bad) == PM_ERR_PARAM);
    }

    /* PWM cannot take a timer that is a free-running counter: this is the
       TIM2 echo/PWM collision from the board. */
    {
        pm_timer_claim_t pwmbad = pwm0;

        pwmbad.hw_id = 3u;
        pwmbad.owner_id = 6u;
        CHECK(pm_timer_claim(&pwmbad) == PM_ERR_CONFLICT);
    }

    /* Retiming updates every PWM claim on that timer. */
    CHECK(pm_timer_retime(2u, 1u, 31u, 64499u) == PM_OK);
    CHECK(pm_timer_retime(2u, 9u, 31u, 64499u) == PM_ERR_STATE);

    /* Release removes only that owner. */
    CHECK(pm_timer_release(2u, 0u) == PM_OK);
    CHECK(pm_timer_claims(2u) == 1u);
    CHECK(pm_timer_release(2u, 0u) == PM_ERR_STATE);

    /* Table capacity: 8 entries, the ninth distinct timer is refused. */
    pm_timer_reset();
    {
        uint8_t i;
        pm_timer_claim_t c;

        c.role     = PM_TIMER_ROLE_TONE;
        c.psc      = 719u;
        c.arr      = 49u;
        c.channels = 0u;
        for (i = 0u; i < PM_TIMER_CLAIM_MAX; i++) {
            c.hw_id = i;
            c.owner_id = 0u;
            CHECK(pm_timer_claim(&c) == PM_OK);
        }
        c.hw_id = 0u;
        c.owner_id = 1u;
        CHECK(pm_timer_claim(&c) == PM_ERR_CONFLICT);   /* timer taken */
        c.hw_id = 0u;
        c.role = PM_TIMER_ROLE_PWM;
        c.channels = 0x01u;
        c.psc = 0u;
        c.arr = 99u;
        CHECK(pm_timer_claim(&c) == PM_ERR_CONFLICT);
    }

    /* External occupancy: a timer started by code that never called this API
       (the board reads TIM2->CNT for echo while the motor runs) must be
       refused, not silently reconfigured. */
    pm_timer_reset();
    s_probe_busy_id = 2u;           /* TIM2 is the echo counter on the board */
    s_probe_calls   = 0;
    pm_timer_set_probe(timer_probe, NULL);
    {
        pm_timer_claim_t busy = pwm0;

        busy.hw_id    = 2u;
        busy.owner_id = 3u;
        CHECK(pm_timer_check(&busy) == PM_ERR_CONFLICT);   /* dry run sees it */
        CHECK(pm_timer_claim(&busy) == PM_ERR_CONFLICT);
        CHECK(pm_timer_claims(2u) == 0u);                  /* nothing recorded */

        /* Idle timer on the same run goes through (4 = TIM4). */
        busy.hw_id = 4u;
        CHECK(pm_timer_claim(&busy) == PM_OK);
        CHECK(pm_timer_claims(4u) == 1u);
    }

    /* Once the model owns the timer it is legitimately busy (the probe watches
       CR1.CEN). The probe must not fire again or siblings would be refused. */
    s_probe_busy_id = 4u;
    s_probe_calls   = 0;
    {
        pm_timer_claim_t sib = pwm0;

        sib.hw_id    = 4u;
        sib.owner_id = 6u;
        sib.channels = 0x02u;
        CHECK(pm_timer_claim(&sib) == PM_OK);
        CHECK(pm_timer_claims(4u) == 2u);
        CHECK(s_probe_calls == 0);
    }

    /* pm_timer_reset() uninstalls the probe as well (power-on state). */
    pm_timer_reset();
    s_probe_busy_id = 4u;
    {
        pm_timer_claim_t c = pwm0;

        c.hw_id    = 4u;
        c.owner_id = 3u;
        CHECK(pm_timer_claim(&c) == PM_OK);
    }
    pm_timer_reset();
}

#if defined(PM_ENABLE_TONE)
static void test_tone(void)
{
    pm_tone_t t;
    pm_tone_cfg_t cfg;

    mock_reset();
    memset(&t, 0, sizeof(t));

    cfg.hw_id = 0u;
    cfg.freq_hz = 2000u;
    cfg.duty_permille = 50u;

    CHECK(pm_tone_init(NULL, &cfg) == PM_ERR_NULL);
    CHECK(pm_tone_init(&t, NULL) == PM_ERR_NULL);

    cfg.freq_hz = PM_TONE_FREQ_MIN - 1u;
    CHECK(pm_tone_init(&t, &cfg) == PM_ERR_PARAM);
    cfg.freq_hz = PM_TONE_FREQ_MAX + 1u;
    CHECK(pm_tone_init(&t, &cfg) == PM_ERR_PARAM);
    cfg.freq_hz = 2000u;
    cfg.duty_permille = 0u;
    CHECK(pm_tone_init(&t, &cfg) == PM_ERR_PARAM);
    cfg.duty_permille = 1000u;
    CHECK(pm_tone_init(&t, &cfg) == PM_ERR_PARAM);
    cfg.duty_permille = 50u;
    cfg.hw_id = MOCK_TONE_COUNT;
    CHECK(pm_tone_init(&t, &cfg) == PM_ERR_RANGE);

    cfg.hw_id = 0u;
    CHECK(pm_tone_init(&t, &cfg) == PM_OK);
    CHECK(mock_tone_is_open(0u) == 1);
    CHECK(pm_tone_is_active(&t) == true);
    CHECK(pm_tone_frequency_hz(&t) == 2000u);
    CHECK(mock_tone_duty(0u) == 50u);
    CHECK(pm_tone_init(&t, &cfg) == PM_ERR_BUSY);

    CHECK(pm_tone_set(&t, 1000u, 100u) == PM_OK);
    CHECK(pm_tone_frequency_hz(&t) == 1000u);
    CHECK(mock_tone_frequency(0u) == 1000u);

    CHECK(pm_tone_off(&t) == PM_OK);
    CHECK(pm_tone_is_active(&t) == false);
    CHECK(mock_tone_is_active(0u) == 0);

    /* set() resumes the tone with the stored-facing values. */
    CHECK(pm_tone_set(&t, 440u, 500u) == PM_OK);
    CHECK(pm_tone_is_active(&t) == true);

    CHECK(pm_tone_deinit(&t) == PM_OK);
    CHECK(mock_tone_is_open(0u) == 0);
    CHECK(pm_tone_is_active(&t) == false);
    CHECK(pm_tone_deinit(&t) == PM_OK);
    CHECK(pm_tone_set(&t, 440u, 500u) == PM_ERR_STATE);
}
#endif /* PM_ENABLE_TONE */

#if defined(PM_ENABLE_TRACK)
static void test_track(void)
{
    pm_track_t t;
    pm_track_cfg_t cfg;
    bool on_line = false;

    mock_reset();
    memset(&t, 0, sizeof(t));

    cfg.hw_id = 0u;
    cfg.active_level = (uint8_t)PM_GPIO_LEVEL_LOW;

    CHECK(pm_track_init(NULL, &cfg) == PM_ERR_NULL);
    CHECK(pm_track_init(&t, NULL) == PM_ERR_NULL);
    cfg.active_level = 7u;
    CHECK(pm_track_init(&t, &cfg) == PM_ERR_PARAM);
    cfg.active_level = (uint8_t)PM_GPIO_LEVEL_LOW;
    cfg.hw_id = MOCK_TRACK_COUNT;
    CHECK(pm_track_init(&t, &cfg) == PM_ERR_RANGE);

    cfg.hw_id = 0u;
    CHECK(pm_track_init(&t, &cfg) == PM_OK);
    CHECK(mock_track_is_open(0u) == 1);
    CHECK(pm_track_init(&t, &cfg) == PM_ERR_BUSY);

    /* Active low: the pin low means "on the line". */
    mock_track_set_level(0u, PM_GPIO_LEVEL_HIGH);
    CHECK(pm_track_read_raw(&t, &on_line) == PM_OK);
    CHECK(on_line == false);
    mock_track_set_level(0u, PM_GPIO_LEVEL_LOW);
    CHECK(pm_track_read_raw(&t, &on_line) == PM_OK);
    CHECK(on_line == true);

    CHECK(pm_track_read_raw(&t, NULL) == PM_ERR_NULL);
    CHECK(pm_track_deinit(&t) == PM_OK);
    CHECK(mock_track_is_open(0u) == 0);
    CHECK(pm_track_deinit(&t) == PM_OK);
    CHECK(pm_track_read_raw(&t, &on_line) == PM_ERR_STATE);
}
#endif /* PM_ENABLE_TRACK */

#if defined(PM_ENABLE_ENCODER)
static void test_encoder(void)
{
    pm_encoder_t e;
    pm_encoder_cfg_t cfg;

    mock_reset();
    memset(&e, 0, sizeof(e));
    cfg.hw_id = 0u;
    cfg.invert = 0u;
    cfg.has_button = 0u;

    CHECK(pm_encoder_init(NULL, &cfg) == PM_ERR_NULL);
    CHECK(pm_encoder_init(&e, NULL) == PM_ERR_NULL);
    cfg.hw_id = MOCK_ENCODER_COUNT;
    CHECK(pm_encoder_init(&e, &cfg) == PM_ERR_RANGE);
    cfg.hw_id = 0u;

    /* Start from a non-zero state to prove the port's initial AB is adopted. */
    mock_encoder_set_initial_ab(0u, 3u);
    CHECK(pm_encoder_init(&e, &cfg) == PM_OK);
    CHECK(pm_encoder_last_ab(&e) == 3u);
    CHECK(pm_encoder_count(&e) == 0);
    CHECK(pm_encoder_init(&e, &cfg) == PM_ERR_BUSY);

    /* 11 -> 10 -> 00 -> 01 -> 11 is forward: +4. */
    pm_encoder_hw_edge(&e, 2u);
    pm_encoder_hw_edge(&e, 0u);
    pm_encoder_hw_edge(&e, 1u);
    pm_encoder_hw_edge(&e, 3u);
    CHECK(pm_encoder_count(&e) == 4);

    /* Reverse: 11 -> 01 -> 00 -> 10 -> 11 is -4. */
    pm_encoder_hw_edge(&e, 1u);
    pm_encoder_hw_edge(&e, 0u);
    pm_encoder_hw_edge(&e, 2u);
    pm_encoder_hw_edge(&e, 3u);
    CHECK(pm_encoder_count(&e) == 0);

    /* Diagonal jump (missed edge) does not count. */
    pm_encoder_hw_edge(&e, 0u);
    CHECK(pm_encoder_count(&e) == 0);
    CHECK(pm_encoder_last_ab(&e) == 0u);

    CHECK(pm_encoder_reset(&e) == PM_OK);
    CHECK(pm_encoder_count(&e) == 0);

    /* invert swaps A and B: AB 01 is decoded as 10, which is -1 from 00. */
    CHECK(pm_encoder_deinit(&e) == PM_OK);
    cfg.invert = 1u;
    mock_encoder_set_initial_ab(0u, 0u);
    CHECK(pm_encoder_init(&e, &cfg) == PM_OK);
    pm_encoder_hw_edge(&e, 1u);
    CHECK(pm_encoder_count(&e) == -1);
    CHECK(pm_encoder_last_ab(&e) == 2u);

    CHECK(pm_encoder_deinit(&e) == PM_OK);
    CHECK(pm_encoder_count(&e) == 0);
    CHECK(pm_encoder_reset(&e) == PM_ERR_STATE);

    /* Optional push button: absent by default, never an error. */
    {
        bool pressed = true;

        CHECK(pm_encoder_has_button(&e) == false);
        CHECK(pm_encoder_button(NULL, &pressed) == PM_ERR_NULL);
        CHECK(pm_encoder_button(&e, NULL) == PM_ERR_NULL);
        CHECK(pm_encoder_button(&e, &pressed) == PM_ERR_STATE);
        CHECK(pressed == true);          /* untouched while uninitialised */

        cfg.invert     = 0u;
        cfg.has_button = 0u;
        CHECK(pm_encoder_init(&e, &cfg) == PM_OK);
        CHECK(pm_encoder_has_button(&e) == false);
        CHECK(pm_encoder_button(&e, &pressed) == PM_ERR_DISABLED);
        CHECK(pressed == false);         /* a defined answer, not a fault */
        CHECK(pm_encoder_deinit(&e) == PM_OK);
        CHECK(pm_encoder_has_button(&e) == false);

        /* Configured: the state is really read from the port. */
        cfg.has_button = 1u;
        mock_encoder_set_button(0u, 1);
        CHECK(pm_encoder_init(&e, &cfg) == PM_OK);
        CHECK(pm_encoder_has_button(&e) == true);
        CHECK(pm_encoder_button(&e, &pressed) == PM_OK);
        CHECK(pressed == true);
        mock_encoder_set_button(0u, 0);
        CHECK(pm_encoder_button(&e, &pressed) == PM_OK);
        CHECK(pressed == false);
        CHECK(pm_encoder_deinit(&e) == PM_OK);
        CHECK(pm_encoder_has_button(&e) == false);
    }
}
#endif /* PM_ENABLE_ENCODER */

#if defined(PM_ENABLE_RELAY)
static void test_relay(void)
{
    pm_relay_t a;
    pm_relay_t b;
    pm_relay_cfg_t cfg;

    mock_reset();
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    CHECK(pm_relay_init(NULL, &cfg) == PM_ERR_NULL);
    CHECK(pm_relay_init(&a, NULL) == PM_ERR_NULL);

    /* A zero-initialised config carries UNSET: rejected, never defaulted. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.pin.id = 0u;
    CHECK(cfg.active_level == PM_RELAY_ACTIVE_UNSET);
    CHECK(pm_relay_init(&a, &cfg) == PM_ERR_PARAM);
    CHECK(mock_gpio_is_configured(0u) == 0);

    /* Unknown pin is a range error, not a silent no-op. */
    cfg.pin.id = MOCK_GPIO_COUNT;
    cfg.active_level = PM_RELAY_ACTIVE_HIGH;
    CHECK(pm_relay_init(&a, &cfg) == PM_ERR_RANGE);

    /* Active-high on pin 0: init must leave it de-energised (low). */
    cfg.pin.id = 0u;
    CHECK(pm_relay_init(&a, &cfg) == PM_OK);
    CHECK(mock_gpio_level(0u) == PM_GPIO_LEVEL_LOW);
    CHECK(pm_relay_is_on(&a) == false);
    CHECK(pm_relay_init(&a, &cfg) == PM_ERR_BUSY);

    CHECK(pm_relay_on(&a) == PM_OK);
    CHECK(mock_gpio_level(0u) == PM_GPIO_LEVEL_HIGH);
    CHECK(pm_relay_is_on(&a) == true);
    CHECK(pm_relay_toggle(&a) == PM_OK);
    CHECK(mock_gpio_level(0u) == PM_GPIO_LEVEL_LOW);
    CHECK(pm_relay_set(&a, true) == PM_OK);
    CHECK(pm_relay_is_on(&a) == true);
    CHECK(pm_relay_off(&a) == PM_OK);
    CHECK(mock_gpio_level(0u) == PM_GPIO_LEVEL_LOW);

    /* Active-low on pin 1: same API, inverted electrical level. */
    cfg.pin.id = 1u;
    cfg.active_level = PM_RELAY_ACTIVE_LOW;
    CHECK(pm_relay_init(&b, &cfg) == PM_OK);
    CHECK(mock_gpio_level(1u) == PM_GPIO_LEVEL_HIGH);   /* inactive = high */
    CHECK(pm_relay_on(&b) == PM_OK);
    CHECK(mock_gpio_level(1u) == PM_GPIO_LEVEL_LOW);    /* active = low */
    CHECK(pm_relay_is_on(&b) == true);

    /* Two instances, interleaved: each keeps its own state and pin. */
    CHECK(pm_relay_on(&a) == PM_OK);
    CHECK(mock_gpio_level(0u) == PM_GPIO_LEVEL_HIGH);
    CHECK(mock_gpio_level(1u) == PM_GPIO_LEVEL_LOW);    /* B untouched */
    CHECK(pm_relay_off(&b) == PM_OK);
    CHECK(mock_gpio_level(0u) == PM_GPIO_LEVEL_HIGH);   /* A untouched */
    CHECK(pm_relay_is_on(&b) == false);
    CHECK(pm_relay_is_on(&a) == true);

    /* Deinit is fail-safe: the load goes back to the inactive level. */
    CHECK(pm_relay_on(&b) == PM_OK);
    CHECK(pm_relay_deinit(&a) == PM_OK);
    CHECK(mock_gpio_level(0u) == PM_GPIO_LEVEL_LOW);
    CHECK(pm_relay_is_on(&a) == false);
    CHECK(pm_relay_deinit(&a) == PM_OK);                /* idempotent */
    CHECK(pm_relay_on(&a) == PM_ERR_STATE);             /* no longer usable */
    CHECK(pm_relay_is_on(&b) == true);                  /* B still alive */
}
#endif /* PM_ENABLE_RELAY */

#if defined(PM_ENABLE_MOTOR)
static void pwm_open(pm_pwm_t *p, uint8_t hw_id, uint32_t freq_hz)
{
    pm_pwm_cfg_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.hw_id = hw_id;
    cfg.freq_hz = freq_hz;
    cfg.duty_permille = 0u;
    CHECK(pm_pwm_init(p, &cfg) == PM_OK);
}

static void test_motor(void)
{
    pm_pwm_t a;
    pm_pwm_t b;
    pm_pwm_t c;
    pm_pwm_t d;
    pm_motor_t m1;
    pm_motor_t m2;
    pm_motor_t mu;
    pm_motor_cfg_t cfg;
    uint16_t coast_pair[2];

    mock_reset();
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&c, 0, sizeof(c));
    memset(&d, 0, sizeof(d));
    memset(&m1, 0, sizeof(m1));
    memset(&m2, 0, sizeof(m2));
    memset(&mu, 0, sizeof(mu));

    pwm_open(&a, 0u, 20000u);
    pwm_open(&b, 1u, 20000u);
    pwm_open(&c, 2u, 10000u);   /* deliberately different frequency */
    pwm_open(&d, 3u, 20000u);

    memset(&cfg, 0, sizeof(cfg));
    CHECK(pm_motor_init(NULL, &cfg) == PM_ERR_NULL);
    CHECK(pm_motor_init(&m1, NULL) == PM_ERR_NULL);

    cfg.in1 = &a;
    cfg.in2 = &b;

    /* UNSET (zero-initialised) means "no swap", i.e. the same as OFF. */
    CHECK(pm_motor_init(&mu, &cfg) == PM_OK);
    CHECK(pm_motor_set(&mu, 250u, PM_MOTOR_DIR_FORWARD) == PM_OK);
    CHECK(mock_pwm_duty(0u) == 250u);
    CHECK(mock_pwm_duty(1u) == 0u);
    CHECK(pm_motor_stop(&mu, PM_MOTOR_STOP_COAST) == PM_OK);

    /* Anything outside UNSET/OFF/ON is still a parameter error. */
    cfg.invert = (pm_motor_invert_t)3;
    CHECK(pm_motor_init(&m1, &cfg) == PM_ERR_PARAM);
    cfg.invert = PM_MOTOR_INVERT_OFF;

    cfg.in2 = &a;
    CHECK(pm_motor_init(&m1, &cfg) == PM_ERR_PARAM);    /* same channel twice */
    cfg.in2 = &b;

    /* Closed channel is a state error, not a silent coast. */
    {
        pm_pwm_t closed;

        memset(&closed, 0, sizeof(closed));
        cfg.in2 = &closed;
        CHECK(pm_motor_init(&m1, &cfg) == PM_ERR_STATE);
        cfg.in2 = &b;
    }

    /* Same timer, two periods: the TIM2 collision in miniature. */
    cfg.in1 = &c;
    CHECK(pm_motor_init(&m1, &cfg) == PM_ERR_CONFLICT);
    cfg.in1 = &a;

    CHECK(pm_motor_init(&m1, &cfg) == PM_OK);
    CHECK(pm_motor_state(&m1) == PM_MOTOR_STATE_STOP_COAST);
    CHECK(mock_pwm_duty(0u) == 0u);
    CHECK(mock_pwm_duty(1u) == 0u);
    CHECK(pm_motor_init(&m1, &cfg) == PM_ERR_BUSY);

    /* Forward: duty on IN1, IN2 held low. */
    CHECK(pm_motor_set(&m1, 500u, PM_MOTOR_DIR_FORWARD) == PM_OK);
    CHECK(mock_pwm_duty(0u) == 500u);
    CHECK(mock_pwm_duty(1u) == 0u);
    CHECK(pm_motor_state(&m1) == PM_MOTOR_STATE_RUN);
    CHECK(pm_motor_is_running(&m1) == true);

    /* Reverse really moves the duty onto the other input. */
    CHECK(pm_motor_set(&m1, 300u, PM_MOTOR_DIR_REVERSE) == PM_OK);
    CHECK(mock_pwm_duty(0u) == 0u);
    CHECK(mock_pwm_duty(1u) == 300u);
    CHECK(pm_motor_dir(&m1) == PM_MOTOR_DIR_REVERSE);
    CHECK(pm_motor_duty_permille(&m1) == 300u);

    /* duty 0 is a coast, whatever direction was asked for. */
    CHECK(pm_motor_set(&m1, 0u, PM_MOTOR_DIR_FORWARD) == PM_OK);
    CHECK(mock_pwm_duty(0u) == 0u);
    CHECK(mock_pwm_duty(1u) == 0u);
    CHECK(pm_motor_state(&m1) == PM_MOTOR_STATE_STOP_COAST);
    CHECK(pm_motor_is_running(&m1) == false);

    /* Brake: both inputs at 1000 permille, no drive reported. */
    CHECK(pm_motor_stop(&m1, PM_MOTOR_STOP_BRAKE) == PM_OK);
    CHECK(mock_pwm_duty(0u) == 1000u);
    CHECK(mock_pwm_duty(1u) == 1000u);
    CHECK(pm_motor_state(&m1) == PM_MOTOR_STATE_STOP_BRAKE);
    CHECK(pm_motor_is_running(&m1) == false);
    CHECK(pm_motor_duty_permille(&m1) == 0u);

    /* Leaving brake is immediate: no duty left sitting at 1000. */
    CHECK(pm_motor_set(&m1, 600u, PM_MOTOR_DIR_FORWARD) == PM_OK);
    CHECK(mock_pwm_duty(0u) == 600u);
    CHECK(mock_pwm_duty(1u) == 0u);
    CHECK(pm_motor_state(&m1) == PM_MOTOR_STATE_RUN);

    /* Equivalence: set(0) and stop(COAST) produce the same electrical
     * state; both differ from brake. */
    CHECK(pm_motor_set(&m1, 0u, PM_MOTOR_DIR_REVERSE) == PM_OK);
    coast_pair[0] = mock_pwm_duty(0u);
    coast_pair[1] = mock_pwm_duty(1u);
    CHECK(pm_motor_stop(&m1, PM_MOTOR_STOP_COAST) == PM_OK);
    CHECK(mock_pwm_duty(0u) == coast_pair[0]);
    CHECK(mock_pwm_duty(1u) == coast_pair[1]);
    CHECK(coast_pair[0] == 0u);
    CHECK(coast_pair[1] == 0u);
    CHECK(pm_motor_stop(&m1, PM_MOTOR_STOP_BRAKE) == PM_OK);
    CHECK(mock_pwm_duty(0u) != coast_pair[0]);
    CHECK(mock_pwm_duty(1u) != coast_pair[1]);
    CHECK(pm_motor_stop(&m1, PM_MOTOR_STOP_COAST) == PM_OK);

    /* Second instance: invert swaps which input carries the duty. */
    CHECK(pm_pwm_deinit(&c) == PM_OK);
    pwm_open(&c, 2u, 20000u);
    cfg.in1 = &c;
    cfg.in2 = &d;
    cfg.invert = PM_MOTOR_INVERT_ON;
    CHECK(pm_motor_init(&m2, &cfg) == PM_OK);

    CHECK(pm_motor_set(&m1, 600u, PM_MOTOR_DIR_FORWARD) == PM_OK);
    CHECK(pm_motor_set(&m2, 400u, PM_MOTOR_DIR_FORWARD) == PM_OK);
    CHECK(mock_pwm_duty(0u) == 600u);   /* m1: IN1 */
    CHECK(mock_pwm_duty(1u) == 0u);
    CHECK(mock_pwm_duty(2u) == 0u);     /* m2: inverted -> IN2 */
    CHECK(mock_pwm_duty(3u) == 400u);

    /* Cross operations: neither instance disturbs the other. */
    CHECK(pm_motor_stop(&m1, PM_MOTOR_STOP_BRAKE) == PM_OK);
    CHECK(mock_pwm_duty(2u) == 0u);
    CHECK(mock_pwm_duty(3u) == 400u);
    CHECK(pm_motor_state(&m2) == PM_MOTOR_STATE_RUN);
    CHECK(pm_motor_stop(&m2, PM_MOTOR_STOP_COAST) == PM_OK);
    CHECK(mock_pwm_duty(0u) == 1000u);
    CHECK(mock_pwm_duty(1u) == 1000u);
    CHECK(pm_motor_state(&m1) == PM_MOTOR_STATE_STOP_BRAKE);
    CHECK(pm_motor_is_running(&m2) == false);

    CHECK(pm_motor_deinit(&m1) == PM_OK);
    CHECK(mock_pwm_duty(0u) == 0u);
    CHECK(mock_pwm_duty(1u) == 0u);
    CHECK(pm_motor_deinit(&m1) == PM_OK);               /* idempotent */
    CHECK(pm_motor_set(&m1, 100u, PM_MOTOR_DIR_FORWARD) == PM_ERR_STATE);
}
#endif /* PM_ENABLE_MOTOR */

#if defined(PM_ENABLE_SERVO)
static void test_servo(void)
{
    pm_servo_t      s;
    pm_servo_cfg_t  cfg;
    pm_servo_range_t r;
    uint16_t        pulse = 0u;
    uint16_t        angle = 0u;

    mock_reset();
    memset(&s, 0, sizeof(s));
    memset(&cfg, 0, sizeof(cfg));

    /* ---- pure angle -> pulse mapping ---- */
    r.angle_min_deg  = PM_SERVO_SG90_ANGLE_MIN;
    r.angle_max_deg  = PM_SERVO_SG90_ANGLE_MAX;
    r.pulse_min_us   = PM_SERVO_SG90_PULSE_MIN_US;
    r.pulse_max_us   = PM_SERVO_SG90_PULSE_MAX_US;

    CHECK(pm_servo_pulse_from_angle(NULL, 90u, &pulse) == PM_ERR_NULL);
    CHECK(pm_servo_pulse_from_angle(&r, 90u, NULL) == PM_ERR_NULL);
    CHECK(pm_servo_pulse_from_angle(&r, 0u, &pulse) == PM_OK);
    CHECK(pulse == 500u);
    CHECK(pm_servo_pulse_from_angle(&r, 90u, &pulse) == PM_OK);
    CHECK(pulse == 1500u);                 /* centre must be exact */
    CHECK(pm_servo_pulse_from_angle(&r, 180u, &pulse) == PM_OK);
    CHECK(pulse == 2500u);
    CHECK(pm_servo_pulse_from_angle(&r, 181u, &pulse) == PM_ERR_RANGE);

    /* Degenerate ranges are caller bugs, never defaulted. */
    {
        pm_servo_range_t bad = r;

        bad.angle_max_deg = bad.angle_min_deg;
        CHECK(pm_servo_pulse_from_angle(&bad, 0u, &pulse) == PM_ERR_PARAM);
        bad = r;
        bad.pulse_min_us = 0u;
        CHECK(pm_servo_pulse_from_angle(&bad, 90u, &pulse) == PM_ERR_PARAM);
        bad = r;
        bad.pulse_max_us = bad.pulse_min_us;
        CHECK(pm_servo_pulse_from_angle(&bad, 90u, &pulse) == PM_ERR_PARAM);
    }

    /* A different servo means different numbers, same logic. */
    {
        pm_servo_range_t mg = r;

        mg.angle_min_deg = 10u;
        mg.angle_max_deg = 170u;
        mg.pulse_min_us  = 600u;
        mg.pulse_max_us  = 2400u;
        CHECK(pm_servo_pulse_from_angle(&mg, 10u, &pulse) == PM_OK);
        CHECK(pulse == 600u);
        CHECK(pm_servo_pulse_from_angle(&mg, 90u, &pulse) == PM_OK);
        CHECK(pulse == 1500u);
        CHECK(pm_servo_pulse_from_angle(&mg, 170u, &pulse) == PM_OK);
        CHECK(pulse == 2400u);
        CHECK(pm_servo_pulse_from_angle(&mg, 9u, &pulse) == PM_ERR_RANGE);
    }

    /* ---- device model ---- */
    CHECK(pm_servo_init(NULL, &cfg) == PM_ERR_NULL);
    CHECK(pm_servo_init(&s, NULL) == PM_ERR_NULL);

    cfg.hw_id           = 0u;
    cfg.freq_hz         = 50u;
    cfg.range           = r;
    cfg.start_angle_deg = 200u;
    CHECK(pm_servo_init(&s, &cfg) == PM_ERR_RANGE);   /* start angle outside */
    CHECK(mock_pwm_is_open(0u) == 0);

    cfg.start_angle_deg = 90u;
    cfg.freq_hz         = 0u;
    CHECK(pm_servo_init(&s, &cfg) == PM_ERR_PARAM);   /* no frame rate */
    cfg.freq_hz = 1000u;
    CHECK(pm_servo_init(&s, &cfg) == PM_ERR_RANGE);   /* 1 ms frame < 2.5 ms */
    cfg.freq_hz = 50u;

    cfg.hw_id = MOCK_PWM_COUNT;
    CHECK(pm_servo_init(&s, &cfg) == PM_ERR_RANGE);   /* unknown channel */
    cfg.hw_id = 0u;

    CHECK(pm_servo_init(&s, &cfg) == PM_OK);
    CHECK(mock_pwm_is_open(0u) == 1);
    CHECK(mock_pwm_frequency(0u) == 50u);
    CHECK(mock_pwm_duty(0u) == 75u);                  /* 1500 us of 20 ms */
    CHECK(pm_servo_get_angle(&s, &angle) == PM_OK);
    CHECK(angle == 90u);
    CHECK(pm_servo_init(&s, &cfg) == PM_ERR_BUSY);

    CHECK(pm_servo_set_angle(&s, 0u) == PM_OK);
    CHECK(mock_pwm_duty(0u) == 25u);
    CHECK(pm_servo_set_angle(&s, 180u) == PM_OK);
    CHECK(mock_pwm_duty(0u) == 125u);

    /* Out of range keeps the previous position untouched. */
    CHECK(pm_servo_set_angle(&s, 181u) == PM_ERR_RANGE);
    CHECK(mock_pwm_duty(0u) == 125u);
    CHECK(pm_servo_get_angle(&s, &angle) == PM_OK);
    CHECK(angle == 180u);

    /* A different frame rate is just a parameter: 333 Hz -> 3.003 ms frame. */
    {
        pm_servo_t fast;
        pm_servo_cfg_t fcfg = cfg;

        memset(&fast, 0, sizeof(fast));
        fcfg.hw_id   = 2u;
        fcfg.freq_hz = 333u;
        CHECK(pm_servo_init(&fast, &fcfg) == PM_OK);
        CHECK(mock_pwm_frequency(2u) == 333u);
        CHECK(mock_pwm_duty(2u) == 500u);             /* 1500 us / 3003 us */
        CHECK(pm_servo_set_angle(&fast, 0u) == PM_OK);
        CHECK(mock_pwm_duty(2u) == 167u);             /* 500 us / 3003 us */
        CHECK(pm_servo_deinit(&fast) == PM_OK);
        CHECK(mock_pwm_is_open(2u) == 0);
    }

    /* A channel already owned elsewhere is reported, not stolen. */
    {
        pm_pwm_t       other;
        pm_pwm_cfg_t   ocfg;
        pm_servo_t     busy;
        pm_servo_cfg_t bcfg = cfg;

        memset(&other, 0, sizeof(other));
        memset(&ocfg, 0, sizeof(ocfg));
        memset(&busy, 0, sizeof(busy));
        ocfg.hw_id   = 3u;
        ocfg.freq_hz = 50u;
        CHECK(pm_pwm_init(&other, &ocfg) == PM_OK);
        bcfg.hw_id = 3u;
        CHECK(pm_servo_init(&busy, &bcfg) == PM_ERR_BUSY);
        CHECK(pm_pwm_deinit(&other) == PM_OK);
    }

    CHECK(pm_servo_deinit(&s) == PM_OK);
    CHECK(mock_pwm_is_open(0u) == 0);
    CHECK(pm_servo_get_angle(&s, &angle) == PM_ERR_STATE);
    CHECK(pm_servo_set_angle(&s, 90u) == PM_ERR_STATE);
    CHECK(pm_servo_deinit(&s) == PM_ERR_STATE);
}
#endif /* PM_ENABLE_SERVO */

int main(void)
{
    test_ringbuf();
    test_evtq();
    test_gpio();
    test_uart_validation();
    test_uart_tx();
    test_uart_rx();
    test_uart_timeout();
#if defined(PM_ENABLE_SPI)
    test_spi_validation();
    test_spi_async();
    test_spi_sync();
    test_spi_timeout();
    test_spi_error();
#endif
#if defined(PM_ENABLE_I2C)
    test_i2c_validation();
    test_i2c_write_async();
    test_i2c_read_sync();
    test_i2c_write_read();
    test_i2c_timeout();
    test_i2c_error();
#endif
#if defined(PM_ENABLE_ADC)
    test_adc_validation();
    test_adc_async();
    test_adc_timeout();
    test_adc_error();
#endif
#if defined(PM_ENABLE_PWM)
    test_pwm_validation();
    test_pwm_ops();
    test_pwm_math();
#endif
    test_timer_arbiter();
#if defined(PM_ENABLE_TONE)
    test_tone();
#endif
#if defined(PM_ENABLE_TRACK)
    test_track();
#endif
#if defined(PM_ENABLE_ENCODER)
    test_encoder();
#endif
#if defined(PM_ENABLE_RELAY)
    test_relay();
#endif
#if defined(PM_ENABLE_MOTOR)
    test_motor();
#endif
#if defined(PM_ENABLE_SERVO)
    test_servo();
#endif

    printf("\n%d checks, %d failure(s)\n", s_checks, s_failures);
    return (s_failures == 0) ? 0 : 1;
}
