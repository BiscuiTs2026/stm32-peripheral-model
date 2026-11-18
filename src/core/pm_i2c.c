/*
 * pm_i2c.c - I2C master model, independent of any vendor header.
 *
 * The model owns the transaction lifecycle, the deadline and the event queue.
 * The port owns the wires: it starts the HAL state machine and turns
 * completion or error interrupts into pm_i2c_hw_done() / pm_i2c_hw_error().
 */
#include "pm_i2c.h"
#include "pm_osal.h"
#include <string.h>

#define PM_I2C_STATE_UNINIT 0u
#define PM_I2C_STATE_READY  1u

#define PM_I2C_MAGIC        0x49324D31u   /* "I2M1" */

#define PM_I2C_CLOCK_MIN    10000u
#define PM_I2C_CLOCK_MAX    400000u

static bool cfg_valid(const pm_i2c_cfg_t *cfg)
{
    return (cfg->clock_hz >= PM_I2C_CLOCK_MIN) &&
           (cfg->clock_hz <= PM_I2C_CLOCK_MAX);
}

static pm_err_t begin_transaction(pm_i2c_t *s)
{
    pm_irq_state_t irq = pm_osal_critical_enter();
    pm_err_t rc = PM_OK;

    if (s->active != 0u) {
        rc = PM_ERR_BUSY;             /* one transaction at a time */
    } else {
        s->active     = 1u;
        s->last_done  = 0u;
        s->last_error = 0u;
    }

    pm_osal_critical_exit(irq);
    return rc;
}

/* Clears `active` after a start failure, without touching the counters. */
static void clear_active(pm_i2c_t *s)
{
    pm_irq_state_t irq = pm_osal_critical_enter();

    s->active = 0u;

    pm_osal_critical_exit(irq);
}

/* Waits for the port to finish the transaction started by the caller.
 * `expected` is wr_len + rd_len for a write_read. */
static pm_err_t wait_complete(pm_i2c_t *s, uint16_t expected, size_t *done,
                              uint32_t timeout_ms)
{
    uint32_t start = pm_osal_now_ms();

    for (;;) {
        uint8_t  active;
        uint8_t  failed;
        uint16_t got;
        pm_irq_state_t irq;

        irq = pm_osal_critical_enter();
        active = s->active;
        failed = s->last_error;
        got    = s->last_done;
        pm_osal_critical_exit(irq);

        if (active == 0u) {
            if (done != NULL) {
                *done = got;
            }
            if (failed != 0u) {
                return PM_ERR_IO;
            }
            return (got == expected) ? PM_OK : PM_ERR_IO;
        }

        if (timeout_ms == 0u) {
            return PM_ERR_AGAIN;      /* started, still owning the buffers */
        }

        if ((uint32_t)(pm_osal_now_ms() - start) >= timeout_ms) {
            uint16_t stopped = 0u;

            if (s->ops->abort(s->port_ctx, s, &stopped) != PM_OK) {
                stopped = 0u;
            }

            irq = pm_osal_critical_enter();
            active = s->active;       /* the completion ISR may have won */
            failed = s->last_error;
            got    = s->last_done;
            if (active != 0u) {
                s->active    = 0u;
                s->last_done = stopped;
                s->stats.aborted++;
            }
            pm_osal_critical_exit(irq);

            if (active == 0u) {       /* finished while the abort ran */
                if (done != NULL) {
                    *done = got;
                }
                return (failed != 0u) ? PM_ERR_IO : PM_OK;
            }
            if (done != NULL) {
                *done = stopped;
            }
            return PM_ERR_TIMEOUT;
        }

        pm_osal_delay_ms(1u);
    }
}

static bool addr_valid(uint8_t addr7)
{
    return addr7 <= 0x7Fu;
}

