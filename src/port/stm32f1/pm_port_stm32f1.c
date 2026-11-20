/*
 * pm_port_stm32f1.c - STM32F1 backend built on the ST HAL.
 *
 * This file and the board file are the only places where vendor types and
 * HAL calls exist. Everything above pm_port.h is vendor-free.
 *
 * Interrupt mapping:
 *   USARTx_IRQHandler -> HAL_UART_IRQHandler -> HAL_UART_*Callback
 *   RX complete       -> pm_uart_hw_rx_byte()   (ISR, no user code)
 *   TX complete       -> pm_uart_hw_tx_complete() (ISR, may start next chunk)
 *   error             -> pm_uart_hw_error()
 *
 * SPI mapping:
 *   SPIx_IRQHandler   -> HAL_SPI_IRQHandler -> HAL_SPI_TxRxCpltCallback
 *     -> pm_spi_hw_done()          (interrupt-driven full duplex)
 *   DMA1_ChannelN_IRQHandler -> HAL_DMA_IRQHandler -> same completion callback
 *     -> pm_spi_hw_done()          (DMA full duplex, optional)
 *   HAL_SPI_ErrorCallback -> pm_spi_hw_error()
 *
 * I2C mapping:
 *   I2C1_EV_IRQHandler -> HAL_I2C_EV_IRQHandler
 *     -> HAL_I2C_MasterTxCpltCallback (starts the read phase of a write_read)
 *     -> HAL_I2C_MasterRxCpltCallback -> pm_i2c_hw_done()
 *   I2C1_ER_IRQHandler -> HAL_I2C_ER_IRQHandler
 *     -> HAL_I2C_ErrorCallback -> pm_i2c_hw_error()
 */
#include "pm_port_stm32f1.h"
#include "pm_osal.h"
#include "pm_uart.h"
#include "board_f103c8.h"

#if defined(PM_ENABLE_SPI)
#include "pm_spi.h"
#endif
#if defined(PM_ENABLE_I2C)
#include "pm_i2c.h"
#endif
#if defined(PM_ENABLE_ADC)
#include "pm_adc.h"
#endif
#if defined(PM_ENABLE_PWM)
#include "pm_pwm.h"
#endif
#if defined(PM_ENABLE_TONE)
#include "pm_tone.h"
#endif
#if defined(PM_ENABLE_TRACK)
#include "pm_track.h"
#endif
#if defined(PM_ENABLE_ENCODER)
#include "pm_encoder.h"
#endif
#include "pm_timer.h"

#include <string.h>

#include "stm32f1xx_hal.h"

/* ------------------------------------------------------------------ GPIO */

static void gpio_clk_enable(GPIO_TypeDef *port)
{
    if (port == GPIOA) {
        __HAL_RCC_GPIOA_CLK_ENABLE();
    } else if (port == GPIOB) {
        __HAL_RCC_GPIOB_CLK_ENABLE();
    } else if (port == GPIOC) {
        __HAL_RCC_GPIOC_CLK_ENABLE();
    } else if (port == GPIOD) {
        __HAL_RCC_GPIOD_CLK_ENABLE();
    }
}

static uint32_t gpio_speed_to_hal(pm_gpio_speed_t speed)
{
    switch (speed) {
    case PM_GPIO_SPEED_LOW:    return GPIO_SPEED_FREQ_LOW;
    case PM_GPIO_SPEED_MEDIUM: return GPIO_SPEED_FREQ_MEDIUM;
    case PM_GPIO_SPEED_HIGH:   return GPIO_SPEED_FREQ_HIGH;
    default:                   return GPIO_SPEED_FREQ_LOW;
    }
}

static uint32_t gpio_pull_to_hal(pm_gpio_pull_t pull)
{
    switch (pull) {
    case PM_GPIO_PULL_UP:   return GPIO_PULLUP;
    case PM_GPIO_PULL_DOWN: return GPIO_PULLDOWN;
    case PM_GPIO_PULL_NONE:
    default:                return GPIO_NOPULL;
    }
}

static pm_err_t f1_gpio_configure(void *ctx, pm_gpio_pin_t pin, const pm_gpio_cfg_t *cfg)
{
    const pm_f1_gpio_ctx_t *c = (const pm_f1_gpio_ctx_t *)ctx;
    const pm_f1_gpio_map_t *m;
    GPIO_InitTypeDef init;

    if ((c == NULL) || (cfg == NULL)) {
        return PM_ERR_NULL;
    }
    if (pin.id >= c->count) {
        return PM_ERR_RANGE;
    }

    m = &c->map[pin.id];
    if ((m->port == NULL) || (m->pin == 0u)) {
        return PM_ERR_PARAM;
    }

    gpio_clk_enable(m->port);

    init.Pin   = m->pin;
    init.Speed = gpio_speed_to_hal(cfg->speed);
    init.Pull  = gpio_pull_to_hal(cfg->pull);
    if (cfg->dir == PM_GPIO_DIR_OUT) {
        init.Mode = GPIO_MODE_OUTPUT_PP;
    } else {
        init.Mode = (cfg->pull == PM_GPIO_PULL_NONE) ? GPIO_MODE_INPUT
                                                     : GPIO_MODE_INPUT;
    }

    HAL_GPIO_Init(m->port, &init);

    if (cfg->dir == PM_GPIO_DIR_OUT) {
        HAL_GPIO_WritePin(m->port, m->pin,
                          (cfg->initial_level == PM_GPIO_LEVEL_HIGH) ? GPIO_PIN_SET
                                                                     : GPIO_PIN_RESET);
    }
    return PM_OK;
}

static pm_err_t f1_gpio_write(void *ctx, pm_gpio_pin_t pin, pm_gpio_level_t level)
{
    const pm_f1_gpio_ctx_t *c = (const pm_f1_gpio_ctx_t *)ctx;
    const pm_f1_gpio_map_t *m;

    if (c == NULL) {
        return PM_ERR_NULL;
    }
    if (pin.id >= c->count) {
        return PM_ERR_RANGE;
    }
    m = &c->map[pin.id];
    HAL_GPIO_WritePin(m->port, m->pin,
                      (level == PM_GPIO_LEVEL_HIGH) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return PM_OK;
}

static pm_err_t f1_gpio_read(void *ctx, pm_gpio_pin_t pin, pm_gpio_level_t *level)
{
    const pm_f1_gpio_ctx_t *c = (const pm_f1_gpio_ctx_t *)ctx;
    const pm_f1_gpio_map_t *m;

    if ((c == NULL) || (level == NULL)) {
        return PM_ERR_NULL;
    }
    if (pin.id >= c->count) {
        return PM_ERR_RANGE;
    }
    m = &c->map[pin.id];
    *level = (HAL_GPIO_ReadPin(m->port, m->pin) == GPIO_PIN_SET) ? PM_GPIO_LEVEL_HIGH
                                                                 : PM_GPIO_LEVEL_LOW;
    return PM_OK;
}

static const pm_gpio_ops_t s_gpio_ops = {
    .configure = f1_gpio_configure,
    .write     = f1_gpio_write,
    .read      = f1_gpio_read,
};

/* ------------------------------------------------------------------ UART */

#define PM_F1_UART_COUNT 3u

typedef struct {
    USART_TypeDef       *inst;
    IRQn_Type            irqn;
    UART_HandleTypeDef   huart;
    pm_uart_t           *owner;
    uint8_t              rx_byte;
    uint8_t              opened;
} pm_f1_uart_t;

static pm_f1_uart_t s_uart[PM_F1_UART_COUNT] = {
    { .inst = USART1, .irqn = USART1_IRQn },
    { .inst = USART2, .irqn = USART2_IRQn },
    { .inst = USART3, .irqn = USART3_IRQn },
};

static pm_f1_uart_t *uart_from_handle(UART_HandleTypeDef *huart)
{
    uint8_t i;

    for (i = 0u; i < PM_F1_UART_COUNT; i++) {
        if (&s_uart[i].huart == huart) {
            return &s_uart[i];
        }
    }
    return NULL;
}

static void uart_gpio_init(uint8_t idx)
{
    GPIO_InitTypeDef init;
    GPIO_TypeDef    *port;
    uint16_t         tx_pin;
    uint16_t         rx_pin;

    if (idx == 0u) {                          /* USART1: PA9 TX, PA10 RX */
        __HAL_RCC_GPIOA_CLK_ENABLE();
        port   = GPIOA;
        tx_pin = GPIO_PIN_9;
        rx_pin = GPIO_PIN_10;
    } else if (idx == 1u) {                   /* USART2: PA2 TX, PA3 RX */
        __HAL_RCC_GPIOA_CLK_ENABLE();
        port   = GPIOA;
        tx_pin = GPIO_PIN_2;
        rx_pin = GPIO_PIN_3;
    } else {                                  /* USART3: PB10 TX, PB11 RX */
        __HAL_RCC_GPIOB_CLK_ENABLE();
        port   = GPIOB;
        tx_pin = GPIO_PIN_10;
        rx_pin = GPIO_PIN_11;
    }

    /* TX: alternate-function push-pull. */
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    init.Pull  = GPIO_NOPULL;
    init.Pin   = tx_pin;
    init.Mode  = GPIO_MODE_AF_PP;
    HAL_GPIO_Init(port, &init);

    /* RX: plain input with pull-up. On STM32F1 an AF_PP RX pin keeps the
     * output driver active, which blocks the peer's TX line (RXNE never
     * gets set). Confirmed on hardware 2026-11-18. */
    init.Pull  = GPIO_PULLUP;
    init.Pin   = rx_pin;
    init.Mode  = GPIO_MODE_INPUT;
    HAL_GPIO_Init(port, &init);
}

static void uart_clk_enable(uint8_t idx)
{
    if (idx == 0u) {
        __HAL_RCC_USART1_CLK_ENABLE();
    } else if (idx == 1u) {
        __HAL_RCC_USART2_CLK_ENABLE();
    } else {
        __HAL_RCC_USART3_CLK_ENABLE();
    }
}

static uint32_t uart_parity_to_hal(pm_uart_parity_t parity)
{
    switch (parity) {
    case PM_UART_PARITY_EVEN: return UART_PARITY_EVEN;
    case PM_UART_PARITY_ODD:  return UART_PARITY_ODD;
    case PM_UART_PARITY_NONE:
    default:                  return UART_PARITY_NONE;
    }
}

static pm_err_t f1_uart_open(void *ctx, pm_uart_t *u, const pm_uart_cfg_t *cfg)
{
    pm_f1_uart_t *h;
    uint8_t idx;

    PM_UNUSED(ctx);

    if ((u == NULL) || (cfg == NULL)) {
        return PM_ERR_NULL;
    }
    idx = cfg->hw_id;
    if (idx >= PM_F1_UART_COUNT) {
        return PM_ERR_RANGE;
    }
    h = &s_uart[idx];
    if (h->opened != 0u) {
        return PM_ERR_BUSY;
    }

    uart_clk_enable(idx);
    uart_gpio_init(idx);

    h->huart.Instance          = h->inst;
    h->huart.Init.BaudRate     = cfg->baudrate;
    h->huart.Init.WordLength   = UART_WORDLENGTH_8B;
    h->huart.Init.StopBits     = (cfg->stop_bits == PM_UART_STOP_2) ? UART_STOPBITS_2
                                                                    : UART_STOPBITS_1;
    h->huart.Init.Parity       = uart_parity_to_hal(cfg->parity);
    h->huart.Init.Mode         = UART_MODE_TX_RX;
    h->huart.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    h->huart.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&h->huart) != HAL_OK) {
        return PM_ERR_IO;
    }

    HAL_NVIC_SetPriority(h->irqn, 1u, 0u);
    HAL_NVIC_EnableIRQ(h->irqn);

    h->owner  = u;
    h->opened = 1u;
    return PM_OK;
}

