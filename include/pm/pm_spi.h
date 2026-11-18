/*
 * pm_spi.h - full-duplex SPI master model API.
 *
 * Lifecycle:  init -> (transfer/poll)* -> deinit
 *
 * Transfer model:
 *  - One transfer is in flight at a time. A transfer is "one chip-select
 *    window": len bytes are clocked out of `tx` while len bytes are clocked
 *    into `rx`, in lock step. There is no queue; the caller owns both buffers
 *    and keeps them alive for the whole transfer.
 *  - `use_dma` in the config is a request, not a guarantee: a port that cannot
 *    provide DMA fails the transfer setup with PM_ERR_IO instead of silently
 *    falling back to interrupts.
 *
 * ISR-to-main-loop path (STM32F1 port):
 *   SPIx_IRQHandler -> HAL_SPI_IRQHandler -> HAL_SPI_TxRxCpltCallback
 *     -> pm_spi_hw_done()   (clears `active`, counts bytes, pushes SPI_DONE)
 *   DMA path:
 *   DMA1_ChannelN_IRQHandler -> HAL_DMA_IRQHandler -> SPI DMATransmitReceiveCplt
 *     -> HAL_SPI_TxRxCpltCallback -> pm_spi_hw_done()
 *   Errors: HAL_SPI_ErrorCallback -> pm_spi_hw_error() -> SPI_ERROR event.
 *   The ISR never calls user code. pm_spi_poll() runs callbacks in the main
 *   loop only.
 *
 * Timeout:
 *  - timeout_ms == 0: start and return. PM_OK if it already finished,
 *    PM_ERR_AGAIN when the transfer is still running. The buffers stay
 *    borrowed until SPI_DONE/SPI_ERROR is polled.
 *  - timeout_ms > 0: wait up to timeout_ms. On expiry the port aborts the
 *    transfer, `*done` reports the bytes actually clocked, the buffers are no
 *    longer referenced, and PM_ERR_TIMEOUT is returned.
 */
#ifndef PM_SPI_H
#define PM_SPI_H

#include "pm_common.h"
#include "pm_err.h"
#include "pm_port.h"
#include "pm_event.h"

#define PM_SPI_TIMEOUT_NOW 0u
#define PM_SPI_MAX_FRAME   0xFFFFu   /* one transfer is one uint16_t length */

struct pm_spi {
    uint32_t magic;          /* set by pm_spi_init, cleared by deinit */
    uint8_t  state;
    uint8_t  hw_id;
    uint8_t  use_dma;
    uint8_t  reserved;

    volatile uint8_t  active;      /* 1 while a transfer owns the buffers */
    volatile uint8_t  last_error;  /* 1 when the last transfer failed */
    volatile uint16_t last_done;   /* bytes clocked by the last transfer */

    uint32_t clock_hz;             /* effective SCK rate reported by the port */

    pm_evtq_t            evtq;
    const pm_spi_ops_t  *ops;
    void                *port_ctx;
    pm_spi_stats_t       stats;
};

typedef void (*pm_spi_evt_cb_t)(pm_spi_t *s, const pm_evt_t *evt, void *user_ctx);

/* Initializes the object and opens the hardware instance described by cfg.
 * The object must be zero-initialized. Returns PM_ERR_PARAM for an impossible
 * clock/mode, PM_ERR_RANGE for an unknown hw_id, PM_ERR_BUSY when the object
 * or the hardware instance is already open. */
pm_err_t pm_spi_init(pm_spi_t *s, const pm_spi_cfg_t *cfg);

/* Aborts an in-flight transfer, then releases the instance. Idempotent. */
pm_err_t pm_spi_deinit(pm_spi_t *s);

/* Full-duplex transfer, see the timeout rules above.
 * Both `tx` and `rx` are mandatory; there is no half-duplex shortcut.
 * PM_ERR_PARAM when len > PM_SPI_MAX_FRAME, PM_ERR_BUSY when a transfer is
 * already in flight. */
pm_err_t pm_spi_transfer(pm_spi_t *s, const uint8_t *tx, uint8_t *rx,
                         size_t len, size_t *done, uint32_t timeout_ms);

/* Pops one pending event (may be NULL) and optionally invokes `cb` with it in
 * main-loop context. Returns false when the queue is empty. */
bool pm_spi_poll(pm_spi_t *s, pm_evt_t *evt, pm_spi_evt_cb_t cb, void *user_ctx);

pm_err_t pm_spi_get_stats(const pm_spi_t *s, pm_spi_stats_t *out);

/* Effective SCK rate after the prescaler; 0 for an invalid object. */
uint32_t pm_spi_clock_hz(const pm_spi_t *s);

/* -------------------------------------------------- ISR-side internal hooks
 * Called only by the port backend, from interrupt context. They never call
 * user code and never block. */
void pm_spi_hw_done(pm_spi_t *s, uint16_t len);
void pm_spi_hw_error(pm_spi_t *s, uint16_t hw_error_mask, uint16_t done);

#endif /* PM_SPI_H */
