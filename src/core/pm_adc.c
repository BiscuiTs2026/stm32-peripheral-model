/*
 * pm_adc.c - single-shot ADC model, independent of any vendor header.
 *
 * The model owns the conversion lifecycle, the deadline and the event queue.
 * The port owns the wires: it starts one software-triggered conversion and
 * turns the end-of-conversion interrupt into pm_adc_hw_done().
 */
#include "pm_adc.h"
#include "pm_osal.h"
#include <string.h>

#define PM_ADC_STATE_UNINIT 0u
#define PM_ADC_STATE_READY  1u

#define PM_ADC_MAGIC        0x41444D31u   /* "ADM1" */

static bool cfg_valid(const pm_adc_cfg_t *cfg)
{
    return (cfg->sample_time <= PM_ADC_SAMPLE_239CYCLES_5) &&
           (cfg->vref_mv >= 1000u) &&
           (cfg->vref_mv <= 5000u);
}

static pm_err_t begin_conversion(pm_adc_t *a)
{
    pm_irq_state_t irq = pm_osal_critical_enter();
    pm_err_t rc = PM_OK;

    if (a->active != 0u) {
        rc = PM_ERR_BUSY;             /* one conversion at a time */
    } else {
        a->active     = 1u;
        a->last_error = 0u;
    }

    pm_osal_critical_exit(irq);
    return rc;
}

/* Clears `active` after a start failure, without touching the counters. */
static void clear_active(pm_adc_t *a)
{
    pm_irq_state_t irq = pm_osal_critical_enter();

    a->active = 0u;

    pm_osal_critical_exit(irq);
}

pm_err_t pm_adc_init(pm_adc_t *a, const pm_adc_cfg_t *cfg)
{
    const pm_port_t *port;
    pm_err_t rc;

    if ((a == NULL) || (cfg == NULL)) {
        return PM_ERR_NULL;
    }
    if (!cfg_valid(cfg)) {
        return PM_ERR_PARAM;
    }
    if ((a->magic == PM_ADC_MAGIC) && (a->state == PM_ADC_STATE_READY)) {
        return PM_ERR_BUSY;
    }

    port = pm_port_get();
    if ((port == NULL) || (port->adc == NULL)) {
        return PM_ERR_STATE;
    }
    if (cfg->hw_id >= port->adc_count) {
        return PM_ERR_RANGE;
    }

    (void)memset(a, 0, sizeof(*a));
    a->hw_id    = cfg->hw_id;
    a->vref_mv  = cfg->vref_mv;
    a->ops      = port->adc;
    a->port_ctx = port->adc_ctx;
    pm_evtq_init(&a->evtq);

    rc = a->ops->open(a->port_ctx, a, cfg);
    if (rc != PM_OK) {
        a->ops = NULL;
        return rc;
    }

    a->state = PM_ADC_STATE_READY;
    a->magic = PM_ADC_MAGIC;
    return PM_OK;
}

pm_err_t pm_adc_deinit(pm_adc_t *a)
{
    if (a == NULL) {
        return PM_ERR_NULL;
    }
    if (a->state != PM_ADC_STATE_READY) {
        return PM_OK;                 /* already closed */
    }

    if (a->active != 0u) {
        (void)a->ops->abort(a->port_ctx, a);
        a->active = 0u;
    }

    a->ops->close(a->port_ctx, a);
    a->state   = PM_ADC_STATE_UNINIT;
    a->magic   = 0u;
    a->ops     = NULL;
    a->vref_mv = 0u;
    return PM_OK;
}

