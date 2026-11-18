# STM32F1 外设通用模型

## 这是什么

一套按"接口契约优先"方式组织的 STM32F1 外设代码：上层只依赖 `include/pm/` 里的公共 API，芯片相关的一切都收敛在 port 层。目标是换板子只改一张引脚表，换芯片只换一个 port 目录，核心逻辑永远能在主机上跑单测。

当前版本 **v0.4**，配套 STM32F103C8T6 最小系统板（72 MHz、20 KB RAM、64 KB Flash）。
已实现 GPIO、UART、SPI（主机全双工，中断 + 可选 DMA）、I2C（主机，中断状态机，支持写读合并）、ADC（单通道单次转换，EOC 中断）与 PWM（定时器比较输出）。

## 能做什么

- 驱动 GPIO、UART、SPI（主机全双工）、I2C（主机）、ADC（单通道单次）、PWM（定时器比较输出）；
- 异步接口统一走事件队列：ISR 只投递事件，用户回调只在 `pm_*_poll()` 的主循环上下文执行；
- 静态分配、无 `malloc`；错误码为 `pm_err_t` 负值；超时语义统一（0 = 立即返回 `PM_ERR_AGAIN`，>0 = 有界等待）；
- 换板子只改 `platform/stm32f103/board_f103c8.c` 的引脚表，换芯片只换一个 port 目录；
- 核心逻辑可在 PC 上用内存 mock 后端跑主机单测，不需要硬件和 ARM 工具链。

## 不能做什么

**未上板验证的外设**（已实现、已编译、有主机单测，但没接真实器件/负载，不能当作"可用"）：

| 外设 | 尚未验证的部分 |
| --- | --- |
| SPI | 未接从机；只验证了主机初始化、中断/DMA 收发路径与主机 mock |
| I2C | 未接真实器件；本板板载 OLED（0x7A）与 AHT20（0x70）可直接用于验证 |
| ADC | 未接真实信号源；只验证了转换流程与主机 mock |
| PWM | 未接负载（电机 / 舵机 / 蜂鸣器）；只验证了配置与占空比换算 |

已在真实板卡上验证的只有：时钟（HSE→PLL 72 MHz）与 UART 收发。

此外当前不提供：DMA 通用抽象、RTC、Flash、看门狗、FreeRTOS 集成、CI、CMake presets；
同一硬件定时器的通道共享周期，不同频率同时使用会返回 `PM_ERR_BUSY`。

---

## 验证状态

| 项目 | 状态 | 证据 |
| --- | --- | --- |
| 时钟 | HSE→PLL 72 MHz | `pm_clock_init()` 配置；HSE 起振失败回落 HSI 8 MHz，`pm_platform_core_clock()` 可读实际主频 |
| ARM 交叉编译 | 通过 | `arm-none-eabi-gcc 14.3.1`，无项目代码警告 |
| 链接 | 通过 | Flash 24424 B / 64 KB（37.27%），RAM 3104 B / 20 KB（15.16%） |
| 产物 | 已生成 | `pm_f103_demo`（ELF）/ `.bin`（24424 B）/ `.hex` / `.map` |
| 主机单测 | 通过 | 345 项断言，0 失败（MSVC 19.51，C11） |
| 动态分配 | 不存在 | `nm` 中无 `malloc` / `free` / `_sbrk` |
| 向量表 | 正确 | 首字 `0x20005000`（20 KB RAM 顶），Reset_Handler `0x08000229`（Thumb） |
| **上板运行** | **部分验证** | 时钟与 UART 收发已在真实板卡上验证；I2C、SPI、ADC、PWM 未接真实器件/负载 |

未做上板验证的部分不写成"可用"。

**I2C 器件事实**（针对本板）：本板 I2C1 上的 OLED（0x7A，驱动芯片 CH1116）与 AHT20
温湿度传感器（0x70）都是**板载器件**，不是外部模块；另有一个 I2C 排针与两者共用 PB6/PB7。
因此 I2C 可以直接用板载器件验证，但目前**尚未在真实器件上验证**。

