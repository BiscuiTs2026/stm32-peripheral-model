/*
 * pm_servo.h - angle to pulse-width mapping for a hobby servo (v0.6).
 *
 * Why this is a model of its own instead of a pm_pwm option: the electrical
 * interface never changes (one PWM line, 50 Hz..333 Hz), only the mapping
 * between angle and pulse width does. SG90 wants 500..2500 us over 0..180 deg;
 * an MG996R wants the same range but other endpoints, a digital servo wants
 * 333 Hz. Parameterising that mapping is exactly what the model should own, so
 * swapping the servo means changing two numbers, not the logic.
 *
 * Layering: a device model that composes pm_pwm, like pm_relay composes
 * pm_gpio. It adds no port and no board table (docs/00-contract.md section 24).
 *
 * Lifecycle: init (drives cfg.start_angle_deg) -> set_angle* -> deinit.
 */
#ifndef PM_SERVO_H
#define PM_SERVO_H

#include "pm_common.h"
#include "pm_err.h"
#include "pm_port.h"
#include "pm_pwm.h"

/* Reference endpoints for the common blue SG90 / MG90S class. */
#define PM_SERVO_SG90_ANGLE_MIN    0u
#define PM_SERVO_SG90_ANGLE_MAX    180u
#define PM_SERVO_SG90_PULSE_MIN_US 500u
#define PM_SERVO_SG90_PULSE_MAX_US 2500u

/* Calibration of one servo type. The four endpoints must satisfy
 * angle_min < angle_max and 0 < pulse_min < pulse_max. */
typedef struct {
    uint16_t angle_min_deg;   /* angle that maps to pulse_min_us */
    uint16_t angle_max_deg;   /* angle that maps to pulse_max_us */
    uint16_t pulse_min_us;
    uint16_t pulse_max_us;
} pm_servo_range_t;

typedef struct {
    uint8_t          hw_id;           /* pm_pwm channel, board-defined */
    uint32_t         freq_hz;         /* frame rate, 50 for analog servos */
    pm_servo_range_t range;
    uint16_t         start_angle_deg; /* required: init drives this position */
} pm_servo_cfg_t;

typedef struct pm_servo pm_servo_t;

struct pm_servo {
    uint32_t         magic;
    uint8_t          state;
    uint8_t          hw_id;
    uint16_t         angle_deg;   /* last commanded angle */
    uint32_t         freq_hz;     /* frame rate the channel was opened at */
    pm_servo_range_t range;
    pm_pwm_t         pwm;         /* composed channel, same layer */
};

/* Angle -> pulse width, integer only, rounded to the nearest microsecond.
 * PM_ERR_NULL  : range or pulse_us is NULL
 * PM_ERR_PARAM : range endpoints are not strictly increasing (or pulse_min 0)
 * PM_ERR_RANGE : angle outside [angle_min_deg, angle_max_deg]; no clamping.
 * This is a pure function so the mapping can be unit-tested without hardware. */
pm_err_t pm_servo_pulse_from_angle(const pm_servo_range_t *range,
                                   uint16_t angle_deg, uint16_t *pulse_us);

/* Opens the PWM channel at cfg->freq_hz and immediately drives
 * cfg->start_angle_deg. Returns PM_ERR_NULL, PM_ERR_PARAM (bad frequency or
 * range), PM_ERR_RANGE (start angle outside the range, or pulse_max_us longer
 * than one frame), PM_ERR_BUSY (already open) or a pm_pwm error. */
pm_err_t pm_servo_init(pm_servo_t *s, const pm_servo_cfg_t *cfg);

/* Stops the output and releases the channel. Idempotent. */
pm_err_t pm_servo_deinit(pm_servo_t *s);

/* Converts and applies one angle. PM_ERR_RANGE when the angle is outside the
 * configured range; the previous position is kept. */
pm_err_t pm_servo_set_angle(pm_servo_t *s, uint16_t angle_deg);

/* Last commanded angle. PM_ERR_STATE when not initialized. */
pm_err_t pm_servo_get_angle(const pm_servo_t *s, uint16_t *angle_deg);

#endif /* PM_SERVO_H */