pm_err_t pm_i2c_init(pm_i2c_t *i, const pm_i2c_cfg_t *cfg)
{
    const pm_port_t *port;
    pm_err_t rc;
    uint32_t actual_hz = 0u;

    if ((i == NULL) || (cfg == NULL)) {
        return PM_ERR_NULL;
    }
    if (!cfg_valid(cfg)) {
        return PM_ERR_PARAM;
    }
    if ((i->magic == PM_I2C_MAGIC) && (i->state == PM_I2C_STATE_READY)) {
        return PM_ERR_BUSY;
    }

    port = pm_port_get();
    if ((port == NULL) || (port->i2c == NULL)) {
        return PM_ERR_STATE;
    }
    if (cfg->hw_id >= port->i2c_count) {
        return PM_ERR_RANGE;
    }

    (void)memset(i, 0, sizeof(*i));
    i->hw_id    = cfg->hw_id;
    i->ops      = port->i2c;
    i->port_ctx = port->i2c_ctx;
    pm_evtq_init(&i->evtq);

    rc = i->ops->open(i->port_ctx, i, cfg, &actual_hz);
    if (rc != PM_OK) {
        i->ops = NULL;
        return rc;
    }

    i->clock_hz = (actual_hz != 0u) ? actual_hz : cfg->clock_hz;
    i->state    = PM_I2C_STATE_READY;
    i->magic    = PM_I2C_MAGIC;
    return PM_OK;
}

pm_err_t pm_i2c_deinit(pm_i2c_t *i)
{
    if (i == NULL) {
        return PM_ERR_NULL;
    }
    if (i->state != PM_I2C_STATE_READY) {
        return PM_OK;                 /* already closed */
    }

    if (i->active != 0u) {
        uint16_t ignored = 0u;
        (void)i->ops->abort(i->port_ctx, i, &ignored);
        i->active = 0u;
    }

    i->ops->close(i->port_ctx, i);
    i->state    = PM_I2C_STATE_UNINIT;
    i->magic    = 0u;
    i->ops      = NULL;
    i->clock_hz = 0u;
    return PM_OK;
}

pm_err_t pm_i2c_write(pm_i2c_t *i, uint8_t addr7, const uint8_t *data,
                      size_t len, size_t *done, uint32_t timeout_ms)
{
    pm_err_t rc;

    if (done != NULL) {
        *done = 0u;
    }
    if ((i == NULL) || (data == NULL)) {
        return PM_ERR_NULL;
    }
    if (i->state != PM_I2C_STATE_READY) {
        return PM_ERR_STATE;
    }
    if (!addr_valid(addr7)) {
        return PM_ERR_PARAM;
    }
    if (len == 0u) {
        return PM_OK;
    }
    if (len > PM_I2C_MAX_FRAME) {
        return PM_ERR_PARAM;
    }

    rc = begin_transaction(i);
    if (rc != PM_OK) {
        return rc;
    }
    rc = i->ops->start_write(i->port_ctx, i, addr7, data, (uint16_t)len);
    if (rc != PM_OK) {
        clear_active(i);
        return rc;
    }
    return wait_complete(i, (uint16_t)len, done, timeout_ms);
}

pm_err_t pm_i2c_read(pm_i2c_t *i, uint8_t addr7, uint8_t *data,
                     size_t len, size_t *done, uint32_t timeout_ms)
{
    pm_err_t rc;

    if (done != NULL) {
        *done = 0u;
    }
    if ((i == NULL) || (data == NULL)) {
        return PM_ERR_NULL;
    }
    if (i->state != PM_I2C_STATE_READY) {
        return PM_ERR_STATE;
    }
    if (!addr_valid(addr7)) {
        return PM_ERR_PARAM;
    }
    if (len == 0u) {
        return PM_OK;
    }
    if (len > PM_I2C_MAX_FRAME) {
        return PM_ERR_PARAM;
    }

    rc = begin_transaction(i);
    if (rc != PM_OK) {
        return rc;
    }
    rc = i->ops->start_read(i->port_ctx, i, addr7, data, (uint16_t)len);
    if (rc != PM_OK) {
        clear_active(i);
        return rc;
    }
    return wait_complete(i, (uint16_t)len, done, timeout_ms);
}

