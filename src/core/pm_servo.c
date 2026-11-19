/*
 * pm_servo.c - angle to pulse width, then pulse to PWM duty.
 *
 * Hardware independent: the only down-call is pm_pwm, the component layer.
 */
#include "pm_servo.h"
#include <string.h>

#define PM_SERVO_STATE_UNINIT 0u
#define PM_SERVO_STATE_READY  1u
#define PM_SERVO_MAGIC        0x53525631u   /* "SRV1" */
#define PM_SERVO_US_PER_S     1000000u

static bool range_valid(const pm_servo_range_t *r)
{
    return (r->angle_min_deg < r->angle_max_deg) &&
           (r->pulse_min_us != 0u) &&
           (r->pulse_min_us < r->pulse_max_us);
}

pm_err_t pm_servo_pulse_from_angle(const pm_servo_range_t *range,
                                   uint16_t angle_deg, uint16_t *pulse_us)
{
    uint32_t a_span;
    uint32_t p_span;
    uint32_t offset;

    if ((range == NULL) || (pulse_us == NULL)) {
        return PM_ERR_NULL;
    }
    if (!range_valid(range)) {
        return PM_ERR_PARAM;
    }
    if ((angle_deg < range->angle_min_deg) ||
        (angle_deg > range->angle_max_deg)) {
        return PM_ERR_RANGE;          /* no clamping: the caller asked wrong */
    }

    a_span = (uint32_t)(range->angle_max_deg - range->angle_min_deg);
    p_span = (uint32_t)(range->pulse_max_us - range->pulse_min_us);
    offset = (uint32_t)(angle_deg - range->angle_min_deg);

    /* Nearest microsecond, integer only: (offset * span + half) / span. */
    *pulse_us = (uint16_t)((uint32_t)range->pulse_min_us +
                           ((offset * p_span) + (a_span / 2u)) / a_span);
    return PM_OK;
}

/* Pulse -> permille at the frequency the channel actually runs at. Working in
 * microseconds keeps the product inside 32 bits (pulse <= period_us <= 1e6). */
static pm_err_t apply_pulse(pm_servo_t *s, uint16_t pulse_us)
{
    uint32_t freq   = pm_pwm_frequency_hz(&s->pwm);
    uint32_t period = (freq == 0u) ? 0u : (PM_SERVO_US_PER_S / freq);
    uint32_t duty;

    if (period == 0u) {
        return PM_ERR_STATE;
    }
    if ((uint32_t)pulse_us > period) {
        return PM_ERR_RANGE;          /* pulse longer than one frame */
    }
    duty = (((uint32_t)pulse_us * PM_PWM_DUTY_MAX) + (period / 2u)) / period;
    return pm_pwm_set_duty(&s->pwm, (uint16_t)duty);
}

pm_err_t pm_servo_init(pm_servo_t *s, const pm_servo_cfg_t *cfg)
{
    pm_pwm_cfg_t pwm_cfg;
    pm_err_t     rc;
    uint32_t     period_us;

    if ((s == NULL) || (cfg == NULL)) {
        return PM_ERR_NULL;
    }
    if ((s->magic == PM_SERVO_MAGIC) && (s->state == PM_SERVO_STATE_READY)) {
        return PM_ERR_BUSY;
    }
    if (!range_valid(&cfg->range)) {
        return PM_ERR_PARAM;
    }
    if ((cfg->freq_hz == 0u) || (cfg->freq_hz > PM_SERVO_US_PER_S)) {
        return PM_ERR_PARAM;
    }
    if ((cfg->start_angle_deg < cfg->range.angle_min_deg) ||
        (cfg->start_angle_deg > cfg->range.angle_max_deg)) {
        return PM_ERR_RANGE;
    }
    period_us = PM_SERVO_US_PER_S / cfg->freq_hz;
    if ((uint32_t)cfg->range.pulse_max_us > period_us) {
        return PM_ERR_RANGE;          /* the frame cannot hold the full range */
    }

    (void)memset(s, 0, sizeof(*s));
    pwm_cfg.hw_id         = cfg->hw_id;
    pwm_cfg.freq_hz       = cfg->freq_hz;
    pwm_cfg.duty_permille = 0u;        /* start silent, then drive the angle */
    rc = pm_pwm_init(&s->pwm, &pwm_cfg);
    if (rc != PM_OK) {
        return rc;
    }

    s->range  = cfg->range;
    s->hw_id  = cfg->hw_id;
    s->freq_hz = pm_pwm_frequency_hz(&s->pwm);
    s->state  = PM_SERVO_STATE_READY;
    s->magic  = PM_SERVO_MAGIC;

    rc = pm_servo_set_angle(s, cfg->start_angle_deg);
    if (rc != PM_OK) {
        (void)pm_pwm_deinit(&s->pwm);
        (void)memset(s, 0, sizeof(*s));
        return rc;
    }
    return PM_OK;
}

pm_err_t pm_servo_deinit(pm_servo_t *s)
{
    if (s == NULL) {
        return PM_ERR_NULL;
    }
    if ((s->magic != PM_SERVO_MAGIC) || (s->state != PM_SERVO_STATE_READY)) {
        return PM_ERR_STATE;
    }
    (void)pm_pwm_deinit(&s->pwm);      /* stops the compare output */
    (void)memset(s, 0, sizeof(*s));
    return PM_OK;
}

pm_err_t pm_servo_set_angle(pm_servo_t *s, uint16_t angle_deg)
{
    uint16_t pulse = 0u;
    pm_err_t rc;

    if (s == NULL) {
        return PM_ERR_NULL;
    }
    if (s->state != PM_SERVO_STATE_READY) {
        return PM_ERR_STATE;
    }

    rc = pm_servo_pulse_from_angle(&s->range, angle_deg, &pulse);
    if (rc != PM_OK) {
        return rc;
    }
    rc = apply_pulse(s, pulse);
    if (rc != PM_OK) {
        return rc;
    }
    s->angle_deg = angle_deg;
    return PM_OK;
}

pm_err_t pm_servo_get_angle(const pm_servo_t *s, uint16_t *angle_deg)
{
    if ((s == NULL) || (angle_deg == NULL)) {
        return PM_ERR_NULL;
    }
    if (s->state != PM_SERVO_STATE_READY) {
        return PM_ERR_STATE;
    }
    *angle_deg = s->angle_deg;
    return PM_OK;
}