### Flash/RAM 增量

| 版本 | 内容 | Flash | RAM | 相对上一版 |
| --- | --- | --- | --- | --- |
| v0.1 | GPIO + UART | 6440 B（9.83%） | 2224 B（10.86%） | — |
| v0.2 | + SPI（IT + DMA） | 11704 B（17.86%） | 2728 B（13.32%） | +5264 B / +504 B |
| v0.3 | + 时钟树修复、UART RX 与 SPI MISO 引脚修复、I2C | 18872 B（28.80%） | 2840 B（13.87%） | +7168 B / +112 B |
| v0.4 | + ADC（单次转换）与 PWM（TIM2/TIM4 比较输出） | 24424 B（37.27%） | 3104 B（15.16%） | +5552 B / +264 B |

v0.3 的 Flash 增量主要来自 HAL I2C 状态机（`HAL_I2C_EV_IRQHandler` 2228 B、
`HAL_I2C_Init` 336 B、`HAL_I2C_ER_IRQHandler` 320 B、四个 `Master_*_IT` 共 828 B）
与 `HAL_RCC_OscConfig` 824 B + `HAL_RCC_ClockConfig` 312 B；模型与端口胶水约 1 KB。
RAM 的 +112 B 是静态 `I2C_HandleTypeDef` 与端口状态（`s_i2c`，`nm` 显示 0x70）。
由于端口注册表无条件引用已实现外设的 ops，**即使 APP 不用 I2C/SPI，这部分代码也会被链接**。

v0.4 的 Flash 增量来自 HAL ADC（`HAL_ADC_Init`、`HAL_ADC_Start_IT`、
`HAL_ADC_IRQHandler`、`HAL_ADCEx_Calibration_Start`）与 HAL TIM PWM
（`HAL_TIM_PWM_Init`、`HAL_TIM_PWM_ConfigChannel`、`HAL_TIM_PWM_Start/Stop`），
以及模型/端口胶水。RAM 的 +264 B 主要是 `ADC_HandleTypeDef`、`TIM_HandleTypeDef`
与端口静态状态（`s_adc`、`s_pwm_timer[2]`、`s_pwm[4]`）。同样地，APP 不用 ADC/PWM
时这部分也会被链接。

---

## 目录与职责

```
docs/00-contract.md     接口契约（规范，先读这个）
docs/01-layering.md     目录、依赖、时序、移植
include/pm/             公共头，无厂商类型
src/core/               与硬件无关的模型（可在主机编译）
src/osal/               Cortex-M3 的时基与临界区
src/port/stm32f1/       HAL 后端、UART/SPI/I2C/ADC/PWM ISR、pm_port_get()
platform/stm32f103/     板级引脚表、启动文件、链接脚本、HAL 配置
tests/host/             内存 mock 后端 + 主机单测
third_party/            CMSIS 与 ST HAL 子集（BSD-3-Clause，原样分发）
```

---

## 构建

工具链：`arm-none-eabi-gcc` 14.x、CMake ≥ 3.20、Ninja。

`cmake/arm-gcc-toolchain.cmake` 按以下优先级解析工具链：

1. `-DTOOLCHAIN_ROOT=<gcc bin 目录>`（cmake 命令行）；
2. 环境变量 `TOOLCHAIN_ROOT`；
3. 默认不设前缀，由 CMake 从 `PATH` 查找 `arm-none-eabi-*`。

所以工具链已在 `PATH` 中时无需额外参数；使用 STM32Cube、xPack 等 bundle 时显式指过去即可：

```powershell
cmake -S . -B build-arm -G Ninja `
      -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/arm-gcc-toolchain.cmake" `
      -DTOOLCHAIN_ROOT="<cube>/gnu-tools-for-stm32/<version>/bin"