pm_err_t pm_adc_read(pm_adc_t *a, uint16_t *raw, uint32_t timeout_ms)
{
    pm_err_t rc;
    uint32_t start;

    if ((a == NULL) || (raw == NULL)) {
        return PM_ERR_NULL;
    }
    if (a->state != PM_ADC_STATE_READY) {
        return PM_ERR_STATE;
    }

    rc = begin_conversion(a);
    if (rc != PM_OK) {
        return rc;
    }
    rc = a->ops->start(a->port_ctx, a);
    if (rc != PM_OK) {
        clear_active(a);
        return rc;
    }

    start = pm_osal_now_ms();
    for (;;) {
        uint8_t  active;
        uint8_t  failed;
        uint16_t value;
        pm_irq_state_t irq = pm_osal_critical_enter();

        active = a->active;
        failed = a->last_error;
        value  = a->last_value;
        pm_osal_critical_exit(irq);

        if (active == 0u) {
            if (failed != 0u) {
                return PM_ERR_IO;
            }
            *raw = value;
            return PM_OK;
        }

        if (timeout_ms == 0u) {
            return PM_ERR_AGAIN;      /* started, still running */
        }

        if ((uint32_t)(pm_osal_now_ms() - start) >= timeout_ms) {
            (void)a->ops->abort(a->port_ctx, a);

            irq = pm_osal_critical_enter();
            active = a->active;       /* the completion ISR may have won */
            failed = a->last_error;
            value  = a->last_value;
            if (active != 0u) {
                a->active = 0u;
                a->stats.aborted++;
            }
            pm_osal_critical_exit(irq);

            if (active == 0u) {       /* finished while the abort ran */
                if (failed != 0u) {
                    return PM_ERR_IO;
                }
                *raw = value;
                return PM_OK;
            }
            return PM_ERR_TIMEOUT;
        }

        pm_osal_delay_ms(1u);
    }
}

pm_err_t pm_adc_read_mv(pm_adc_t *a, uint16_t *mv, uint32_t timeout_ms)
{
    uint16_t raw = 0u;
    pm_err_t rc;

    if (mv == NULL) {
        return PM_ERR_NULL;
    }

    rc = pm_adc_read(a, &raw, timeout_ms);
    if (rc == PM_OK) {
        *mv = pm_adc_to_mv(a, raw);
    }
    return rc;
}

uint16_t pm_adc_to_mv(const pm_adc_t *a, uint16_t raw)
{
    uint32_t vref = ((a != NULL) && (a->vref_mv >= 1000u) &&
                     (a->vref_mv <= 5000u))
                        ? a->vref_mv
                        : 3300u;

    raw = (raw > PM_ADC_MAX_RAW) ? (uint16_t)PM_ADC_MAX_RAW : raw;
    return (uint16_t)(((uint32_t)raw * vref + (PM_ADC_MAX_RAW / 2u)) /
                      PM_ADC_MAX_RAW);
}

bool pm_adc_poll(pm_adc_t *a, pm_evt_t *evt, pm_adc_evt_cb_t cb, void *user_ctx)
{
    pm_evt_t local;

    if (a == NULL) {
        return false;
    }
    if (!pm_evtq_pop(&a->evtq, &local)) {
        return false;
    }
    if (evt != NULL) {
        *evt = local;
    }
    if (cb != NULL) {
        cb(a, &local, user_ctx);      /* main-loop context by contract */
    }
    return true;
}

pm_err_t pm_adc_get_stats(const pm_adc_t *a, pm_adc_stats_t *out)
{
    if ((a == NULL) || (out == NULL)) {
        return PM_ERR_NULL;
    }
    *out = a->stats;
    return PM_OK;
}

/* ------------------------------------------------------------ ISR hooks */

void pm_adc_hw_done(pm_adc_t *a, uint16_t raw)
{
    if ((a == NULL) || (a->state != PM_ADC_STATE_READY)) {
        return;
    }
    if (a->active == 0u) {
        return;                       /* late completion after an abort */
    }

    a->last_value = (raw > PM_ADC_MAX_RAW) ? (uint16_t)PM_ADC_MAX_RAW : raw;
    a->last_error = 0u;
    PM_BARRIER();                     /* publish the value before `active` */
    a->active = 0u;

    a->stats.conversions++;
    a->stats.last_value = a->last_value;
    pm_evtq_push(&a->evtq, PM_EVT_ADC_DONE, a->hw_id, a->last_value);
}

void pm_adc_hw_error(pm_adc_t *a, uint16_t hw_error_mask)
{
    if ((a == NULL) || (a->state != PM_ADC_STATE_READY)) {
        return;
    }

    a->stats.errors++;
    a->stats.last_error = hw_error_mask;

    if (a->active != 0u) {
        a->last_error = 1u;
        PM_BARRIER();
        a->active = 0u;
    }
    pm_evtq_push(&a->evtq, PM_EVT_ADC_ERROR, a->hw_id, hw_error_mask);
}