static void f1_uart_close(void *ctx, pm_uart_t *u)
{
    pm_f1_uart_t *h;
    uint8_t idx;

    PM_UNUSED(ctx);

    if (u == NULL) {
        return;
    }
    idx = u->hw_id;
    if (idx >= PM_F1_UART_COUNT) {
        return;
    }
    h = &s_uart[idx];

    HAL_NVIC_DisableIRQ(h->irqn);
    (void)HAL_UART_DeInit(&h->huart);
    h->owner  = NULL;
    h->opened = 0u;
}

static pm_err_t f1_uart_start_tx(void *ctx, pm_uart_t *u, const uint8_t *data, uint16_t len)
{
    pm_f1_uart_t *h;

    PM_UNUSED(ctx);

    if ((u == NULL) || (data == NULL) || (len == 0u)) {
        return PM_ERR_PARAM;
    }
    h = &s_uart[u->hw_id];
    return (HAL_UART_Transmit_IT(&h->huart, (uint8_t *)(uintptr_t)data, len) == HAL_OK)
               ? PM_OK
               : PM_ERR_BUSY;
}

static void f1_uart_enable_rx(void *ctx, pm_uart_t *u)
{
    pm_f1_uart_t *h;

    PM_UNUSED(ctx);

    if (u == NULL) {
        return;
    }
    h = &s_uart[u->hw_id];
    (void)HAL_UART_Receive_IT(&h->huart, &h->rx_byte, 1u);
}

static void f1_uart_disable_rx(void *ctx, pm_uart_t *u)
{
    pm_f1_uart_t *h;

    PM_UNUSED(ctx);

    if (u == NULL) {
        return;
    }
    h = &s_uart[u->hw_id];
    __HAL_UART_DISABLE_IT(&h->huart, UART_IT_RXNE);
}

static const pm_uart_ops_t s_uart_ops = {
    .open       = f1_uart_open,
    .close      = f1_uart_close,
    .start_tx   = f1_uart_start_tx,
    .enable_rx  = f1_uart_enable_rx,
    .disable_rx = f1_uart_disable_rx,
};

/* ------------------------------------------------------------- HAL events */

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    pm_f1_uart_t *h = uart_from_handle(huart);

    if (h == NULL) {
        return;
    }
    if (h->owner != NULL) {
        pm_uart_hw_rx_byte(h->owner, h->rx_byte);
    }
    (void)HAL_UART_Receive_IT(huart, &h->rx_byte, 1u);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    pm_f1_uart_t *h = uart_from_handle(huart);

    if ((h != NULL) && (h->owner != NULL)) {
        pm_uart_hw_tx_complete(h->owner);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    pm_f1_uart_t *h = uart_from_handle(huart);

    if (h == NULL) {
        return;
    }
    if (h->owner != NULL) {
        pm_uart_hw_error(h->owner, (uint16_t)huart->ErrorCode);
    }
    huart->ErrorCode = HAL_UART_ERROR_NONE;
    (void)HAL_UART_Receive_IT(huart, &h->rx_byte, 1u);
}

void USART1_IRQHandler(void)
{
    HAL_UART_IRQHandler(&s_uart[0].huart);
}

void USART2_IRQHandler(void)
{
    HAL_UART_IRQHandler(&s_uart[1].huart);
}

void USART3_IRQHandler(void)
{
    HAL_UART_IRQHandler(&s_uart[2].huart);
}

#if defined(PM_ENABLE_SPI)
/* ------------------------------------------------------------------- SPI */

#define PM_F1_SPI_COUNT 2u

typedef struct {
    SPI_TypeDef         *inst;
    IRQn_Type            irqn;
    SPI_HandleTypeDef    hspi;
    DMA_HandleTypeDef    hdma_rx;
    DMA_HandleTypeDef    hdma_tx;
    DMA_Channel_TypeDef *dma_rx_ch;
    DMA_Channel_TypeDef *dma_tx_ch;
    IRQn_Type            dma_rx_irqn;
    IRQn_Type            dma_tx_irqn;
    pm_spi_t            *owner;
    uint8_t              opened;
    uint8_t              dma;
} pm_f1_spi_t;

/* DMA request mapping for STM32F103 medium density (RM0008, table 78). */
static pm_f1_spi_t s_spi[PM_F1_SPI_COUNT] = {
    { .inst = SPI1, .irqn = SPI1_IRQn,
      .dma_rx_ch = DMA1_Channel2, .dma_tx_ch = DMA1_Channel3,
      .dma_rx_irqn = DMA1_Channel2_IRQn, .dma_tx_irqn = DMA1_Channel3_IRQn },
    { .inst = SPI2, .irqn = SPI2_IRQn,
      .dma_rx_ch = DMA1_Channel4, .dma_tx_ch = DMA1_Channel5,
      .dma_rx_irqn = DMA1_Channel4_IRQn, .dma_tx_irqn = DMA1_Channel5_IRQn },
};

static pm_f1_spi_t *spi_from_handle(SPI_HandleTypeDef *hspi)
{
    uint8_t i;

    for (i = 0u; i < PM_F1_SPI_COUNT; i++) {
        if (&s_spi[i].hspi == hspi) {
            return &s_spi[i];
        }
    }
    return NULL;
}

static void spi_clk_enable(uint8_t idx)
{
    if (idx == 0u) {
        __HAL_RCC_SPI1_CLK_ENABLE();
    } else {
        __HAL_RCC_SPI2_CLK_ENABLE();
    }
}

static uint32_t spi_bus_clock(uint8_t idx)
{
    return (idx == 0u) ? HAL_RCC_GetPCLK2Freq() : HAL_RCC_GetPCLK1Freq();
}

static uint32_t spi_prescaler_code(uint32_t div)
{
    switch (div) {
    case 2u:   return SPI_BAUDRATEPRESCALER_2;
    case 4u:   return SPI_BAUDRATEPRESCALER_4;
    case 8u:   return SPI_BAUDRATEPRESCALER_8;
    case 16u:  return SPI_BAUDRATEPRESCALER_16;
    case 32u:  return SPI_BAUDRATEPRESCALER_32;
    case 64u:  return SPI_BAUDRATEPRESCALER_64;
    case 128u: return SPI_BAUDRATEPRESCALER_128;
    case 256u:
    default:   return SPI_BAUDRATEPRESCALER_256;
    }
}

static void spi_gpio_init(const pm_f1_spi_pins_t *pins)
{
    GPIO_InitTypeDef init;

    gpio_clk_enable(pins->sck_port);
    gpio_clk_enable(pins->miso_port);
    gpio_clk_enable(pins->mosi_port);

    /* SCK / MOSI：主机输出，复用推挽。 */
    init.Mode  = GPIO_MODE_AF_PP;
    init.Pull  = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_HIGH;

    init.Pin = pins->sck_pin;
    HAL_GPIO_Init(pins->sck_port, &init);
    init.Pin = pins->mosi_pin;
    HAL_GPIO_Init(pins->mosi_port, &init);

    /* MISO：主机模式下由从机驱动，必须是输入。配成 AF_PP 会让主机输出级
       顶住线路（与 UART RX 同类错误）。RM0008 主机模式允许输入浮空或上拉，
       这里取上拉，未接从机时引脚不会浮空。 */
    init.Mode  = GPIO_MODE_INPUT;
    init.Pull  = GPIO_PULLUP;
    init.Speed = GPIO_SPEED_FREQ_HIGH;

    init.Pin = pins->miso_pin;
    HAL_GPIO_Init(pins->miso_port, &init);
}

static pm_err_t spi_dma_init(pm_f1_spi_t *h)
{
    __HAL_RCC_DMA1_CLK_ENABLE();

    h->hdma_rx.Instance                 = h->dma_rx_ch;
    h->hdma_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    h->hdma_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
    h->hdma_rx.Init.MemInc              = DMA_MINC_ENABLE;
    h->hdma_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    h->hdma_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    h->hdma_rx.Init.Mode                = DMA_NORMAL;
    h->hdma_rx.Init.Priority            = DMA_PRIORITY_HIGH;
    if (HAL_DMA_Init(&h->hdma_rx) != HAL_OK) {
        return PM_ERR_IO;
    }
    __HAL_LINKDMA(&h->hspi, hdmarx, h->hdma_rx);

    h->hdma_tx.Instance                 = h->dma_tx_ch;
    h->hdma_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    h->hdma_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
    h->hdma_tx.Init.MemInc              = DMA_MINC_ENABLE;
    h->hdma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    h->hdma_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    h->hdma_tx.Init.Mode                = DMA_NORMAL;
    h->hdma_tx.Init.Priority            = DMA_PRIORITY_HIGH;
    if (HAL_DMA_Init(&h->hdma_tx) != HAL_OK) {
        (void)HAL_DMA_DeInit(&h->hdma_rx);
        return PM_ERR_IO;
    }
    __HAL_LINKDMA(&h->hspi, hdmatx, h->hdma_tx);

    HAL_NVIC_SetPriority(h->dma_rx_irqn, 2u, 0u);
    HAL_NVIC_EnableIRQ(h->dma_rx_irqn);
    HAL_NVIC_SetPriority(h->dma_tx_irqn, 2u, 0u);
    HAL_NVIC_EnableIRQ(h->dma_tx_irqn);
    return PM_OK;
}

static void spi_dma_deinit(pm_f1_spi_t *h)
{
    HAL_NVIC_DisableIRQ(h->dma_rx_irqn);
    HAL_NVIC_DisableIRQ(h->dma_tx_irqn);
    (void)HAL_DMA_DeInit(&h->hdma_tx);
    (void)HAL_DMA_DeInit(&h->hdma_rx);
}

#if defined(PM_ENABLE_TRACK)
/* Defined in the track section below; used by f1_spi_open() to refuse a pin
 * that a tracking sensor already owns. */
static bool track_uses_spi(uint8_t spi_id);
#endif