cmake --build build-arm
```

工具链文件必须给绝对路径：CMake 4 会把相对路径按构建目录解析，写成
`-DCMAKE_TOOLCHAIN_FILE=cmake/...` 会在全新构建目录里找不到文件。

产物在 `build-arm/`：`pm_f103_demo`（ELF，无扩展名）、`.bin`、`.hex`、`.map`，构建结束会打印 Flash/RAM 占用。

## 主机单测

不需要任何硬件，也不需要 ARM 工具链：

```powershell
cmake -S tests/host -B build-host
cmake --build build-host --config Debug
./build-host/Debug/pm_host_tests.exe
```

输出形如 `345 checks, 0 failure(s)`，退出码 0 表示全过。

---

## 用法

### GPIO

```c
#include "pm_gpio.h"
#include "board_f103c8.h"

pm_gpio_pin_t led = { .id = BOARD_GPIO_LED_BLUE };
pm_gpio_cfg_t cfg = {
    .dir = PM_GPIO_DIR_OUT,
    .pull = PM_GPIO_PULL_NONE,
    .speed = PM_GPIO_SPEED_LOW,
    .initial_level = PM_GPIO_LEVEL_LOW,
};

(void)pm_gpio_configure(led, &cfg);
(void)pm_gpio_toggle(led);
```

### UART（异步、非阻塞）

```c
static pm_uart_t s_uart;
static uint8_t   s_rx[256];
static uint8_t   s_tx[512];

static const pm_uart_cfg_t cfg = {
    .hw_id = BOARD_UART_DEBUG,      /* USART2, PA2/PA3 */
    .baudrate = 115200,
    .parity = PM_UART_PARITY_NONE,
    .stop_bits = PM_UART_STOP_1,
    .data_bits = 8,
    .rx_storage = s_rx, .rx_size = sizeof(s_rx),
    .tx_storage = s_tx, .tx_size = sizeof(s_tx),
};

(void)pm_uart_init(&s_uart, &cfg);

/* 主循环里 */
size_t got;
if (pm_uart_read(&s_uart, buf, sizeof(buf), &got) == PM_OK) {
    (void)pm_uart_write(&s_uart, buf, got, NULL, PM_UART_TIMEOUT_NOW);
}
pm_evt_t evt;
while (pm_uart_poll(&s_uart, &evt, NULL, NULL)) {
    /* 处理 RX_DATA / TX_DONE / ERROR */
}
```

关键行为：

- `pm_uart_write` 只入队，不忙等发送完成；
- 接收字节全部由 ISR 搬进环形缓冲，主循环读取；
- 用户回调只在 `pm_uart_poll` 里执行，永远不在中断上下文；
- 缓冲区满时丢数据并计数，可通过 `pm_uart_get_stats` 观测。

### SPI（主机、全双工）

```c
static pm_spi_t s_spi;
static uint8_t  s_tx[4];
static uint8_t  s_rx[4];

static const pm_spi_cfg_t spi_cfg = {
    .hw_id     = BOARD_SPI_2,       /* PB13/PB14/PB15，与 KEY2/循迹/编码器按键复用 */
    .clock_hz  = 1000000,
    .mode      = PM_SPI_MODE_0,
    .bit_order = PM_SPI_MSB_FIRST,
    .nss       = PM_SPI_NSS_SOFT,   /* 片选由 GPIO 控制 */
    .use_dma   = false,
};

(void)pm_spi_init(&s_spi, &spi_cfg);

