/*
 * pm_tone.h - audible tone output driven by a timer interrupt.
 *
 * This is the software-tone backend: the port runs a periodic interrupt and
 * flips one GPIO. It exists because the board-verified buzzer path is a
 * TIM3 interrupt, not a hardware compare channel.
 *
 * Contract:
 *  - The public surface is frequency and duty only. Tick length, tick counts
 *    and the duty quantisation are port implementation details; callers must
 *    not depend on them (docs/00-contract.md section 17).
 *  - Frequency range 100..5000 Hz, duty 1..990 permille. 0 and 1000 are
 *    rejected: silence is pm_tone_off(), not a duty value.
 *  - init() starts the tone. set() changes frequency and duty and keeps the
 *    tone running; off() stops it with the pin idle (low); a later set()
 *    starts it again.
 *  - The timer it uses is claimed through pm_timer.h; a conflict returns
 *    PM_ERR_CONFLICT instead of overwriting another module's time base.
 */
#ifndef PM_TONE_H
#define PM_TONE_H

#include "pm_common.h"
#include "pm_err.h"
#include "pm_port.h"

#define PM_TONE_FREQ_MIN 100u
#define PM_TONE_FREQ_MAX 5000u
#define PM_TONE_DUTY_MIN 1u
#define PM_TONE_DUTY_MAX 990u

struct pm_tone {
    uint32_t magic;
    uint8_t  state;
    uint8_t  hw_id;
    uint8_t  active;         /* 1 while the interrupt is toggling the pin */
    uint8_t  reserved;

    uint32_t freq_hz;
    uint16_t duty_permille;

    const pm_tone_ops_t *ops;
    void                *port_ctx;
};

/* Starts the tone. The object must be zero-initialised. Returns
 * PM_ERR_PARAM for an out-of-range frequency or duty, PM_ERR_RANGE for an
 * unknown hw_id, PM_ERR_BUSY when the object is already open and
 * PM_ERR_CONFLICT when the timer belongs to another module. */
pm_err_t pm_tone_init(pm_tone_t *t, const pm_tone_cfg_t *cfg);

/* Stops the tone and releases it. Idempotent. */
pm_err_t pm_tone_deinit(pm_tone_t *t);

/* Changes frequency and duty and guarantees the tone is running afterwards. */
pm_err_t pm_tone_set(pm_tone_t *t, uint32_t freq_hz, uint16_t duty_permille);

/* Stops toggling; the pin goes to its idle level. Parameters are kept. */
pm_err_t pm_tone_off(pm_tone_t *t);

/* Stored values; 0 / false for an object that is not initialized. */
uint32_t pm_tone_frequency_hz(const pm_tone_t *t);
uint16_t pm_tone_duty_permille(const pm_tone_t *t);
bool     pm_tone_is_active(const pm_tone_t *t);

#endif /* PM_TONE_H */
