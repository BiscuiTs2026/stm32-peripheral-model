/*
 * pm_tone.c - audible tone model, hardware independent.
 *
 * Only validation, state and forwarding live here; the tick generator is a
 * port detail.
 */
#include "pm_tone.h"
#include <string.h>

#define PM_TONE_STATE_UNINIT 0u
#define PM_TONE_STATE_READY  1u
#define PM_TONE_MAGIC        0x544F4E31u   /* "TON1" */

static bool params_valid(uint32_t freq_hz, uint16_t duty_permille)
{
    return (freq_hz >= PM_TONE_FREQ_MIN) && (freq_hz <= PM_TONE_FREQ_MAX) &&
           (duty_permille >= PM_TONE_DUTY_MIN) &&
           (duty_permille <= PM_TONE_DUTY_MAX);
}

pm_err_t pm_tone_init(pm_tone_t *t, const pm_tone_cfg_t *cfg)
{
    const pm_port_t *port;
    pm_err_t rc;

    if ((t == NULL) || (cfg == NULL)) {
        return PM_ERR_NULL;
    }
    if (!params_valid(cfg->freq_hz, cfg->duty_permille)) {
        return PM_ERR_PARAM;
    }
    if ((t->magic == PM_TONE_MAGIC) && (t->state == PM_TONE_STATE_READY)) {
        return PM_ERR_BUSY;
    }

    port = pm_port_get();
    if ((port == NULL) || (port->tone == NULL)) {
        return PM_ERR_STATE;
    }
    if (cfg->hw_id >= port->tone_count) {
        return PM_ERR_RANGE;
    }

    (void)memset(t, 0, sizeof(*t));
    t->hw_id    = cfg->hw_id;
    t->ops      = port->tone;
    t->port_ctx = port->tone_ctx;

    rc = t->ops->open(t->port_ctx, t, cfg);
    if (rc != PM_OK) {
        t->ops = NULL;
        return rc;
    }

    t->freq_hz       = cfg->freq_hz;
    t->duty_permille = cfg->duty_permille;
    t->active        = 1u;
    t->state         = PM_TONE_STATE_READY;
    t->magic         = PM_TONE_MAGIC;
    return PM_OK;
}

pm_err_t pm_tone_deinit(pm_tone_t *t)
{
    if (t == NULL) {
        return PM_ERR_NULL;
    }
    if (t->state != PM_TONE_STATE_READY) {
        return PM_OK;
    }

    t->ops->close(t->port_ctx, t);
    (void)memset(t, 0, sizeof(*t));
    return PM_OK;
}

pm_err_t pm_tone_set(pm_tone_t *t, uint32_t freq_hz, uint16_t duty_permille)
{
    pm_err_t rc;

    if (t == NULL) {
        return PM_ERR_NULL;
    }
    if (t->state != PM_TONE_STATE_READY) {
        return PM_ERR_STATE;
    }
    if (!params_valid(freq_hz, duty_permille)) {
        return PM_ERR_PARAM;
    }

    rc = t->ops->set(t->port_ctx, t, freq_hz, duty_permille);
    if (rc == PM_OK) {
        t->freq_hz       = freq_hz;
        t->duty_permille = duty_permille;
        t->active        = 1u;
    }
    return rc;
}

pm_err_t pm_tone_off(pm_tone_t *t)
{
    pm_err_t rc;

    if (t == NULL) {
        return PM_ERR_NULL;
    }
    if (t->state != PM_TONE_STATE_READY) {
        return PM_ERR_STATE;
    }

    rc = t->ops->off(t->port_ctx, t);
    if (rc == PM_OK) {
        t->active = 0u;
    }
    return rc;
}

uint32_t pm_tone_frequency_hz(const pm_tone_t *t)
{
    if ((t == NULL) || (t->state != PM_TONE_STATE_READY)) {
        return 0u;
    }
    return t->freq_hz;
}

uint16_t pm_tone_duty_permille(const pm_tone_t *t)
{
    if ((t == NULL) || (t->state != PM_TONE_STATE_READY)) {
        return 0u;
    }
    return t->duty_permille;
}

bool pm_tone_is_active(const pm_tone_t *t)
{
    if ((t == NULL) || (t->state != PM_TONE_STATE_READY)) {
        return false;
    }
    return t->active != 0u;
}