size_t done;
pm_err_t rc = pm_spi_transfer(&s_spi, s_tx, s_rx, 4, &done, 10);
if (rc == PM_OK) {
    /* done == 4，s_rx 已收到 4 字节 */
}
```

- `timeout > 0` 时等待完成，到期自动中止并返回 `PM_ERR_TIMEOUT`，`done` 是已时钟出的字节数；
- `timeout == 0` 启动后立即返回，未完成返回 `PM_ERR_AGAIN`；此后 `tx/rx` 归驱动借用，
  直到 `pm_spi_poll` 取到 `PM_EVT_SPI_DONE` 或 `PM_EVT_SPI_ERROR` 才能复用；
- `use_dma = true` 走 DMA1 通道；DMA 初始化失败直接返回错误，不静默降级；
- 完成/错误中断只清标志、计数、投递事件，用户回调只经 `pm_spi_poll` 在主循环执行。

完整规则见 [docs/00-contract.md](docs/00-contract.md) 第 11 节。

### I2C（主机、中断状态机）

```c
static pm_i2c_t s_i2c;
static uint8_t  s_reg = 0x71;      /* AHT20 状态寄存器 */
static uint8_t  s_buf[6];

static const pm_i2c_cfg_t i2c_cfg = {
    .hw_id    = BOARD_I2C_1,       /* PB6 SCL / PB7 SDA */
    .clock_hz = 100000,
};

(void)pm_i2c_init(&s_i2c, &i2c_cfg);

size_t done;
pm_err_t rc = pm_i2c_write_read(&s_i2c, 0x70, &s_reg, 1, s_buf, 6, &done, 50);
if (rc == PM_OK) {
    /* done == 7；写段 1 字节 + 读段 6 字节，中间是 repeated START */
}

pm_evt_t evt;
while (pm_i2c_poll(&s_i2c, &evt, NULL, NULL)) {
    /* I2C_DONE / I2C_ERROR */
}
```

- 提供 `pm_i2c_write` / `pm_i2c_read` / `pm_i2c_write_read` 三种事务；
- 不用 HAL 的阻塞版本：字节推进全部由 I2C 事件中断完成，模型只做有界等待；
- `timeout == 0` 时立即返回 `PM_ERR_AGAIN`，缓冲区在 `I2C_DONE/I2C_ERROR` 被 poll 到之前归驱动借用；
- 超时通过 `HAL_I2C_DeInit + Init` 复位外设，`done` 报告已移动字节数；**不做总线清除**；
- 实际 SCL 由 `pm_i2c_clock_hz()` 从 CCR 与 PCLK1 反算，不是回显请求值。

完整规则见 [docs/00-contract.md](docs/00-contract.md) 第 12 节。

### ADC（单通道、单次转换）

```c
static pm_adc_t s_adc;

static const pm_adc_cfg_t adc_cfg = {
    .hw_id       = 0u,                    /* ADC1 */
    .channel     = BOARD_ADC_POT,         /* PA5 */
    .sample_time = PM_ADC_SAMPLE_55CYCLES_5,
    .vref_mv     = 3300u,
};

(void)pm_adc_init(&s_adc, &adc_cfg);

uint16_t raw;
uint16_t mv;
if (pm_adc_read(&s_adc, &raw, 10u) == PM_OK) {
    mv = pm_adc_to_mv(&s_adc, raw);       /* raw * 3300 / 4095 */
}

pm_evt_t evt;
while (pm_adc_poll(&s_adc, &evt, NULL, NULL)) {
    /* ADC_DONE（arg = 原始值）/ ADC_ERROR（arg = HAL 错误掩码） */
}
```

- 单通道、单次、软件触发；没有扫描序列和连续模式；
- `timeout > 0` 有界等待，到期停止转换并返回 `PM_ERR_TIMEOUT`；
- `timeout == 0` 启动后返回 `PM_ERR_AGAIN`，结果从 `ADC_DONE` 事件取；
- NTC 分压源阻抗较高，建议 `PM_ADC_SAMPLE_55CYCLES_5` 或更长；
- PA5 与 SPI1_SCK 复用，用 SPI1 前必须先 `pm_adc_deinit`。

完整规则见 [docs/00-contract.md](docs/00-contract.md) 第 13 节。

### PWM（定时器比较输出）

```c
static pm_pwm_t s_servo;

static const pm_pwm_cfg_t servo_cfg = {
    .hw_id         = BOARD_PWM_SERVO,     /* PB8, TIM4_CH3 */
    .freq_hz       = 50u,
    .duty_permille = 75u,                 /* 1.5 ms */
};