static pm_err_t f1_spi_open(void *ctx, pm_spi_t *s, const pm_spi_cfg_t *cfg,
                            uint32_t *actual_hz)
{
    const pm_f1_spi_ctx_t *c = (const pm_f1_spi_ctx_t *)ctx;
    const pm_f1_spi_pins_t *pins;
    pm_f1_spi_t *h;
    uint32_t pclk;
    uint32_t div = 2u;

    if ((c == NULL) || (s == NULL) || (cfg == NULL) || (actual_hz == NULL)) {
        return PM_ERR_NULL;
    }
    if ((cfg->hw_id >= PM_F1_SPI_COUNT) || (cfg->hw_id >= c->count)) {
        return PM_ERR_RANGE;
    }

    h = &s_spi[cfg->hw_id];
    if (h->opened != 0u) {
        return PM_ERR_BUSY;
    }
#if defined(PM_ENABLE_TRACK)
    if (track_uses_spi(cfg->hw_id)) {
        /* A tracking sensor owns one of this instance's pins. */
        return PM_ERR_CONFLICT;
    }
#endif

    pins = &c->pins[cfg->hw_id];
    if ((pins->sck_port == NULL) || (pins->miso_port == NULL) ||
        (pins->mosi_port == NULL)) {
        return PM_ERR_PARAM;
    }
    if (cfg->nss != PM_SPI_NSS_SOFT) {
        /* Both hardware NSS pins are taken on this board (PA4 NTC, PB12 KEY1). */
        return PM_ERR_PARAM;
    }

    spi_clk_enable(cfg->hw_id);
    spi_gpio_init(pins);

    pclk = spi_bus_clock(cfg->hw_id);
    while ((div < 256u) && ((pclk / div) > cfg->clock_hz)) {
        div <<= 1u;
    }

    h->hspi.Instance               = h->inst;
    h->hspi.Init.Mode              = SPI_MODE_MASTER;
    h->hspi.Init.Direction         = SPI_DIRECTION_2LINES;
    h->hspi.Init.DataSize          = SPI_DATASIZE_8BIT;
    h->hspi.Init.CLKPolarity       = ((cfg->mode == PM_SPI_MODE_2) ||
                                      (cfg->mode == PM_SPI_MODE_3))
                                         ? SPI_POLARITY_HIGH
                                         : SPI_POLARITY_LOW;
    h->hspi.Init.CLKPhase          = ((cfg->mode == PM_SPI_MODE_1) ||
                                      (cfg->mode == PM_SPI_MODE_3))
                                         ? SPI_PHASE_2EDGE
                                         : SPI_PHASE_1EDGE;
    h->hspi.Init.NSS               = SPI_NSS_SOFT;
    h->hspi.Init.BaudRatePrescaler = spi_prescaler_code(div);
    h->hspi.Init.FirstBit          = (cfg->bit_order == PM_SPI_LSB_FIRST)
                                         ? SPI_FIRSTBIT_LSB
                                         : SPI_FIRSTBIT_MSB;
    h->hspi.Init.TIMode            = SPI_TIMODE_DISABLE;
    h->hspi.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    h->hspi.Init.CRCPolynomial     = 7u;

    if (HAL_SPI_Init(&h->hspi) != HAL_OK) {
        (void)HAL_SPI_DeInit(&h->hspi);
        return PM_ERR_IO;
    }

    if (cfg->use_dma) {
        if (spi_dma_init(h) != PM_OK) {
            (void)HAL_SPI_DeInit(&h->hspi);
            return PM_ERR_IO;
        }
        h->dma = 1u;
    }

    HAL_NVIC_SetPriority(h->irqn, 2u, 0u);
    HAL_NVIC_EnableIRQ(h->irqn);

    h->owner  = s;
    h->opened = 1u;
    *actual_hz = pclk / div;
    return PM_OK;
}

static void f1_spi_close(void *ctx, pm_spi_t *s)
{
    pm_f1_spi_t *h;

    PM_UNUSED(ctx);

    if (s == NULL) {
        return;
    }
    if (s->hw_id >= PM_F1_SPI_COUNT) {
        return;
    }
    h = &s_spi[s->hw_id];

    HAL_NVIC_DisableIRQ(h->irqn);
    if (h->dma != 0u) {
        spi_dma_deinit(h);
    }
    (void)HAL_SPI_DeInit(&h->hspi);

    h->owner  = NULL;
    h->opened = 0u;
    h->dma    = 0u;
}

static pm_err_t f1_spi_start(void *ctx, pm_spi_t *s, const uint8_t *tx,
                             uint8_t *rx, uint16_t len)
{
    pm_f1_spi_t *h;
    HAL_StatusTypeDef st;
    uint8_t *tx_mut;

    PM_UNUSED(ctx);

    if ((s == NULL) || (tx == NULL) || (rx == NULL) || (len == 0u)) {
        return PM_ERR_PARAM;
    }
    if (s->hw_id >= PM_F1_SPI_COUNT) {
        return PM_ERR_RANGE;
    }
    h = &s_spi[s->hw_id];

    /* HAL takes a non-const TX pointer but only reads from it. */
    tx_mut = (uint8_t *)(uintptr_t)tx;

    if (h->dma != 0u) {
        st = HAL_SPI_TransmitReceive_DMA(&h->hspi, tx_mut, rx, len);
    } else {
        st = HAL_SPI_TransmitReceive_IT(&h->hspi, tx_mut, rx, len);
    }
    if (st == HAL_OK) {
        return PM_OK;
    }
    return (st == HAL_BUSY) ? PM_ERR_BUSY : PM_ERR_IO;
}

/* Bytes already clocked, read from the HAL/DMA counters before an abort. */
static uint16_t spi_clocked(const pm_f1_spi_t *h)
{
    uint16_t total = h->hspi.RxXferSize;
    uint16_t remaining;

    if (h->dma != 0u) {
        remaining = (uint16_t)__HAL_DMA_GET_COUNTER(&h->hdma_rx);
    } else {
        remaining = h->hspi.RxXferCount;
    }
    return (remaining <= total) ? (uint16_t)(total - remaining) : total;
}

static pm_err_t f1_spi_abort(void *ctx, pm_spi_t *s, uint16_t *done)
{
    pm_f1_spi_t *h;

    PM_UNUSED(ctx);

    if ((s == NULL) || (done == NULL)) {
        return PM_ERR_NULL;
    }
    if (s->hw_id >= PM_F1_SPI_COUNT) {
        return PM_ERR_RANGE;
    }
    h = &s_spi[s->hw_id];

    *done = spi_clocked(h);
    if (HAL_SPI_Abort(&h->hspi) != HAL_OK) {
        *done = 0u;
        return PM_ERR_IO;
    }
    return PM_OK;
}

static const pm_spi_ops_t s_spi_ops = {
    .open  = f1_spi_open,
    .close = f1_spi_close,
    .start = f1_spi_start,
    .abort = f1_spi_abort,
};

/* ------------------------------------------------------------- HAL events */

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    pm_f1_spi_t *h = spi_from_handle(hspi);

    if ((h != NULL) && (h->owner != NULL)) {
        pm_spi_hw_done(h->owner, hspi->RxXferSize);
    }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    pm_f1_spi_t *h = spi_from_handle(hspi);

    if (h == NULL) {
        return;
    }
    if (h->owner != NULL) {
        pm_spi_hw_error(h->owner, (uint16_t)hspi->ErrorCode, spi_clocked(h));
    }
}

void SPI1_IRQHandler(void)
{
    HAL_SPI_IRQHandler(&s_spi[0].hspi);
}

void SPI2_IRQHandler(void)
{
    HAL_SPI_IRQHandler(&s_spi[1].hspi);
}

void DMA1_Channel2_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_spi[0].hdma_rx);
}

void DMA1_Channel3_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_spi[0].hdma_tx);
}

void DMA1_Channel4_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_spi[1].hdma_rx);
}

void DMA1_Channel5_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_spi[1].hdma_tx);
}

#endif /* PM_ENABLE_SPI */

#if defined(PM_ENABLE_I2C)
/* ------------------------------------------------------------------- I2C */

#define PM_F1_I2C_COUNT 1u

/* Backend-local error bit used when the read phase of a write_read cannot be
 * started from the TX-complete callback. It is not a HAL error code. */
#define PM_F1_I2C_LOCAL_ERR 0x8000u

typedef struct {
    I2C_TypeDef       *inst;
    IRQn_Type          ev_irqn;
    IRQn_Type          er_irqn;
    I2C_HandleTypeDef  hi2c;
    pm_i2c_t          *owner;
    const uint8_t     *wr_buf;
    uint8_t           *rd_buf;
    uint16_t           wr_len;
    uint16_t           rd_len;
    uint8_t            addr8;      /* device address shifted left by one */
    uint8_t            opened;
    uint8_t            phase;      /* 0 = write, 1 = read */
} pm_f1_i2c_t;

static pm_f1_i2c_t s_i2c[PM_F1_I2C_COUNT] = {
    { .inst = I2C1, .ev_irqn = I2C1_EV_IRQn, .er_irqn = I2C1_ER_IRQn },
};

static pm_f1_i2c_t *i2c_from_handle(I2C_HandleTypeDef *hi2c)
{
    uint8_t i;

    for (i = 0u; i < PM_F1_I2C_COUNT; i++) {
        if (&s_i2c[i].hi2c == hi2c) {
            return &s_i2c[i];
        }
    }
    return NULL;
}

static void i2c_clk_enable(uint8_t idx)
{
    if (idx == 0u) {
        __HAL_RCC_I2C1_CLK_ENABLE();
    } else {
        __HAL_RCC_I2C2_CLK_ENABLE();
    }
}

static void i2c_gpio_init(const pm_f1_i2c_pins_t *pins)
{
    GPIO_InitTypeDef init;

    gpio_clk_enable(pins->scl_port);
    gpio_clk_enable(pins->sda_port);

    /* Open drain, alternate function. The internal pull-up is weak compared
     * to a proper bus pull-up; modules on this board carry their own. */
    init.Mode  = GPIO_MODE_AF_OD;
    init.Pull  = GPIO_PULLUP;
    init.Speed = GPIO_SPEED_FREQ_HIGH;

    init.Pin = pins->scl_pin;
    HAL_GPIO_Init(pins->scl_port, &init);
    init.Pin = pins->sda_pin;
    HAL_GPIO_Init(pins->sda_port, &init);
}

/* Bytes moved in the current transaction, read before the peripheral is
 * reset. During the read phase the write phase is already complete. */
static uint16_t i2c_progress(const pm_f1_i2c_t *h)
{
    uint16_t cur = 0u;

    if (h->hi2c.XferSize > h->hi2c.XferCount) {
        cur = (uint16_t)(h->hi2c.XferSize - h->hi2c.XferCount);
    }
    if (h->phase == 0u) {
        return cur;
    }
    return (uint16_t)(h->wr_len + cur);
}

/* SCL as actually programmed in CCR, not as requested. Standard mode:
 * SCL = PCLK1 / (2 * CCR); fast mode with DUTY_2: PCLK1 / (3 * CCR). */
static uint32_t i2c_actual_hz(const pm_f1_i2c_t *h)
{
    uint32_t ccr = (uint32_t)(h->inst->CCR & I2C_CCR_CCR);
    uint32_t pclk = HAL_RCC_GetPCLK1Freq();

    if (ccr == 0u) {
        return 0u;
    }
    if (h->hi2c.Init.ClockSpeed > 100000u) {
        return pclk / (3u * ccr);
    }
    return pclk / (2u * ccr);
}

static pm_err_t f1_i2c_open(void *ctx, pm_i2c_t *i, const pm_i2c_cfg_t *cfg,
                            uint32_t *actual_hz)
{
    const pm_f1_i2c_ctx_t *c = (const pm_f1_i2c_ctx_t *)ctx;
    const pm_f1_i2c_pins_t *pins;
    pm_f1_i2c_t *h;

    if ((c == NULL) || (i == NULL) || (cfg == NULL) || (actual_hz == NULL)) {
        return PM_ERR_NULL;
    }
    if ((cfg->hw_id >= PM_F1_I2C_COUNT) || (cfg->hw_id >= c->count)) {
        return PM_ERR_RANGE;
    }

    h = &s_i2c[cfg->hw_id];
    if (h->opened != 0u) {
        return PM_ERR_BUSY;
    }

    pins = &c->pins[cfg->hw_id];
    if ((pins->scl_port == NULL) || (pins->sda_port == NULL)) {
        return PM_ERR_PARAM;
    }

    i2c_clk_enable(cfg->hw_id);
    i2c_gpio_init(pins);

    h->hi2c.Instance             = h->inst;
    h->hi2c.Init.ClockSpeed      = cfg->clock_hz;
    h->hi2c.Init.DutyCycle       = I2C_DUTYCYCLE_2;
    h->hi2c.Init.OwnAddress1     = 0u;      /* master only */
    h->hi2c.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    h->hi2c.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    h->hi2c.Init.OwnAddress2     = 0u;
    h->hi2c.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    h->hi2c.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;

    if (HAL_I2C_Init(&h->hi2c) != HAL_OK) {
        (void)HAL_I2C_DeInit(&h->hi2c);
        return PM_ERR_IO;
    }

    HAL_NVIC_SetPriority(h->ev_irqn, 2u, 0u);
    HAL_NVIC_SetPriority(h->er_irqn, 1u, 0u);
    HAL_NVIC_EnableIRQ(h->ev_irqn);
    HAL_NVIC_EnableIRQ(h->er_irqn);

    h->owner  = i;
    h->opened = 1u;
    h->phase  = 0u;
    *actual_hz = i2c_actual_hz(h);
    if (*actual_hz == 0u) {
        *actual_hz = cfg->clock_hz;
    }
    return PM_OK;
}

