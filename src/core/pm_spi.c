/*
 * pm_spi.c - SPI master model, independent of any vendor header.
 *
 * The model owns the transfer lifecycle, the deadline and the event queue.
 * The port owns the wires: it starts the hardware and turns completion or
 * error interrupts into pm_spi_hw_done() / pm_spi_hw_error().
 */
#include "pm_spi.h"
#include "pm_osal.h"
#include <string.h>

#define PM_SPI_STATE_UNINIT 0u
#define PM_SPI_STATE_READY  1u

#define PM_SPI_MAGIC        0x50534D31u   /* "PSM1" */

#define PM_SPI_CLOCK_MIN    1000u
#define PM_SPI_CLOCK_MAX    36000000u     /* PCLK2 / 2 on this board */

static bool cfg_valid(const pm_spi_cfg_t *cfg)
{
    if ((cfg->clock_hz < PM_SPI_CLOCK_MIN) || (cfg->clock_hz > PM_SPI_CLOCK_MAX)) {
        return false;
    }
    if ((cfg->mode != PM_SPI_MODE_0) && (cfg->mode != PM_SPI_MODE_1) &&
        (cfg->mode != PM_SPI_MODE_2) && (cfg->mode != PM_SPI_MODE_3)) {
        return false;
    }
    if ((cfg->bit_order != PM_SPI_MSB_FIRST) &&
        (cfg->bit_order != PM_SPI_LSB_FIRST)) {
        return false;
    }
    if ((cfg->nss != PM_SPI_NSS_SOFT) && (cfg->nss != PM_SPI_NSS_HARD)) {
        return false;
    }
    return true;
}

pm_err_t pm_spi_init(pm_spi_t *s, const pm_spi_cfg_t *cfg)
{
    const pm_port_t *port;
    pm_err_t rc;
    uint32_t actual_hz = 0u;

    if ((s == NULL) || (cfg == NULL)) {
        return PM_ERR_NULL;
    }
    if (!cfg_valid(cfg)) {
        return PM_ERR_PARAM;
    }
    if ((s->magic == PM_SPI_MAGIC) && (s->state == PM_SPI_STATE_READY)) {
        return PM_ERR_BUSY;
    }

    port = pm_port_get();
    if ((port == NULL) || (port->spi == NULL)) {
        return PM_ERR_STATE;
    }
    if (cfg->hw_id >= port->spi_count) {
        return PM_ERR_RANGE;
    }

    (void)memset(s, 0, sizeof(*s));
    s->hw_id    = cfg->hw_id;
    s->use_dma  = cfg->use_dma ? 1u : 0u;
    s->ops      = port->spi;
    s->port_ctx = port->spi_ctx;
    pm_evtq_init(&s->evtq);

    rc = s->ops->open(s->port_ctx, s, cfg, &actual_hz);
    if (rc != PM_OK) {
        s->ops = NULL;
        return rc;
    }

    s->clock_hz = (actual_hz != 0u) ? actual_hz : cfg->clock_hz;
    s->state    = PM_SPI_STATE_READY;
    s->magic    = PM_SPI_MAGIC;
    return PM_OK;
}

pm_err_t pm_spi_deinit(pm_spi_t *s)
{
    if (s == NULL) {
        return PM_ERR_NULL;
    }
    if (s->state != PM_SPI_STATE_READY) {
        return PM_OK;                 /* already closed */
    }

    if (s->active != 0u) {
        uint16_t ignored = 0u;
        (void)s->ops->abort(s->port_ctx, s, &ignored);
        s->active = 0u;
    }

    s->ops->close(s->port_ctx, s);
    s->state    = PM_SPI_STATE_UNINIT;
    s->magic    = 0u;
    s->ops      = NULL;
    s->clock_hz = 0u;
    return PM_OK;
}

pm_err_t pm_spi_transfer(pm_spi_t *s, const uint8_t *tx, uint8_t *rx,
                         size_t len, size_t *done, uint32_t timeout_ms)
{
    uint16_t n;
    uint32_t start;
    pm_err_t rc;
    pm_irq_state_t irq;

    if (done != NULL) {
        *done = 0u;
    }
    if ((s == NULL) || (tx == NULL) || (rx == NULL)) {
        return PM_ERR_NULL;
    }
    if (s->state != PM_SPI_STATE_READY) {
        return PM_ERR_STATE;
    }
    if (len == 0u) {
        return PM_OK;
    }
    if (len > PM_SPI_MAX_FRAME) {
        return PM_ERR_PARAM;
    }

    irq = pm_osal_critical_enter();
    if (s->active != 0u) {
        pm_osal_critical_exit(irq);
        return PM_ERR_BUSY;
    }
    s->active     = 1u;
    s->last_done  = 0u;
    s->last_error = 0u;
    pm_osal_critical_exit(irq);

    n = (uint16_t)len;
    rc = s->ops->start(s->port_ctx, s, tx, rx, n);
    if (rc != PM_OK) {
        irq = pm_osal_critical_enter();
        s->active = 0u;
        pm_osal_critical_exit(irq);
        return rc;
    }

    start = pm_osal_now_ms();

    for (;;) {
        uint8_t  active;
        uint8_t  failed;
        uint16_t got;

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
            return (got == n) ? PM_OK : PM_ERR_IO;
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

bool pm_spi_poll(pm_spi_t *s, pm_evt_t *evt, pm_spi_evt_cb_t cb, void *user_ctx)
{
    pm_evt_t local;

    if (s == NULL) {
        return false;
    }
    if (!pm_evtq_pop(&s->evtq, &local)) {
        return false;
    }
    if (evt != NULL) {
        *evt = local;
    }
    if (cb != NULL) {
        cb(s, &local, user_ctx);      /* main-loop context by contract */
    }
    return true;
}

pm_err_t pm_spi_get_stats(const pm_spi_t *s, pm_spi_stats_t *out)
{
    if ((s == NULL) || (out == NULL)) {
        return PM_ERR_NULL;
    }
    *out = s->stats;
    return PM_OK;
}

uint32_t pm_spi_clock_hz(const pm_spi_t *s)
{
    if ((s == NULL) || (s->state != PM_SPI_STATE_READY)) {
        return 0u;
    }
    return s->clock_hz;
}

/* ------------------------------------------------------------ ISR hooks */

void pm_spi_hw_done(pm_spi_t *s, uint16_t len)
{
    if ((s == NULL) || (s->state != PM_SPI_STATE_READY)) {
        return;
    }
    if (s->active == 0u) {
        return;                       /* late completion after an abort */
    }

    s->last_done  = len;
    s->last_error = 0u;
    PM_BARRIER();                     /* publish the counters before `active` */
    s->active = 0u;

    s->stats.transfers++;
    s->stats.tx_bytes += len;
    s->stats.rx_bytes += len;
    pm_evtq_push(&s->evtq, PM_EVT_SPI_DONE, s->hw_id, len);
}

void pm_spi_hw_error(pm_spi_t *s, uint16_t hw_error_mask, uint16_t done)
{
    if ((s == NULL) || (s->state != PM_SPI_STATE_READY)) {
        return;
    }

    s->stats.errors++;
    s->stats.last_error = hw_error_mask;

    if (s->active != 0u) {
        s->last_done  = done;
        s->last_error = 1u;
        PM_BARRIER();
        s->active = 0u;
    }
    pm_evtq_push(&s->evtq, PM_EVT_SPI_ERROR, s->hw_id, hw_error_mask);
}