(void)pm_pwm_init(&s_servo, &servo_cfg);  /* 配置后立即输出 */

/* 舵机常用脉宽接口 */
(void)pm_pwm_set_pulse_us(&s_servo, 1500u);

/* 需要时停/开输出，不改配置 */
(void)pm_pwm_enable(&s_servo, false);
(void)pm_pwm_enable(&s_servo, true);
```

- 占空比用千分比（0～1000），不引入浮点；
- `pm_pwm_set_pulse_us` 按当前频率换算，超过一个周期截断到 1000‰；
- **同一定时器的通道共享周期**：PA0/PA1 同属 TIM2，PB8/PB9 同属 TIM4。
  兄弟通道已打开且频率不同时，第二个 `init` 返回 `PM_ERR_BUSY`，不会静默改期；
  因此舵机（50 Hz）与蜂鸣器（约 2 kHz）不能同时在 TIM4 上运行。

完整规则见 [docs/00-contract.md](docs/00-contract.md) 第 14 节。

---

## 与四层架构的关系

这套代码对应四层架构里 **BSP 及其接口** 的部分：

```
APP → components → OSAL → BSP(本仓库的 pm_* 契约) → 芯片后端 → HAL
```

- `pm_osal.h` 是 OSAL 的对外面，裸机实现放在 `src/osal/`；
- `pm_port.h` 是 BSP 的接缝，芯片实现放在 `src/port/`；
- APP 与 components 只 include `include/pm/`。

---

## 已知限制

- DMA 抽象、RTC、Flash、看门狗尚未实现；
- ADC 只做单通道、单次、软件触发；没有扫描、连续转换、注入通道与 DMA；未上板验证；
- PWM 只做比较输出（PWM 模式 1）；同一 TIM 的通道共享周期，舵机与蜂鸣器不能以不同
  频率同时挂在 TIM4；未上板验证；
- SPI 只做主机、8 位帧、单传输（无队列）；MISO 在主机模式下配为输入上拉
  （RM0008 允许输入浮空或上拉），SCK/MOSI 为复用推挽；未接从机，未上板验证；
- 本板 SPI 硬件 NSS 引脚（PA4 / PB12）已被 NTC、KEY1 占用，`PM_SPI_NSS_HARD` 会返回 `PM_ERR_PARAM`，只能用 GPIO 软件片选；
- I2C 只做主机、7 位地址、无 DMA；超时只复位外设，不做 SCL 脉冲总线恢复；
  尚未在真实器件上验证（板载 OLED 0x7A 与 AHT20 0x70 可直接用于上板验证）；
- UART 目前是逐字节中断，尚未提供 DMA + 空闲中断的接收路径；
- port 注册表无条件引用已实现外设的 ops：不用 SPI/I2C 也会链接对应驱动，后续可按需裁剪；
- 尚未接入 CMake presets，尚未提供 CI；
- 对象结构体布局不承诺 ABI 稳定。

## 第三方代码

`third_party/` 是编译所需的最小集合，文件内容未修改，每个源文件都保留 ST/ARM 原始的版权与许可头：

| 组件 | 来源与版本 | 许可 |
| --- | --- | --- |
| STM32F1 HAL Driver | 1.1.10，随 STM32CubeF1 1.8.0（含 patch）分发 | BSD-3-Clause |
| CMSIS Core（ARM） | 随 STM32CubeF1 1.8.0 分发 | Apache-2.0 |
| CMSIS Device STM32F1xx | 随 STM32CubeF1 1.8.0 分发 | Apache-2.0 |

版权归 STMicroelectronics / ARM Limited 各自所有者。完整许可矩阵见
[third_party/ST_LICENSE.md](third_party/ST_LICENSE.md)，来源与替换说明见
[third_party/README.md](third_party/README.md)。本仓库自带链接脚本与启动配置，
未复制 ST 的链接脚本模板（其条款禁止再分发）。