static void f1_i2c_close(void *ctx, pm_i2c_t *i)
{
    PM_UNUSED(ctx);

    if ((i == NULL) || (i->hw_id >= PM_F1_I2C_COUNT)) {
        return;
    }

    {
        pm_f1_i2c_t *h = &s_i2c[i->hw_id];

        HAL_NVIC_DisableIRQ(h->ev_irqn);
        HAL_NVIC_DisableIRQ(h->er_irqn);
        (void)HAL_I2C_DeInit(&h->hi2c);

        h->owner  = NULL;
        h->opened = 0u;
        h->phase  = 0u;
        h->wr_buf = NULL;
        h->rd_buf = NULL;
        h->wr_len = 0u;
        h->rd_len = 0u;
    }
}

static pm_err_t i2c_start_result(HAL_StatusTypeDef st)
{
    if (st == HAL_OK) {
        return PM_OK;
    }
    return (st == HAL_BUSY) ? PM_ERR_BUSY : PM_ERR_IO;
}

static pm_err_t f1_i2c_start_write(void *ctx, pm_i2c_t *i, uint8_t addr7,
                                   const uint8_t *data, uint16_t len)
{
    pm_f1_i2c_t *h;

    PM_UNUSED(ctx);

    if ((i == NULL) || (data == NULL)) {
        return PM_ERR_NULL;
    }
    if (i->hw_id >= PM_F1_I2C_COUNT) {
        return PM_ERR_RANGE;
    }
    h = &s_i2c[i->hw_id];

    h->addr8  = (uint8_t)(addr7 << 1);
    h->wr_buf = data;
    h->rd_buf = NULL;
    h->wr_len = len;
    h->rd_len = 0u;
    h->phase  = 0u;

    return i2c_start_result(HAL_I2C_Master_Transmit_IT(
        &h->hi2c, h->addr8, (uint8_t *)(uintptr_t)data, len));
}

static pm_err_t f1_i2c_start_read(void *ctx, pm_i2c_t *i, uint8_t addr7,
                                  uint8_t *data, uint16_t len)
{
    pm_f1_i2c_t *h;

    PM_UNUSED(ctx);

    if ((i == NULL) || (data == NULL)) {
        return PM_ERR_NULL;
    }
    if (i->hw_id >= PM_F1_I2C_COUNT) {
        return PM_ERR_RANGE;
    }
    h = &s_i2c[i->hw_id];

    h->addr8  = (uint8_t)(addr7 << 1);
    h->wr_buf = NULL;
    h->rd_buf = data;
    h->wr_len = 0u;
    h->rd_len = len;
    h->phase  = 1u;

    return i2c_start_result(
        HAL_I2C_Master_Receive_IT(&h->hi2c, h->addr8, data, len));
}

static pm_err_t f1_i2c_start_write_read(void *ctx, pm_i2c_t *i, uint8_t addr7,
                                        const uint8_t *wr, uint16_t wr_len,
                                        uint8_t *rd, uint16_t rd_len)
{
    pm_f1_i2c_t *h;

    PM_UNUSED(ctx);

    if ((i == NULL) || (wr == NULL) || (rd == NULL)) {
        return PM_ERR_NULL;
    }
    if (i->hw_id >= PM_F1_I2C_COUNT) {
        return PM_ERR_RANGE;
    }
    h = &s_i2c[i->hw_id];

    h->addr8  = (uint8_t)(addr7 << 1);
    h->wr_buf = wr;
    h->rd_buf = rd;
    h->wr_len = wr_len;
    h->rd_len = rd_len;
    h->phase  = 0u;

    /* No STOP after the write: the read phase continues with a repeated
     * START from the TX-complete callback. */
    return i2c_start_result(HAL_I2C_Master_Seq_Transmit_IT(
        &h->hi2c, h->addr8, (uint8_t *)(uintptr_t)wr, wr_len,
        I2C_FIRST_FRAME));
}

/* Resets the peripheral instead of issuing an asynchronous abort: HAL's
 * abort path itself waits for the bus, and the model needs a bounded return.
 * The price is that a slave holding SDA low is not cleared here. */
static pm_err_t f1_i2c_abort(void *ctx, pm_i2c_t *i, uint16_t *done)
{
    pm_f1_i2c_t *h;
    uint16_t moved;

    PM_UNUSED(ctx);

    if ((i == NULL) || (done == NULL)) {
        return PM_ERR_NULL;
    }
    if (i->hw_id >= PM_F1_I2C_COUNT) {
        return PM_ERR_RANGE;
    }
    h = &s_i2c[i->hw_id];

    moved = i2c_progress(h);

    HAL_NVIC_DisableIRQ(h->ev_irqn);
    HAL_NVIC_DisableIRQ(h->er_irqn);
    (void)HAL_I2C_DeInit(&h->hi2c);
    (void)HAL_I2C_Init(&h->hi2c);
    HAL_NVIC_EnableIRQ(h->ev_irqn);
    HAL_NVIC_EnableIRQ(h->er_irqn);

    h->phase  = 0u;
    h->wr_buf = NULL;
    h->rd_buf = NULL;
    h->wr_len = 0u;
    h->rd_len = 0u;

    *done = moved;
    return PM_OK;
}

static const pm_i2c_ops_t s_i2c_ops = {
    .open            = f1_i2c_open,
    .close           = f1_i2c_close,
    .start_write     = f1_i2c_start_write,
    .start_read      = f1_i2c_start_read,
    .start_write_read = f1_i2c_start_write_read,
    .abort           = f1_i2c_abort,
};

void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    pm_f1_i2c_t *h = i2c_from_handle(hi2c);

    if ((h == NULL) || (h->owner == NULL)) {
        return;
    }

    if ((h->rd_len != 0u) && (h->rd_buf != NULL) && (h->phase == 0u)) {
        /* write_read: repeated START, then read until STOP. */
        if (HAL_I2C_Master_Seq_Receive_IT(hi2c, h->addr8, h->rd_buf,
                                          h->rd_len,
                                          I2C_LAST_FRAME) == HAL_OK) {
            h->phase = 1u;
            return;
        }
        pm_i2c_hw_error(h->owner, PM_F1_I2C_LOCAL_ERR, h->wr_len);
        return;
    }

    pm_i2c_hw_done(h->owner, h->wr_len, 0u);
}

void HAL_I2C_MasterRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    pm_f1_i2c_t *h = i2c_from_handle(hi2c);

    if ((h != NULL) && (h->owner != NULL)) {
        pm_i2c_hw_done(h->owner, h->wr_len, h->rd_len);
    }
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    pm_f1_i2c_t *h = i2c_from_handle(hi2c);

    if ((h != NULL) && (h->owner != NULL)) {
        pm_i2c_hw_error(h->owner, (uint16_t)hi2c->ErrorCode,
                        i2c_progress(h));
    }
}

void I2C1_EV_IRQHandler(void)
{
    HAL_I2C_EV_IRQHandler(&s_i2c[0].hi2c);
}

void I2C1_ER_IRQHandler(void)
{
    HAL_I2C_ER_IRQHandler(&s_i2c[0].hi2c);
}

#endif /* PM_ENABLE_I2C */

#if defined(PM_ENABLE_ADC)
/* ------------------------------------------------------------------- ADC */

#define PM_F1_ADC_COUNT 1u

typedef struct {
    ADC_TypeDef       *inst;
    IRQn_Type          irqn;
    ADC_HandleTypeDef  hadc;
    pm_adc_t          *owner;
    uint8_t            opened;
} pm_f1_adc_t;

static pm_f1_adc_t s_adc[PM_F1_ADC_COUNT] = {
    { .inst = ADC1, .irqn = ADC1_2_IRQn },
};

static pm_f1_adc_t *adc_from_handle(ADC_HandleTypeDef *hadc)
{
    uint8_t i;

    for (i = 0u; i < PM_F1_ADC_COUNT; i++) {
        if (&s_adc[i].hadc == hadc) {
            return &s_adc[i];
        }
    }
    return NULL;
}

static uint32_t adc_sample_to_hal(pm_adc_sample_t sample)
{
    switch (sample) {
    case PM_ADC_SAMPLE_1CYCLE_5:    return ADC_SAMPLETIME_1CYCLE_5;
    case PM_ADC_SAMPLE_7CYCLES_5:   return ADC_SAMPLETIME_7CYCLES_5;
    case PM_ADC_SAMPLE_13CYCLES_5:  return ADC_SAMPLETIME_13CYCLES_5;
    case PM_ADC_SAMPLE_28CYCLES_5:  return ADC_SAMPLETIME_28CYCLES_5;
    case PM_ADC_SAMPLE_41CYCLES_5:  return ADC_SAMPLETIME_41CYCLES_5;
    case PM_ADC_SAMPLE_55CYCLES_5:  return ADC_SAMPLETIME_55CYCLES_5;
    case PM_ADC_SAMPLE_71CYCLES_5:  return ADC_SAMPLETIME_71CYCLES_5;
    case PM_ADC_SAMPLE_239CYCLES_5:
    default:                        return ADC_SAMPLETIME_239CYCLES_5;
    }
}

static void adc_gpio_init(const pm_f1_adc_channel_t *ch)
{
    GPIO_InitTypeDef init;

    gpio_clk_enable(ch->port);

    init.Pin   = ch->pin;
    init.Mode  = GPIO_MODE_ANALOG;
    init.Pull  = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(ch->port, &init);
}

/* ADCCLK must stay at or below 14 MHz. With PCLK2 = 72 MHz, /6 gives 12 MHz. */
static void adc_clock_config(void)
{
    RCC_PeriphCLKInitTypeDef pclk = {0};

    pclk.PeriphClockSelection = RCC_PERIPHCLK_ADC;
    pclk.AdcClockSelection    = RCC_ADCPCLK2_DIV6;
    (void)HAL_RCCEx_PeriphCLKConfig(&pclk);
}

