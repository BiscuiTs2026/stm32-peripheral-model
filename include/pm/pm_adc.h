/*
 * pm_adc.h - single-channel, single-shot ADC model API.
 *
 * Lifecycle:  init -> (read / poll)* -> deinit
 *
 * Conversion model:
 *  - One channel per object, one conversion per call, software trigger.
 *    There is no scan sequencer, no continuous mode and no queue in this
 *    release.
 *  - The raw value is the right-aligned 12-bit result (0..4095).
 *  - `vref_mv` in the config is used only by pm_adc_to_mv(); the port does
 *    not measure the reference.
 *
 * ISR-to-main-loop path (STM32F1 port):
 *   ADC1_2_IRQHandler -> HAL_ADC_IRQHandler -> HAL_ADC_ConvCpltCallback
 *     -> pm_adc_hw_done()   (stores the sample, clears `active`, pushes
 *        ADC_DONE with the raw value in `arg`)
 *   HAL_ADC_ErrorCallback -> pm_adc_hw_error() -> ADC_ERROR event.
 *   The ISR never calls user code. pm_adc_poll() runs callbacks in the main
 *   loop only.
 *
 * Timeout:
 *  - timeout_ms == 0: start and return. PM_OK if the sample is already
 *    available, PM_ERR_AGAIN while the conversion is still running. The
 *    result is then retrieved through the ADC_DONE event (arg = raw value)
 *    or a later pm_adc_read().
 *  - timeout_ms > 0: wait up to timeout_ms. On expiry the port stops the
 *    conversion and PM_ERR_TIMEOUT is returned.
 *
 * Errors:
 *  - A conversion that ends through the HAL error callback returns PM_ERR_IO
 *    and pushes ADC_ERROR whose arg is the HAL error mask.
 */
#ifndef PM_ADC_H
#define PM_ADC_H

#include "pm_common.h"
#include "pm_err.h"
#include "pm_port.h"
#include "pm_event.h"

#define PM_ADC_TIMEOUT_NOW 0u
#define PM_ADC_MAX_RAW     4095u

struct pm_adc {
    uint32_t magic;          /* set by pm_adc_init, cleared by deinit */
    uint8_t  state;
    uint8_t  hw_id;
    uint16_t vref_mv;

    volatile uint8_t  active;      /* 1 while a conversion is in flight */
    volatile uint8_t  last_error;  /* 1 when the last conversion failed */
    volatile uint16_t last_value;  /* raw sample of the last conversion */

    pm_evtq_t            evtq;
    const pm_adc_ops_t  *ops;
    void                *port_ctx;
    pm_adc_stats_t       stats;
};

typedef void (*pm_adc_evt_cb_t)(pm_adc_t *a, const pm_evt_t *evt, void *user_ctx);

/* Initializes the object and opens the hardware instance described by cfg.
 * The object must be zero-initialized. Returns PM_ERR_PARAM for an invalid
 * sample time or voltage reference, PM_ERR_RANGE for an unknown hw_id or
 * channel, PM_ERR_BUSY when the object or the instance is already open. */
pm_err_t pm_adc_init(pm_adc_t *a, const pm_adc_cfg_t *cfg);

/* Stops an in-flight conversion, then releases the instance. Idempotent. */
pm_err_t pm_adc_deinit(pm_adc_t *a);

/* One conversion; see the timeout rules above. `raw` is written only on
 * PM_OK. */
pm_err_t pm_adc_read(pm_adc_t *a, uint16_t *raw, uint32_t timeout_ms);

/* Same as pm_adc_read, then converted with pm_adc_to_mv(). `mv` is written
 * only on PM_OK. */
pm_err_t pm_adc_read_mv(pm_adc_t *a, uint16_t *mv, uint32_t timeout_ms);

/* raw * vref_mv / 4095, rounded. Uses the configured reference; falls back to
 * 3300 mV for an invalid object so the result stays defined. */
uint16_t pm_adc_to_mv(const pm_adc_t *a, uint16_t raw);

/* Pops one pending event (may be NULL) and optionally invokes `cb` with it in
 * main-loop context. Returns false when the queue is empty. */
bool pm_adc_poll(pm_adc_t *a, pm_evt_t *evt, pm_adc_evt_cb_t cb, void *user_ctx);

pm_err_t pm_adc_get_stats(const pm_adc_t *a, pm_adc_stats_t *out);

/* -------------------------------------------------- ISR-side internal hooks
 * Called only by the port backend, from interrupt context. They never call
 * user code and never block. */
void pm_adc_hw_done(pm_adc_t *a, uint16_t raw);
void pm_adc_hw_error(pm_adc_t *a, uint16_t hw_error_mask);

#endif /* PM_ADC_H */