pm_err_t pm_i2c_write_read(pm_i2c_t *i, uint8_t addr7, const uint8_t *wr,
                           size_t wr_len, uint8_t *rd, size_t rd_len,
                           size_t *done, uint32_t timeout_ms)
{
    pm_err_t rc;

    if (done != NULL) {
        *done = 0u;
    }
    if ((i == NULL) || (wr == NULL) || (rd == NULL)) {
        return PM_ERR_NULL;
    }
    if (i->state != PM_I2C_STATE_READY) {
        return PM_ERR_STATE;
    }
    if (!addr_valid(addr7)) {
        return PM_ERR_PARAM;
    }
    if ((wr_len == 0u) || (rd_len == 0u)) {
        return PM_ERR_PARAM;
    }
    if ((wr_len > PM_I2C_MAX_FRAME) || (rd_len > PM_I2C_MAX_FRAME)) {
        return PM_ERR_PARAM;
    }

    rc = begin_transaction(i);
    if (rc != PM_OK) {
        return rc;
    }
    rc = i->ops->start_write_read(i->port_ctx, i, addr7, wr, (uint16_t)wr_len,
                                  rd, (uint16_t)rd_len);
    if (rc != PM_OK) {
        clear_active(i);
        return rc;
    }
    return wait_complete(i, (uint16_t)(wr_len + rd_len), done, timeout_ms);
}

bool pm_i2c_poll(pm_i2c_t *i, pm_evt_t *evt, pm_i2c_evt_cb_t cb,
                 void *user_ctx)
{
    pm_evt_t local;

    if (i == NULL) {
        return false;
    }
    if (!pm_evtq_pop(&i->evtq, &local)) {
        return false;
    }
    if (evt != NULL) {
        *evt = local;
    }
    if (cb != NULL) {
        cb(i, &local, user_ctx);      /* main-loop context by contract */
    }
    return true;
}

pm_err_t pm_i2c_get_stats(const pm_i2c_t *i, pm_i2c_stats_t *out)
{
    if ((i == NULL) || (out == NULL)) {
        return PM_ERR_NULL;
    }
    *out = i->stats;
    return PM_OK;
}

uint32_t pm_i2c_clock_hz(const pm_i2c_t *i)
{
    if ((i == NULL) || (i->state != PM_I2C_STATE_READY)) {
        return 0u;
    }
    return i->clock_hz;
}

/* ------------------------------------------------------------ ISR hooks */

void pm_i2c_hw_done(pm_i2c_t *i, uint16_t tx_bytes, uint16_t rx_bytes)
{
    if ((i == NULL) || (i->state != PM_I2C_STATE_READY)) {
        return;
    }
    if (i->active == 0u) {
        return;                       /* late completion after an abort */
    }

    i->last_done  = (uint16_t)(tx_bytes + rx_bytes);
    i->last_error = 0u;
    PM_BARRIER();                     /* publish the counters before `active` */
    i->active = 0u;

    i->stats.transfers++;
    i->stats.tx_bytes += tx_bytes;
    i->stats.rx_bytes += rx_bytes;
    pm_evtq_push(&i->evtq, PM_EVT_I2C_DONE, i->hw_id, i->last_done);
}

void pm_i2c_hw_error(pm_i2c_t *i, uint16_t hw_error_mask, uint16_t done)
{
    if ((i == NULL) || (i->state != PM_I2C_STATE_READY)) {
        return;
    }

    i->stats.errors++;
    i->stats.last_error = hw_error_mask;

    if (i->active != 0u) {
        i->last_done  = done;
        i->last_error = 1u;
        PM_BARRIER();
        i->active = 0u;
    }
    pm_evtq_push(&i->evtq, PM_EVT_I2C_ERROR, i->hw_id, hw_error_mask);
}