static pm_err_t f1_adc_open(void *ctx, pm_adc_t *a, const pm_adc_cfg_t *cfg)
{
    const pm_f1_adc_ctx_t     *c = (const pm_f1_adc_ctx_t *)ctx;
    const pm_f1_adc_channel_t *ch;
    pm_f1_adc_t               *h;
    ADC_ChannelConfTypeDef     chcfg;

    if ((c == NULL) || (a == NULL) || (cfg == NULL)) {
        return PM_ERR_NULL;
    }
    if ((cfg->hw_id >= PM_F1_ADC_COUNT) || (cfg->hw_id >= c->count)) {
        return PM_ERR_RANGE;
    }
    if (cfg->channel >= c->count) {
        return PM_ERR_RANGE;
    }

    h = &s_adc[cfg->hw_id];
    if (h->opened != 0u) {
        return PM_ERR_BUSY;
    }

    ch = &c->channels[cfg->channel];
    if ((ch->port == NULL) || (ch->pin == 0u)) {
        return PM_ERR_PARAM;
    }

    __HAL_RCC_ADC1_CLK_ENABLE();
    adc_clock_config();
    adc_gpio_init(ch);

    h->hadc.Instance                   = h->inst;
    h->hadc.Init.ScanConvMode          = ADC_SCAN_DISABLE;
    h->hadc.Init.ContinuousConvMode    = DISABLE;
    h->hadc.Init.DiscontinuousConvMode = DISABLE;
    h->hadc.Init.NbrOfDiscConversion   = 0u;
    h->hadc.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    h->hadc.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    h->hadc.Init.NbrOfConversion       = 1u;

    if (HAL_ADC_Init(&h->hadc) != HAL_OK) {
        return PM_ERR_IO;
    }

    chcfg.Channel      = ch->hal_channel;
    chcfg.Rank         = ADC_REGULAR_RANK_1;
    chcfg.SamplingTime = adc_sample_to_hal(cfg->sample_time);
    if (HAL_ADC_ConfigChannel(&h->hadc, &chcfg) != HAL_OK) {
        (void)HAL_ADC_DeInit(&h->hadc);
        return PM_ERR_IO;
    }

    /* F1 requires a calibration after every power-up / re-init. */
    if (HAL_ADCEx_Calibration_Start(&h->hadc) != HAL_OK) {
        (void)HAL_ADC_DeInit(&h->hadc);
        return PM_ERR_IO;
    }

    HAL_NVIC_SetPriority(h->irqn, 2u, 0u);
    HAL_NVIC_EnableIRQ(h->irqn);

    h->owner  = a;
    h->opened = 1u;
    return PM_OK;
}

static void f1_adc_close(void *ctx, pm_adc_t *a)
{
    pm_f1_adc_t *h;

    PM_UNUSED(ctx);

    if ((a == NULL) || (a->hw_id >= PM_F1_ADC_COUNT)) {
        return;
    }
    h = &s_adc[a->hw_id];

    HAL_NVIC_DisableIRQ(h->irqn);
    (void)HAL_ADC_Stop_IT(&h->hadc);
    (void)HAL_ADC_DeInit(&h->hadc);

    h->owner  = NULL;
    h->opened = 0u;
}

static pm_err_t f1_adc_start(void *ctx, pm_adc_t *a)
{
    PM_UNUSED(ctx);

    if ((a == NULL) || (a->hw_id >= PM_F1_ADC_COUNT)) {
        return PM_ERR_RANGE;
    }
    return (HAL_ADC_Start_IT(&s_adc[a->hw_id].hadc) == HAL_OK) ? PM_OK
                                                               : PM_ERR_BUSY;
}

static pm_err_t f1_adc_abort(void *ctx, pm_adc_t *a)
{
    PM_UNUSED(ctx);

    if ((a == NULL) || (a->hw_id >= PM_F1_ADC_COUNT)) {
        return PM_ERR_RANGE;
    }
    return (HAL_ADC_Stop_IT(&s_adc[a->hw_id].hadc) == HAL_OK) ? PM_OK
                                                              : PM_ERR_IO;
}

static const pm_adc_ops_t s_adc_ops = {
    .open  = f1_adc_open,
    .close = f1_adc_close,
    .start = f1_adc_start,
    .abort = f1_adc_abort,
};

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    pm_f1_adc_t *h = adc_from_handle(hadc);

    if ((h != NULL) && (h->owner != NULL)) {
        pm_adc_hw_done(h->owner, (uint16_t)HAL_ADC_GetValue(hadc));
    }
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
    pm_f1_adc_t *h = adc_from_handle(hadc);

    if ((h != NULL) && (h->owner != NULL)) {
        pm_adc_hw_error(h->owner, (uint16_t)hadc->ErrorCode);
    }
}

void ADC1_2_IRQHandler(void)
{
    HAL_ADC_IRQHandler(&s_adc[0].hadc);
}

#endif /* PM_ENABLE_ADC */

/* APB1 timer clock doubles when the APB1 prescaler is not 1. Shared by every
 * module that owns a TIMx on this chip (PWM, tone). */
static uint32_t f1_apb1_timer_clock(void)
{
    uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();

    return (((RCC->CFGR & RCC_CFGR_PPRE1) == RCC_CFGR_PPRE1_DIV1) ? pclk1
                                                                  : (pclk1 * 2u));
}

#if defined(PM_ENABLE_PWM) || defined(PM_ENABLE_TONE)
/* Non-model firmware on this board drives timers directly: the echo reader in
 * main.c polls TIM2->CNT while the motor runs, and a CubeMX-style init may have
 * remapped a timer to other pins. Both leave a mark that needs no cooperation
 * from the other driver: a running counter (CR1.CEN) or a non-zero AFIO remap.
 * Reading registers only; the probe never writes.
 * timer_index follows board_timer_id_t and equals the timer number:
 * 2 = TIM2, 3 = TIM3, 4 = TIM4. */
static bool f1_timer_externally_busy(void *ctx, uint8_t hw_id)
{
    TIM_TypeDef *inst;
    uint32_t     remap_mask;

    (void)ctx;
    switch (hw_id) {
    case 2u:
        inst       = TIM2;
        remap_mask = 0x00000300u;   /* AFIO_MAPR TIM2_REMAP[1:0] */
        break;
    case 3u:
        inst       = TIM3;
        remap_mask = 0x00000C00u;   /* AFIO_MAPR TIM3_REMAP[1:0] */
        break;
    case 4u:
        inst       = TIM4;
        remap_mask = 0x00001000u;   /* AFIO_MAPR TIM4_REMAP */
        break;
    default:
        return true;                /* unknown timer: never take it over */
    }

    if ((inst->CR1 & TIM_CR1_CEN) != 0u) {
        return true;
    }
    return (AFIO->MAPR & remap_mask) != 0u;
}
#endif

#if defined(PM_ENABLE_PWM)
/* ------------------------------------------------------------------- PWM */

#define PM_F1_PWM_COUNT       3u
#define PM_F1_TIMER_FIRST     2u      /* board_timer_id_t value of TIM2 */
#define PM_F1_PWM_TIMER_COUNT 3u
#define PM_F1_PWM_NO_CHANNEL  0xFFu

typedef struct {
    TIM_TypeDef      *inst;
    TIM_HandleTypeDef htim;
    uint8_t           inited;
    uint8_t           open_mask;   /* bit n = TIM_CHANNEL_(n+1) is open */
    uint32_t          freq_req;    /* requested period shared by the channels */
    uint16_t          psc;         /* PSC register value (division factor - 1) */
    uint16_t          arr;         /* ARR register value (period ticks - 1) */
} pm_f1_pwm_timer_t;

typedef struct {
    uint8_t  opened;
    uint8_t  enabled;
    uint8_t  ch_index;
    uint16_t permille;
} pm_f1_pwm_ch_t;

/* timer_index uses board_timer_id_t, i.e. the timer number: 2/3/4 = TIM2/3/4.
 * The array is dense, so index it by (timer_index - PM_F1_TIMER_FIRST). */
static pm_f1_pwm_timer_t s_pwm_timer[PM_F1_PWM_TIMER_COUNT] = {
    { .inst = TIM2 },
    { .inst = TIM3 },
    { .inst = TIM4 },
};

static pm_f1_pwm_ch_t s_pwm[PM_F1_PWM_COUNT];

#if defined(PM_ENABLE_TONE)
/* Defined next to pwm_uses_gpio(); declared here because PWM opens first. */
static bool tone_uses_gpio(GPIO_TypeDef *port, uint16_t pin);
#endif

static uint8_t pwm_channel_index(uint32_t channel)
{
    switch (channel) {
    case TIM_CHANNEL_1: return 0u;
    case TIM_CHANNEL_2: return 1u;
    case TIM_CHANNEL_3: return 2u;
    case TIM_CHANNEL_4: return 3u;
    default:            return PM_F1_PWM_NO_CHANNEL;
    }
}

static void pwm_timer_clk_enable(TIM_TypeDef *inst)
{
    if (inst == TIM2) {
        __HAL_RCC_TIM2_CLK_ENABLE();
    } else if (inst == TIM3) {
        __HAL_RCC_TIM3_CLK_ENABLE();
    } else if (inst == TIM4) {
        __HAL_RCC_TIM4_CLK_ENABLE();
    }
}

static uint32_t pwm_actual_hz(const pm_f1_pwm_timer_t *t)
{
    return f1_apb1_timer_clock() /
           ((uint32_t)(t->psc + 1u) * ((uint32_t)t->arr + 1u));
}

static void pwm_gpio_init(const pm_f1_pwm_pins_t *pins)
{
    GPIO_InitTypeDef init;

    gpio_clk_enable(pins->port);

    init.Pin   = pins->pin;
    init.Mode  = GPIO_MODE_AF_PP;
    init.Pull  = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(pins->port, &init);
}

static pm_err_t pwm_channel_config(pm_f1_pwm_timer_t *t, uint32_t channel,
                                   uint16_t permille)
{
    TIM_OC_InitTypeDef oc;

    oc.OCMode       = TIM_OCMODE_PWM1;
    oc.Pulse        = pm_pwm_compare_count(t->arr, permille);
    oc.OCPolarity   = TIM_OCPOLARITY_HIGH;
    oc.OCNPolarity  = TIM_OCNPOLARITY_HIGH;
    oc.OCFastMode   = TIM_OCFAST_DISABLE;
    oc.OCIdleState  = TIM_OCIDLESTATE_RESET;
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;

    return (HAL_TIM_PWM_ConfigChannel(&t->htim, &oc, channel) == HAL_OK)
               ? PM_OK
               : PM_ERR_IO;
}

/* Re-applies the stored duty of every open channel after a period change. */
static void pwm_refresh_compare(pm_f1_pwm_timer_t *t, uint8_t timer_index,
                                const pm_f1_pwm_ctx_t *c)
{
    uint8_t i;
    uint8_t n = (c->count < PM_F1_PWM_COUNT) ? c->count : PM_F1_PWM_COUNT;

    for (i = 0u; i < n; i++) {
        const pm_f1_pwm_pins_t *pins = &c->pins[i];

        if ((s_pwm[i].opened != 0u) && (pins->timer_index == timer_index)) {
            __HAL_TIM_SET_COMPARE(&t->htim, pins->channel,
                                  pm_pwm_compare_count(t->arr, s_pwm[i].permille));
        }
    }
}

