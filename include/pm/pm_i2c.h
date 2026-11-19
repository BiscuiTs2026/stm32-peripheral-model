/*
 * pm_i2c.h - interrupt-driven I2C master model API.
 *
 * Lifecycle:  init -> (write/read/write_read/poll)* -> deinit
 *
 * Transfer model:
 *  - One transfer is in flight at a time. Each call is one bus transaction:
 *      write       : START, addr+W, data..., STOP
 *      read        : START, addr+R, data..., STOP
 *      write_read  : START, addr+W, wr..., repeated START, addr+R, rd...,
 *                    STOP (the register-read pattern; no STOP in between)
 *  - There is no queue. The caller owns the buffers and keeps them alive
 *    until the transfer completes or is aborted.
 *  - No busy-waiting: the hardware is driven by the HAL state machine through
 *    I2C event/error interrupts. `pm_i2c_write` with timeout_ms > 0 sleeps
 *    between deadline checks and is never the one moving bytes.
 *  - DMA is not used by the F1 port in this release. The IRQ path is the
 *    portable one; DMA for I2C is a later addition, not a config flag with a
 *    silent fallback.
 *
 * ISR-to-main-loop path (STM32F1 port):
 *   I2C1_EV_IRQHandler -> HAL_I2C_EV_IRQHandler
 *     -> HAL_I2C_MasterTxCpltCallback (may start the read phase of a
 *        write_read from ISR context) / HAL_I2C_MasterRxCpltCallback
 *     -> pm_i2c_hw_done()   (clears `active`, counts bytes, pushes I2C_DONE)
 *   I2C1_ER_IRQHandler -> HAL_I2C_ER_IRQHandler -> HAL_I2C_ErrorCallback
 *     -> pm_i2c_hw_error() -> I2C_ERROR event.
 *   The ISR never calls user code. pm_i2c_poll() runs callbacks in the main
 *   loop only.
 *
 * Timeout:
 *  - timeout_ms == 0: start and return. PM_OK if it already finished,
 *    PM_ERR_AGAIN when it is still running. The buffers stay borrowed until
 *    I2C_DONE/I2C_ERROR is polled.
 *  - timeout_ms > 0: wait up to timeout_ms. On expiry the port resets the
 *    peripheral (DeInit + Init) and `*done` reports the bytes actually moved.
 *    PM_ERR_TIMEOUT is returned. Bus clearing (9 clock pulses on SCL) is NOT
 *    attempted by this release; a slave that holds SDA low keeps the bus
 *    unusable until it is powered off or the line is fixed.
 *
 * Errors:
 *  - A NACK from the addressed device (or during data) ends the transfer with
 *    PM_ERR_IO and a I2C_ERROR event whose arg is the HAL error mask.
 *  - Optional classification (pm_i2c_set_error_counting) narrows that code to
 *    PM_ERR_NACK / PM_ERR_BUS and keeps per-class counters; it is off by
 *    default so existing callers keep the PM_ERR_IO branch. Contract 12.7.
 */
#ifndef PM_I2C_H
#define PM_I2C_H

#include "pm_common.h"
#include "pm_err.h"
#include "pm_port.h"
#include "pm_event.h"

#define PM_I2C_TIMEOUT_NOW 0u
#define PM_I2C_MAX_FRAME   0xFFFFu   /* one phase is one uint16_t length */

/* Classification of one failed transfer (optional feature, see
 * pm_i2c_set_error_counting). */
typedef enum {
    PM_I2C_ERRCLASS_NONE = 0,
    PM_I2C_ERRCLASS_AF,       /* NACK: the device did not answer */
    PM_I2C_ERRCLASS_BERR,     /* misplaced START/STOP */
    PM_I2C_ERRCLASS_ARLO,     /* lost arbitration */
    PM_I2C_ERRCLASS_OVR,      /* overrun, a byte was dropped */
    PM_I2C_ERRCLASS_TIMEOUT,  /* peripheral or deadline timeout */
    PM_I2C_ERRCLASS_OTHER     /* port-local failure, see contract 12.4 */
} pm_i2c_err_class_t;

typedef struct {
    uint32_t af;
    uint32_t berr;
    uint32_t arlo;
    uint32_t ovr;
    uint32_t timeout;
    uint32_t other;
} pm_i2c_err_counts_t;

