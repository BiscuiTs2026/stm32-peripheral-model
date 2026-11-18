/*
 * main.c - entry glue for the STM32F103C8T6 demo.
 *
 * This file only sequences initialization and schedules work. It contains no
 * driver code and no register access; everything goes through pm_* APIs.
 *
 * Demo behaviour:
 *   - blue LED toggles every 500 ms
 *   - USART2 (115200 8N1) echoes received bytes
 *   - "hb\r\n" is sent once per second
 */
#include "pm_common.h"
#include "pm_osal.h"
#include "pm_gpio.h"
#include "pm_uart.h"
#include "pm_platform.h"
#include "board_f103c8.h"

static pm_uart_t s_uart;
static uint8_t   s_rx_storage[256];
static uint8_t   s_tx_storage[512];

static uint32_t s_uart_errors;

static const pm_gpio_cfg_t s_led_cfg = {
    .dir           = PM_GPIO_DIR_OUT,
    .pull          = PM_GPIO_PULL_NONE,
    .speed         = PM_GPIO_SPEED_LOW,
    .initial_level = PM_GPIO_LEVEL_LOW,
};

static const pm_uart_cfg_t s_uart_cfg = {
    .hw_id      = BOARD_UART_DEBUG,
    .baudrate   = 115200u,
    .parity     = PM_UART_PARITY_NONE,
    .stop_bits  = PM_UART_STOP_1,
    .data_bits  = 8u,
    .rx_storage = s_rx_storage,
    .rx_size    = (uint16_t)sizeof(s_rx_storage),
    .tx_storage = s_tx_storage,
    .tx_size    = (uint16_t)sizeof(s_tx_storage),
};

static void on_uart_event(pm_uart_t *u, const pm_evt_t *evt, void *ctx)
{
    PM_UNUSED(u);
    PM_UNUSED(ctx);

    if (evt->id == PM_EVT_UART_ERROR) {
        s_uart_errors++;
    }
}

static void uart_service(void)
{
    uint8_t buf[32];
    size_t  got = 0u;

    while (pm_uart_read(&s_uart, buf, sizeof(buf), &got) == PM_OK) {
        (void)pm_uart_write(&s_uart, buf, got, NULL, PM_UART_TIMEOUT_NOW);
    }

    while (pm_uart_poll(&s_uart, NULL, on_uart_event, NULL)) {
        /* drain one event per call */
    }
}

static void error_loop(void)
{
    pm_gpio_pin_t red = { .id = BOARD_GPIO_LED_RED };

    for (;;) {
        (void)pm_gpio_toggle(red);
        pm_osal_delay_ms(100u);
    }
}

int main(void)
{
    pm_gpio_pin_t led_blue = { .id = BOARD_GPIO_LED_BLUE };
    pm_gpio_pin_t led_red  = { .id = BOARD_GPIO_LED_RED };
    uint32_t next_led_ms;
    uint32_t next_hb_ms;

    pm_platform_init();

    if (pm_gpio_configure(led_blue, &s_led_cfg) != PM_OK) {
        error_loop();
    }
    if (pm_gpio_configure(led_red, &s_led_cfg) != PM_OK) {
        error_loop();
    }
    if (pm_uart_init(&s_uart, &s_uart_cfg) != PM_OK) {
        error_loop();
    }

    next_led_ms = pm_osal_now_ms();
    next_hb_ms  = next_led_ms;

    for (;;) {
        uint32_t now = pm_osal_now_ms();

        uart_service();

        if ((uint32_t)(now - next_led_ms) >= 500u) {
            next_led_ms += 500u;
            (void)pm_gpio_toggle(led_blue);
        }
        if ((uint32_t)(now - next_hb_ms) >= 1000u) {
            static const uint8_t hb[] = "hb\r\n";
            next_hb_ms += 1000u;
            (void)pm_uart_write(&s_uart, hb, sizeof(hb) - 1u, NULL,
                                PM_UART_TIMEOUT_NOW);
        }
    }
}