static pm_err_t f1_pwm_open(void *ctx, pm_pwm_t *p, const pm_pwm_cfg_t *cfg,
                            uint32_t *actual_hz)
{
    const pm_f1_pwm_ctx_t  *c = (const pm_f1_pwm_ctx_t *)ctx;
    const pm_f1_pwm_pins_t *pins;
    pm_f1_pwm_timer_t      *t;
    pm_f1_pwm_ch_t         *ch;
    uint8_t                 ci;
    uint16_t                psc;
    uint16_t                arr;
    pm_err_t                rc;

    if ((c == NULL) || (p == NULL) || (cfg == NULL) || (actual_hz == NULL)) {
        return PM_ERR_NULL;
    }
    if ((cfg->hw_id >= PM_F1_PWM_COUNT) || (cfg->hw_id >= c->count)) {
        return PM_ERR_RANGE;
    }

    ch = &s_pwm[cfg->hw_id];
    if (ch->opened != 0u) {
        return PM_ERR_BUSY;
    }

    pins = &c->pins[cfg->hw_id];
    if ((pins->port == NULL) || (pins->pin == 0u) || (pins->inst == NULL)) {
        return PM_ERR_PARAM;
    }
#if defined(PM_ENABLE_TONE)
    /* Symmetric with the tone side: a pin belongs to one backend only. */
    if (tone_uses_gpio(pins->port, pins->pin)) {
        return PM_ERR_CONFLICT;
    }
#endif
    if ((pins->timer_index < PM_F1_TIMER_FIRST) ||
        (pins->timer_index >= (PM_F1_TIMER_FIRST + PM_F1_PWM_TIMER_COUNT))) {
        return PM_ERR_PARAM;
    }
    ci = pwm_channel_index(pins->channel);
    if (ci == PM_F1_PWM_NO_CHANNEL) {
        return PM_ERR_PARAM;
    }

    t = &s_pwm_timer[pins->timer_index - PM_F1_TIMER_FIRST];
    if (t->inst != pins->inst) {
        return PM_ERR_PARAM;
    }

    if (t->open_mask != 0u) {
        /* A sibling already owns the time base: the period must match. */
        if (t->freq_req != cfg->freq_hz) {
            return PM_ERR_BUSY;
        }
        psc = t->psc;
        arr = t->arr;
    } else {
        pm_pwm_timing_t timing;

        rc = pm_pwm_timing_compute(f1_apb1_timer_clock(), cfg->freq_hz,
                                   &timing);
        if (rc != PM_OK) {
            return rc;
        }
        psc = timing.psc;
        arr = timing.arr;
    }

    /* Declare the timer before touching a register: another module (tone or,
     * later, a counter) already on this TIM means PM_ERR_CONFLICT instead of
     * silently rewriting its time base (docs/00-contract.md section 20). */
    {
        pm_timer_claim_t claim;

        claim.hw_id    = pins->timer_index;
        claim.owner_id = cfg->hw_id;
        claim.role     = PM_TIMER_ROLE_PWM;
        claim.psc      = psc;
        claim.arr      = arr;
        claim.channels = (uint8_t)(1u << ci);

        rc = pm_timer_claim(&claim);
        if (rc != PM_OK) {
            return rc;
        }
    }

    /* Mandatory self-check of the exact pair about to be programmed. */
    if (!pm_pwm_timing_matches(f1_apb1_timer_clock(), psc, arr, cfg->freq_hz,
                               10u)) {
        (void)pm_timer_release(pins->timer_index, cfg->hw_id);
        return PM_ERR_PARAM;
    }

    if (t->open_mask == 0u) {
        pwm_timer_clk_enable(t->inst);
        t->psc    = psc;
        t->arr    = arr;
        t->freq_req = cfg->freq_hz;

        t->htim.Instance               = t->inst;
        t->htim.Init.Prescaler         = psc;
        t->htim.Init.CounterMode       = TIM_COUNTERMODE_UP;
        t->htim.Init.Period            = arr;
        t->htim.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
        t->htim.Init.RepetitionCounter = 0u;
        t->htim.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

        if (HAL_TIM_PWM_Init(&t->htim) != HAL_OK) {
            (void)pm_timer_release(pins->timer_index, cfg->hw_id);
            return PM_ERR_IO;
        }
        t->inited = 1u;
    }

    pwm_gpio_init(pins);
    if (pwm_channel_config(t, pins->channel, cfg->duty_permille) != PM_OK) {
        if (t->open_mask == 0u) {
            (void)HAL_TIM_Base_DeInit(&t->htim);
            t->inited = 0u;
        }
        (void)pm_timer_release(pins->timer_index, cfg->hw_id);
        return PM_ERR_IO;
    }
    if (HAL_TIM_PWM_Start(&t->htim, pins->channel) != HAL_OK) {
        if (t->open_mask == 0u) {
            (void)HAL_TIM_Base_DeInit(&t->htim);
            t->inited = 0u;
        }
        (void)pm_timer_release(pins->timer_index, cfg->hw_id);
        return PM_ERR_IO;
    }

    t->open_mask = (uint8_t)(t->open_mask | (1u << ci));
    ch->opened   = 1u;
    ch->enabled  = 1u;
    ch->ch_index = ci;
    ch->permille = cfg->duty_permille;

    *actual_hz = pwm_actual_hz(t);
    return PM_OK;
}

static void f1_pwm_close(void *ctx, pm_pwm_t *p)
{
    const pm_f1_pwm_ctx_t  *c = (const pm_f1_pwm_ctx_t *)ctx;
    const pm_f1_pwm_pins_t *pins;
    pm_f1_pwm_timer_t      *t;
    pm_f1_pwm_ch_t         *ch;

    if ((c == NULL) || (p == NULL) || (p->hw_id >= PM_F1_PWM_COUNT) ||
        (p->hw_id >= c->count)) {
        return;
    }

    ch = &s_pwm[p->hw_id];
    if (ch->opened == 0u) {
        return;
    }
    pins = &c->pins[p->hw_id];
    t    = &s_pwm_timer[pins->timer_index - PM_F1_TIMER_FIRST];

    (void)HAL_TIM_PWM_Stop(&t->htim, pins->channel);
    t->open_mask = (uint8_t)(t->open_mask & (uint8_t)~(1u << ch->ch_index));
    (void)pm_timer_release(pins->timer_index, p->hw_id);

    if (t->open_mask == 0u) {
        (void)HAL_TIM_Base_DeInit(&t->htim);
        t->inited   = 0u;
        t->freq_req = 0u;
        t->psc      = 0u;
        t->arr      = 0u;
    }

    ch->opened   = 0u;
    ch->enabled  = 0u;
    ch->ch_index = 0u;
    ch->permille = 0u;
}

static pm_err_t f1_pwm_set_freq(void *ctx, pm_pwm_t *p, uint32_t freq_hz,
                                uint32_t *actual_hz)
{
    const pm_f1_pwm_ctx_t  *c = (const pm_f1_pwm_ctx_t *)ctx;
    const pm_f1_pwm_pins_t *pins;
    pm_f1_pwm_timer_t      *t;
    pm_f1_pwm_ch_t         *ch;
    uint16_t                psc;
    uint16_t                arr;
    uint32_t                other;

    if ((c == NULL) || (p == NULL) || (actual_hz == NULL)) {
        return PM_ERR_NULL;
    }
    if ((p->hw_id >= PM_F1_PWM_COUNT) || (p->hw_id >= c->count)) {
        return PM_ERR_RANGE;
    }

    ch = &s_pwm[p->hw_id];
    if (ch->opened == 0u) {
        return PM_ERR_STATE;
    }
    pins = &c->pins[p->hw_id];
    t    = &s_pwm_timer[pins->timer_index - PM_F1_TIMER_FIRST];

    other = (uint32_t)(t->open_mask & (uint8_t)~(1u << ch->ch_index));
    if ((other != 0u) && (t->freq_req != freq_hz)) {
        /* Retiming the timer would silently change the sibling's period. */
        return PM_ERR_BUSY;
    }

    {
        pm_pwm_timing_t timing;
        pm_err_t        rc;

        rc = pm_pwm_timing_compute(f1_apb1_timer_clock(), freq_hz, &timing);
        if (rc != PM_OK) {
            return rc;
        }
        psc = timing.psc;
        arr = timing.arr;

        /* The occupation table must agree with the registers, including for
         * the sibling channels that share this time base. */
        rc = pm_timer_retime(pins->timer_index, p->hw_id, psc, arr);
        if (rc != PM_OK) {
            return rc;
        }
    }
    t->psc      = psc;
    t->arr      = arr;
    t->freq_req = freq_hz;

    __HAL_TIM_SET_PRESCALER(&t->htim, psc);
    __HAL_TIM_SET_AUTORELOAD(&t->htim, arr);
    t->htim.Instance->EGR = TIM_EGR_UG;   /* load PSC/ARR now */
    pwm_refresh_compare(t, pins->timer_index, c);

    *actual_hz = pwm_actual_hz(t);
    return PM_OK;
}

static pm_err_t f1_pwm_set_duty(void *ctx, pm_pwm_t *p, uint16_t permille)
{
    const pm_f1_pwm_ctx_t  *c = (const pm_f1_pwm_ctx_t *)ctx;
    const pm_f1_pwm_pins_t *pins;
    pm_f1_pwm_timer_t      *t;
    pm_f1_pwm_ch_t         *ch;

    if ((c == NULL) || (p == NULL)) {
        return PM_ERR_NULL;
    }
    if ((p->hw_id >= PM_F1_PWM_COUNT) || (p->hw_id >= c->count)) {
        return PM_ERR_RANGE;
    }

    ch = &s_pwm[p->hw_id];
    if (ch->opened == 0u) {
        return PM_ERR_STATE;
    }
    pins = &c->pins[p->hw_id];
    t    = &s_pwm_timer[pins->timer_index - PM_F1_TIMER_FIRST];

    __HAL_TIM_SET_COMPARE(&t->htim, pins->channel,
                          pm_pwm_compare_count(t->arr, permille));
    ch->permille = permille;
    return PM_OK;
}

static pm_err_t f1_pwm_enable(void *ctx, pm_pwm_t *p, bool on)
{
    const pm_f1_pwm_ctx_t  *c = (const pm_f1_pwm_ctx_t *)ctx;
    const pm_f1_pwm_pins_t *pins;
    pm_f1_pwm_timer_t      *t;
    pm_f1_pwm_ch_t         *ch;
    HAL_StatusTypeDef       st;

    if ((c == NULL) || (p == NULL)) {
        return PM_ERR_NULL;
    }
    if ((p->hw_id >= PM_F1_PWM_COUNT) || (p->hw_id >= c->count)) {
        return PM_ERR_RANGE;
    }

    ch = &s_pwm[p->hw_id];
    if (ch->opened == 0u) {
        return PM_ERR_STATE;
    }
    pins = &c->pins[p->hw_id];
    t    = &s_pwm_timer[pins->timer_index - PM_F1_TIMER_FIRST];

    st = on ? HAL_TIM_PWM_Start(&t->htim, pins->channel)
            : HAL_TIM_PWM_Stop(&t->htim, pins->channel);
    if (st != HAL_OK) {
        return PM_ERR_IO;
    }
    ch->enabled = on ? 1u : 0u;
    return PM_OK;
}

static const pm_pwm_ops_t s_pwm_ops = {
    .open     = f1_pwm_open,
    .close    = f1_pwm_close,
    .set_freq = f1_pwm_set_freq,
    .set_duty = f1_pwm_set_duty,
    .enable   = f1_pwm_enable,
};

#endif /* PM_ENABLE_PWM */

#if defined(PM_ENABLE_TONE)
/* ------------------------------------------------------------------ TONE */
/*
 * Port implementation detail, not part of the public contract (section 17):
 *  - one update interrupt every 10 us, PSC derived from the APB1 timer clock;
 *  - period_ticks = 100000 / freq_hz;
 *  - duty_ticks   = period_ticks * permille / 1000, quantised to whole ticks.
 * At 72 MHz this is PSC = 719. The board-verified buzzer path is exactly this
 * shape (TIM3 update interrupt toggling PB9).
 */
#define PM_F1_TONE_TICK_HZ 100000u
#define PM_F1_TONE_COUNT   1u

typedef struct {
    uint8_t           opened;
    uint8_t           active;
    uint8_t           timer_index;
    TIM_HandleTypeDef htim;
    TIM_TypeDef      *inst;
    GPIO_TypeDef     *port;
    uint16_t          pin;
    uint16_t          period_ticks;
    uint16_t          duty_ticks;
    uint16_t          tick;
} pm_f1_tone_t;

static pm_f1_tone_t s_tone[PM_F1_TONE_COUNT];

static void tone_timer_clk_enable(TIM_TypeDef *inst)
{
    if (inst == TIM2) {
        __HAL_RCC_TIM2_CLK_ENABLE();
    } else if (inst == TIM3) {
        __HAL_RCC_TIM3_CLK_ENABLE();
    } else if (inst == TIM4) {
        __HAL_RCC_TIM4_CLK_ENABLE();
    }
}