struct pm_i2c {
    uint32_t magic;          /* set by pm_i2c_init, cleared by deinit */
    uint8_t  state;
    uint8_t  hw_id;
    uint8_t  count_errors;   /* 1 when error classification is enabled */
    uint8_t  last_class;     /* pm_i2c_err_class_t of the last failure */
    uint8_t  reserved[2];

    volatile uint8_t  active;      /* 1 while a transfer owns the buffers */
    volatile uint8_t  last_error;  /* 1 when the last transfer failed */
    volatile uint16_t last_done;   /* bytes moved by the last transfer */

    uint32_t clock_hz;             /* effective SCL rate reported by the port */

    pm_evtq_t            evtq;
    const pm_i2c_ops_t  *ops;
    void                *port_ctx;
    pm_i2c_stats_t       stats;
    pm_i2c_err_counts_t  err_counts;   /* only touched when count_errors = 1 */
};

typedef void (*pm_i2c_evt_cb_t)(pm_i2c_t *i, const pm_evt_t *evt,
                                void *user_ctx);

/* Initializes the object and opens the hardware instance described by cfg.
 * The object must be zero-initialized. Returns PM_ERR_PARAM for an impossible
 * clock, PM_ERR_RANGE for an unknown hw_id, PM_ERR_BUSY when the object or the
 * hardware instance is already open. */
pm_err_t pm_i2c_init(pm_i2c_t *i, const pm_i2c_cfg_t *cfg);

/* Aborts an in-flight transfer, then releases the instance. Idempotent. */
pm_err_t pm_i2c_deinit(pm_i2c_t *i);

/* See the timeout rules above. `addr7` is a 7-bit address (0x70 for AHT20,
 * 0x3C/0x3D class devices, ...). */
pm_err_t pm_i2c_write(pm_i2c_t *i, uint8_t addr7, const uint8_t *data,
                      size_t len, size_t *done, uint32_t timeout_ms);
pm_err_t pm_i2c_read(pm_i2c_t *i, uint8_t addr7, uint8_t *data,
                     size_t len, size_t *done, uint32_t timeout_ms);
pm_err_t pm_i2c_write_read(pm_i2c_t *i, uint8_t addr7, const uint8_t *wr,
                           size_t wr_len, uint8_t *rd, size_t rd_len,
                           size_t *done, uint32_t timeout_ms);

/* Pops one pending event (may be NULL) and optionally invokes `cb` with it in
 * main-loop context. Returns false when the queue is empty. */
bool pm_i2c_poll(pm_i2c_t *i, pm_evt_t *evt, pm_i2c_evt_cb_t cb,
                 void *user_ctx);

pm_err_t pm_i2c_get_stats(const pm_i2c_t *i, pm_i2c_stats_t *out);

/* ----------------------------------------- optional error classification
 * Off by default: pm_i2c_write/read/write_read keep returning PM_ERR_IO for
 * every hardware failure, exactly as before, and no counter moves. Enabling it
 * clears the counters first and switches the return codes to
 *   AF                -> PM_ERR_NACK   (device absent / wrong address)
 *   BERR/ARLO/OVR/... -> PM_ERR_BUS    (wiring or bus contention)
 *   timeout           -> PM_ERR_TIMEOUT
 * A short transfer without a hardware error stays PM_ERR_IO in both modes.
 * See docs/00-contract.md section 12.7. */
pm_err_t pm_i2c_set_error_counting(pm_i2c_t *i, bool on);

/* Snapshot of the classified counters. All zero while counting is disabled.
 * PM_ERR_NULL for a NULL argument, PM_ERR_STATE for an uninitialized object. */
pm_err_t pm_i2c_get_error_counts(const pm_i2c_t *i, pm_i2c_err_counts_t *out);

/* Effective SCL rate after clock rounding; 0 for an invalid object. */
uint32_t pm_i2c_clock_hz(const pm_i2c_t *i);

/* -------------------------------------------------- ISR-side internal hooks
 * Called only by the port backend, from interrupt context. They never call
 * user code and never block. */
void pm_i2c_hw_done(pm_i2c_t *i, uint16_t tx_bytes, uint16_t rx_bytes);
void pm_i2c_hw_error(pm_i2c_t *i, uint16_t hw_error_mask, uint16_t done);

#endif /* PM_I2C_H */