static IRQn_Type tone_irqn(TIM_TypeDef *inst)
{
    if (inst == TIM2) {
        return TIM2_IRQn;
    }
    if (inst == TIM4) {
        return TIM4_IRQn;
    }
    return TIM3_IRQn;
}

#if defined(PM_ENABLE_PWM)
/* Called by the tone side: is this pin already driven by an open PWM channel? */
static bool pwm_uses_gpio(GPIO_TypeDef *port, uint16_t pin)
{
    uint8_t i;
    uint8_t n = (pm_f1_pwm_ctx.count < PM_F1_PWM_COUNT) ? pm_f1_pwm_ctx.count
                                                        : PM_F1_PWM_COUNT;

    for (i = 0u; i < n; i++) {
        const pm_f1_pwm_pins_t *pins = &pm_f1_pwm_ctx.pins[i];

        if ((s_pwm[i].opened != 0u) && (pins->port == port) &&
            (pins->pin == pin)) {
            return true;
        }
    }
    return false;
}

#if defined(PM_ENABLE_TONE)
/* Called by the PWM side: mirror of pwm_uses_gpio(). No channel on this board
 * shares PB9 with the tone any more, but boards that do must not be able to
 * steal the pin by opening PWM second. */
static bool tone_uses_gpio(GPIO_TypeDef *port, uint16_t pin)
{
    uint8_t i;

    for (i = 0u; i < PM_F1_TONE_COUNT; i++) {
        if ((s_tone[i].opened != 0u) && (s_tone[i].port == port) &&
            (s_tone[i].pin == pin)) {
            return true;
        }
    }
    return false;
}
#endif
#endif

static void tone_apply(pm_f1_tone_t *s, uint32_t freq_hz, uint16_t permille)
{
    uint32_t period = PM_F1_TONE_TICK_HZ / freq_hz;

    if (period == 0u) {
        period = 1u;
    }
    s->period_ticks = (uint16_t)period;      /* <= 1000 for freq >= 100 Hz */
    s->duty_ticks   = (uint16_t)((period * (uint32_t)permille) / 1000u);
    s->tick         = 0u;

    __HAL_TIM_SET_AUTORELOAD(&s->htim, period - 1u);
    s->htim.Instance->EGR = TIM_EGR_UG;      /* load ARR now */
}

/* Weak HAL hook: only the tone timer is periodic in this port. */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    uint8_t i;

    for (i = 0u; i < PM_F1_TONE_COUNT; i++) {
        pm_f1_tone_t *s = &s_tone[i];

        if ((s->active != 0u) && (s->inst == htim->Instance)) {
            s->tick++;
            if (s->tick >= s->period_ticks) {
                s->tick = 0u;
            }
            HAL_GPIO_WritePin(s->port, s->pin,
                              (s->tick < s->duty_ticks) ? GPIO_PIN_SET
                                                        : GPIO_PIN_RESET);
        }
    }
}

static void tone_irq(TIM_TypeDef *inst)
{
    uint8_t i;

    for (i = 0u; i < PM_F1_TONE_COUNT; i++) {
        if ((s_tone[i].opened != 0u) && (s_tone[i].inst == inst)) {
            HAL_TIM_IRQHandler(&s_tone[i].htim);
        }
    }
}

void TIM2_IRQHandler(void)
{
    tone_irq(TIM2);
}

void TIM3_IRQHandler(void)
{
    tone_irq(TIM3);
}

void TIM4_IRQHandler(void)
{
    tone_irq(TIM4);
}

static pm_err_t f1_tone_open(void *ctx, pm_tone_t *t, const pm_tone_cfg_t *cfg)
{
    const pm_f1_tone_ctx_t  *c = (const pm_f1_tone_ctx_t *)ctx;
    const pm_f1_tone_pins_t *pins;
    pm_f1_tone_t            *s;
    pm_timer_claim_t         claim;
    uint32_t                 psc;
    uint32_t                 period;
    pm_err_t                 rc;

    if ((c == NULL) || (t == NULL) || (cfg == NULL)) {
        return PM_ERR_NULL;
    }
    if ((cfg->hw_id >= PM_F1_TONE_COUNT) || (cfg->hw_id >= c->count)) {
        return PM_ERR_RANGE;
    }

    s = &s_tone[cfg->hw_id];
    if (s->opened != 0u) {
        return PM_ERR_BUSY;
    }

    pins = &c->pins[cfg->hw_id];
    if ((pins->port == NULL) || (pins->pin == 0u) || (pins->inst == NULL)) {
        return PM_ERR_PARAM;
    }

#if defined(PM_ENABLE_PWM)
    if (pwm_uses_gpio(pins->port, pins->pin)) {
        return PM_ERR_CONFLICT;   /* the hardware PWM owns this pin */
    }
#endif

    psc = f1_apb1_timer_clock() / PM_F1_TONE_TICK_HZ;
    if ((psc == 0u) || (psc > 0x10000u)) {
        return PM_ERR_PARAM;
    }
    psc -= 1u;

    period = PM_F1_TONE_TICK_HZ / cfg->freq_hz;   /* 20 .. 1000 ticks */

    claim.hw_id    = pins->timer_index;
    claim.owner_id = cfg->hw_id;
    claim.role     = PM_TIMER_ROLE_TONE;
    claim.psc      = (uint16_t)psc;
    claim.arr      = (uint16_t)(period - 1u);
    claim.channels = 0u;

    rc = pm_timer_claim(&claim);
    if (rc != PM_OK) {
        return rc;
    }

    tone_timer_clk_enable(pins->inst);
    s->inst        = pins->inst;
    s->port        = pins->port;
    s->pin         = pins->pin;
    s->timer_index = pins->timer_index;

    s->htim.Instance               = pins->inst;
    s->htim.Init.Prescaler         = psc;
    s->htim.Init.CounterMode       = TIM_COUNTERMODE_UP;
    s->htim.Init.Period            = period - 1u;
    s->htim.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    s->htim.Init.RepetitionCounter = 0u;
    s->htim.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    if (HAL_TIM_Base_Init(&s->htim) != HAL_OK) {
        (void)pm_timer_release(pins->timer_index, cfg->hw_id);
        return PM_ERR_IO;
    }
    s->opened = 1u;

    gpio_clk_enable(pins->port);
    {
        GPIO_InitTypeDef init;

        init.Pin   = pins->pin;
        init.Mode  = GPIO_MODE_OUTPUT_PP;
        init.Pull  = GPIO_NOPULL;
        init.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(pins->port, &init);
    }
    HAL_GPIO_WritePin(pins->port, pins->pin, GPIO_PIN_RESET);

    HAL_NVIC_SetPriority(tone_irqn(pins->inst), 2u, 0u);
    HAL_NVIC_EnableIRQ(tone_irqn(pins->inst));

    tone_apply(s, cfg->freq_hz, cfg->duty_permille);
    if (HAL_TIM_Base_Start_IT(&s->htim) != HAL_OK) {
        HAL_NVIC_DisableIRQ(tone_irqn(pins->inst));
        (void)HAL_TIM_Base_DeInit(&s->htim);
        (void)pm_timer_release(pins->timer_index, cfg->hw_id);
        (void)memset(s, 0, sizeof(*s));
        return PM_ERR_IO;
    }
    s->active = 1u;
    return PM_OK;
}

static void f1_tone_close(void *ctx, pm_tone_t *t)
{
    const pm_f1_tone_ctx_t *c = (const pm_f1_tone_ctx_t *)ctx;
    pm_f1_tone_t *s;

    if ((c == NULL) || (t == NULL) || (t->hw_id >= PM_F1_TONE_COUNT) ||
        (t->hw_id >= c->count)) {
        return;
    }
    s = &s_tone[t->hw_id];
    if (s->opened == 0u) {
        return;
    }

    (void)HAL_TIM_Base_Stop_IT(&s->htim);
    HAL_NVIC_DisableIRQ(tone_irqn(s->inst));
    HAL_GPIO_WritePin(s->port, s->pin, GPIO_PIN_RESET);
    (void)HAL_TIM_Base_DeInit(&s->htim);
    (void)pm_timer_release(s->timer_index, t->hw_id);
    (void)memset(s, 0, sizeof(*s));
}

static pm_err_t f1_tone_set(void *ctx, pm_tone_t *t, uint32_t freq_hz,
                            uint16_t duty_permille)
{
    const pm_f1_tone_ctx_t *c = (const pm_f1_tone_ctx_t *)ctx;
    pm_f1_tone_t *s;

    if ((c == NULL) || (t == NULL) || (t->hw_id >= PM_F1_TONE_COUNT) ||
        (t->hw_id >= c->count)) {
        return PM_ERR_NULL;
    }
    s = &s_tone[t->hw_id];
    if (s->opened == 0u) {
        return PM_ERR_STATE;
    }

    tone_apply(s, freq_hz, duty_permille);
    if (s->active == 0u) {
        if (HAL_TIM_Base_Start_IT(&s->htim) != HAL_OK) {
            return PM_ERR_IO;
        }
        s->active = 1u;
    }
    return PM_OK;
}

static pm_err_t f1_tone_off(void *ctx, pm_tone_t *t)
{
    const pm_f1_tone_ctx_t *c = (const pm_f1_tone_ctx_t *)ctx;
    pm_f1_tone_t *s;

    if ((c == NULL) || (t == NULL) || (t->hw_id >= PM_F1_TONE_COUNT) ||
        (t->hw_id >= c->count)) {
        return PM_ERR_NULL;
    }
    s = &s_tone[t->hw_id];
    if (s->opened == 0u) {
        return PM_ERR_STATE;
    }

    (void)HAL_TIM_Base_Stop_IT(&s->htim);
    HAL_GPIO_WritePin(s->port, s->pin, GPIO_PIN_RESET);
    s->active = 0u;
    s->tick   = 0u;
    return PM_OK;
}

static const pm_tone_ops_t s_tone_ops = {
    .open  = f1_tone_open,
    .close = f1_tone_close,
    .set   = f1_tone_set,
    .off   = f1_tone_off,
};

#endif /* PM_ENABLE_TONE */

#if defined(PM_ENABLE_TRACK)
/* ----------------------------------------------------------------- TRACK */

#define PM_F1_TRACK_COUNT 1u

typedef struct {
    uint8_t opened;
} pm_f1_track_t;

static pm_f1_track_t s_track[PM_F1_TRACK_COUNT];

static bool track_uses_spi(uint8_t spi_id)
{
    uint8_t i;
    uint8_t n = (pm_f1_track_ctx.count < PM_F1_TRACK_COUNT)
                    ? pm_f1_track_ctx.count
                    : PM_F1_TRACK_COUNT;

    for (i = 0u; i < n; i++) {
        if ((s_track[i].opened != 0u) &&
            (pm_f1_track_ctx.pins[i].spi_hw_id == spi_id)) {
            return true;
        }
    }
    return false;
}

static pm_err_t f1_track_open(void *ctx, pm_track_t *t,
                              const pm_track_cfg_t *cfg)
{
    const pm_f1_track_ctx_t  *c = (const pm_f1_track_ctx_t *)ctx;
    const pm_f1_track_pins_t *pins;
    GPIO_InitTypeDef          init;

    if ((c == NULL) || (t == NULL) || (cfg == NULL)) {
        return PM_ERR_NULL;
    }
    if ((cfg->hw_id >= PM_F1_TRACK_COUNT) || (cfg->hw_id >= c->count)) {
        return PM_ERR_RANGE;
    }
    if (s_track[cfg->hw_id].opened != 0u) {
        return PM_ERR_BUSY;
    }

    pins = &c->pins[cfg->hw_id];
    if ((pins->port == NULL) || (pins->pin == 0u)) {
        return PM_ERR_PARAM;
    }
#if defined(PM_ENABLE_SPI)
    if ((pins->spi_hw_id < PM_F1_SPI_COUNT) &&
        (s_spi[pins->spi_hw_id].opened != 0u)) {
        return PM_ERR_CONFLICT;   /* PB14 is SPI2_MISO */
    }
#endif

    gpio_clk_enable(pins->port);
    init.Pin   = pins->pin;
    init.Mode  = GPIO_MODE_INPUT;
    init.Pull  = GPIO_PULLUP;     /* the sensor is open-drain / open-collector */
    init.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(pins->port, &init);

    s_track[cfg->hw_id].opened = 1u;
    return PM_OK;
}

static void f1_track_close(void *ctx, pm_track_t *t)
{
    const pm_f1_track_ctx_t *c = (const pm_f1_track_ctx_t *)ctx;

    if ((c == NULL) || (t == NULL) || (t->hw_id >= PM_F1_TRACK_COUNT) ||
        (t->hw_id >= c->count)) {
        return;
    }
    if (s_track[t->hw_id].opened == 0u) {
        return;
    }

    HAL_GPIO_DeInit(c->pins[t->hw_id].port, c->pins[t->hw_id].pin);
    s_track[t->hw_id].opened = 0u;
}

static pm_err_t f1_track_read(void *ctx, pm_track_t *t, bool *high)
{
    const pm_f1_track_ctx_t  *c = (const pm_f1_track_ctx_t *)ctx;
    const pm_f1_track_pins_t *pins;

    if ((c == NULL) || (t == NULL) || (high == NULL)) {
        return PM_ERR_NULL;
    }
    if ((t->hw_id >= PM_F1_TRACK_COUNT) || (t->hw_id >= c->count)) {
        return PM_ERR_NULL;
    }
    if (s_track[t->hw_id].opened == 0u) {
        return PM_ERR_STATE;
    }

    pins  = &c->pins[t->hw_id];
    *high = (HAL_GPIO_ReadPin(pins->port, pins->pin) == GPIO_PIN_SET);
    return PM_OK;
}

static const pm_track_ops_t s_track_ops = {
    .open  = f1_track_open,
    .close = f1_track_close,
    .read  = f1_track_read,
};

#endif /* PM_ENABLE_TRACK */

#if defined(PM_ENABLE_ENCODER)
/* --------------------------------------------------------------- ENCODER */

#define PM_F1_ENCODER_COUNT 1u

typedef struct {
    uint8_t       opened;
    pm_encoder_t *owner;
} pm_f1_encoder_t;

static pm_f1_encoder_t s_encoder[PM_F1_ENCODER_COUNT];

static uint8_t encoder_read_ab(const pm_f1_encoder_pins_t *pins)
{
    uint8_t a = (HAL_GPIO_ReadPin(pins->a_port, pins->a_pin) == GPIO_PIN_SET)
                    ? 1u : 0u;
    uint8_t b = (HAL_GPIO_ReadPin(pins->b_port, pins->b_pin) == GPIO_PIN_SET)
                    ? 1u : 0u;

    return (uint8_t)((a << 1) | b);
}

static pm_err_t f1_encoder_open(void *ctx, pm_encoder_t *e,
                                const pm_encoder_cfg_t *cfg,
                                uint8_t *initial_ab)
{
    const pm_f1_encoder_ctx_t  *c = (const pm_f1_encoder_ctx_t *)ctx;
    const pm_f1_encoder_pins_t *pins;
    GPIO_InitTypeDef            init;

    if ((c == NULL) || (e == NULL) || (cfg == NULL) || (initial_ab == NULL)) {
        return PM_ERR_NULL;
    }
    if ((cfg->hw_id >= PM_F1_ENCODER_COUNT) || (cfg->hw_id >= c->count)) {
        return PM_ERR_RANGE;
    }
    if (s_encoder[cfg->hw_id].opened != 0u) {
        return PM_ERR_BUSY;
    }

    pins = &c->pins[cfg->hw_id];
    if ((pins->a_port == NULL) || (pins->a_pin == 0u) ||
        (pins->b_port == NULL) || (pins->b_pin == 0u)) {
        return PM_ERR_PARAM;
    }
    /* Validate the optional button before touching any pin, so a rejected
     * request leaves the A/B inputs untouched. */
    if ((cfg->has_button != 0u) &&
        ((pins->button_port == NULL) || (pins->button_pin == 0u))) {
        return PM_ERR_PARAM;
    }

    gpio_clk_enable(pins->a_port);
    gpio_clk_enable(pins->b_port);
    init.Mode  = GPIO_MODE_IT_RISING_FALLING;
    init.Pull  = GPIO_PULLUP;     /* open-collector encoder */
    init.Speed = GPIO_SPEED_FREQ_LOW;
    init.Pin   = pins->a_pin;
    HAL_GPIO_Init(pins->a_port, &init);
    init.Pin   = pins->b_pin;
    HAL_GPIO_Init(pins->b_port, &init);

    if (cfg->has_button != 0u) {
        /* KY-040 SW closes to GND; the pull-up is what defines "pressed". */
        gpio_clk_enable(pins->button_port);
        init.Mode  = GPIO_MODE_INPUT;
        init.Pull  = GPIO_PULLUP;
        init.Speed = GPIO_SPEED_FREQ_LOW;
        init.Pin   = pins->button_pin;
        HAL_GPIO_Init(pins->button_port, &init);
    }

    /* Drop any stale pending bit before the interrupt is enabled. */
    EXTI->PR = (uint32_t)(pins->a_pin | pins->b_pin);
    HAL_NVIC_SetPriority(pins->irqn, 2u, 0u);
    HAL_NVIC_EnableIRQ(pins->irqn);

    s_encoder[cfg->hw_id].owner  = e;
    s_encoder[cfg->hw_id].opened = 1u;

    *initial_ab = encoder_read_ab(pins);
    return PM_OK;
}

static void f1_encoder_close(void *ctx, pm_encoder_t *e)
{
    const pm_f1_encoder_ctx_t *c = (const pm_f1_encoder_ctx_t *)ctx;

    if ((c == NULL) || (e == NULL) || (e->hw_id >= PM_F1_ENCODER_COUNT) ||
        (e->hw_id >= c->count)) {
        return;
    }
    if (s_encoder[e->hw_id].opened == 0u) {
        return;
    }

    HAL_NVIC_DisableIRQ(c->pins[e->hw_id].irqn);
    HAL_GPIO_DeInit(c->pins[e->hw_id].a_port, c->pins[e->hw_id].a_pin);
    HAL_GPIO_DeInit(c->pins[e->hw_id].b_port, c->pins[e->hw_id].b_pin);
    if (e->has_button != 0u) {
        HAL_GPIO_DeInit(c->pins[e->hw_id].button_port,
                        c->pins[e->hw_id].button_pin);
    }

    s_encoder[e->hw_id].owner  = NULL;
    s_encoder[e->hw_id].opened = 0u;
}

static pm_err_t f1_encoder_read_button(void *ctx, pm_encoder_t *e,
                                       bool *pressed)
{
    const pm_f1_encoder_ctx_t  *c = (const pm_f1_encoder_ctx_t *)ctx;
    const pm_f1_encoder_pins_t *pins;

    if ((c == NULL) || (e == NULL) || (pressed == NULL)) {
        return PM_ERR_NULL;
    }
    if ((e->hw_id >= PM_F1_ENCODER_COUNT) || (e->hw_id >= c->count)) {
        return PM_ERR_RANGE;
    }
    pins = &c->pins[e->hw_id];
    if ((pins->button_port == NULL) || (pins->button_pin == 0u)) {
        return PM_ERR_DISABLED;
    }

    *pressed = (HAL_GPIO_ReadPin(pins->button_port, pins->button_pin) ==
                GPIO_PIN_RESET);
    return PM_OK;
}

/* PA8/PA9 share EXTI9_5. Both edges of both pins call the core hook. */
void EXTI9_5_IRQHandler(void)
{
    uint8_t i;

    for (i = 0u; i < PM_F1_ENCODER_COUNT; i++) {
        pm_f1_encoder_t            *s = &s_encoder[i];
        const pm_f1_encoder_pins_t *pins;

        if ((s->opened == 0u) || (s->owner == NULL)) {
            continue;
        }
        pins = &pm_f1_encoder_ctx.pins[i];
        if ((EXTI->PR & (uint32_t)(pins->a_pin | pins->b_pin)) == 0u) {
            continue;
        }
        EXTI->PR = (uint32_t)(pins->a_pin | pins->b_pin);   /* write 1 to clear */
        pm_encoder_hw_edge(s->owner, encoder_read_ab(pins));
    }
}

static const pm_encoder_ops_t s_encoder_ops = {
    .open        = f1_encoder_open,
    .close       = f1_encoder_close,
    .read_button = f1_encoder_read_button,
};

#endif /* PM_ENABLE_ENCODER */

/* ---------------------------------------------------------- port registry */

static const pm_port_t s_port = {
    .gpio           = &s_gpio_ops,
    .gpio_ctx       = (void *)&pm_f1_gpio_ctx,
    .gpio_pin_count = (uint16_t)BOARD_GPIO_COUNT,
    .uart           = &s_uart_ops,
    .uart_ctx       = NULL,
    .uart_count     = (uint8_t)PM_F1_UART_COUNT,
#if defined(PM_ENABLE_SPI)
    .spi            = &s_spi_ops,
    .spi_ctx        = (void *)&pm_f1_spi_ctx,
    .spi_count      = (uint8_t)PM_F1_SPI_COUNT,
#endif
#if defined(PM_ENABLE_I2C)
    .i2c            = &s_i2c_ops,
    .i2c_ctx        = (void *)&pm_f1_i2c_ctx,
    .i2c_count      = (uint8_t)PM_F1_I2C_COUNT,
#endif
#if defined(PM_ENABLE_ADC)
    .adc            = &s_adc_ops,
    .adc_ctx        = (void *)&pm_f1_adc_ctx,
    .adc_count      = (uint8_t)PM_F1_ADC_COUNT,
#endif
#if defined(PM_ENABLE_PWM)
    .pwm            = &s_pwm_ops,
    .pwm_ctx        = (void *)&pm_f1_pwm_ctx,
    .pwm_count      = (uint8_t)PM_F1_PWM_COUNT,
#endif
#if defined(PM_ENABLE_TONE)
    .tone           = &s_tone_ops,
    .tone_ctx       = (void *)&pm_f1_tone_ctx,
    .tone_count     = (uint8_t)PM_F1_TONE_COUNT,
#endif
#if defined(PM_ENABLE_TRACK)
    .track          = &s_track_ops,
    .track_ctx      = (void *)&pm_f1_track_ctx,
    .track_count    = (uint8_t)PM_F1_TRACK_COUNT,
#endif
#if defined(PM_ENABLE_ENCODER)
    .encoder        = &s_encoder_ops,
    .encoder_ctx    = (void *)&pm_f1_encoder_ctx,
    .encoder_count  = (uint8_t)PM_F1_ENCODER_COUNT,
#endif
};

const pm_port_t *pm_port_get(void)
{
#if defined(PM_ENABLE_PWM) || defined(PM_ENABLE_TONE)
    /* Lets pm_timer_claim() see timers started outside the model. Idempotent;
     * pm_port_stm32f1.c is the only port in this repository. */
    pm_timer_set_probe(f1_timer_externally_busy, NULL);
#endif
    return &s_port;
}
