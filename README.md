# STM32F1 外设通用模型

## 这是什么

一套按"接口契约优先"方式组织的 STM32F1 外设代码：上层只依赖 `include/pm/` 里的公共 API，芯片相关的一切都收敛在 port 层。目标是换板子只改一张引脚表，换芯片只换一个 port 目录，核心逻辑永远能在主机上跑单测。

当前版本 **v0.7.0**，配套 STM32F103C8T6 最小系统板（72 MHz、20 KB RAM、64 KB Flash）。
已实现 GPIO、UART、SPI（主机全双工，中断 + 可选 DMA）、I2C（主机，中断状态机，支持写读合并；repeated START 仅适用于寄存器型器件）、ADC（单通道单次转换，EOC 中断）、PWM（定时器比较输出）、软件音调（TIM3 中断翻转）、循迹（单路数字量）、正交编码器（AB x4，可选按键输入）、继电器（单路 GPIO，极性必填）、电机（DRV8833 形态，双 PWM 输入，含滑行/刹车两种停止）与舵机（角度↔脉宽映射，独立成模型）。

> **使用前必读**：`include/pm/*.h` 里的 `struct pm_xxx` 字段（`magic`、`state` 等）是公开可见的——
> 对象由调用方静态分配。但**结构体布局不承诺 ABI 稳定**：跨版本不要按字段偏移序列化，
> 也不要直接改字段。`PM_ERR_BUSY` / `PM_ERR_STATE` 等生命周期校验依赖 `magic`/`state`，
> 手改字段会让校验失效、行为不可预期。只使用 `pm_*()` 接口。

## 快速开始

不需要硬件，也不需要 ARM 工具链：

```powershell
git clone https://github.com/BiscuiTs2026/stm32-peripheral-model.git
cd stm32-peripheral-model
cmake -S tests/host -B build-host
cmake --build build-host --config Debug
ctest --test-dir build-host -C Debug --output-on-failure
```

期望输出 `689 checks, 0 failure(s)`，退出码 0。

最小单测示例（加到 `tests/host/test_main.c`，并在 `main()` 里调用）：

```c
static void test_ringbuf_minimal(void)
{
    uint8_t storage[8];
    pm_ringbuf_t rb;

    CHECK(pm_rb_init(&rb, storage, 8) == PM_OK);
    CHECK(pm_rb_free(&rb) == 7);    /* 8 字节缓冲留一格区分空/满 */
    CHECK(pm_rb_used(&rb) == 0);
}
```

`CHECK` 宏自己计数，失败时打印文件名与行号；断言总数变化时同步更新 README 里的断言数（当前 689）。

## 能做什么

- 驱动 GPIO、UART、SPI（主机全双工）、I2C（主机，可选错误分类计数）、ADC（单通道单次）、
  PWM（定时器比较输出）、软件音调、循迹、正交编码器（可选按键）、继电器、电机（DRV8833）、
  舵机（角度接口）；
- 定时器是共享资源：模块在 init 时声明 TIM/PSC/ARR/通道，冲突返回 `PM_ERR_CONFLICT`，
  不会静默覆盖别人的时基；端口还会探测"被非模型代码启动/重配"的定时器，遇到即拒绝
  （`PM_ERR_CONFLICT`，见 `docs/00-contract.md` §20）；
- 异步接口统一走事件队列：ISR 只投递事件，用户回调只在 `pm_*_poll()` 的主循环上下文执行；
- 静态分配、无 `malloc`；错误码为 `pm_err_t` 负值；超时语义统一（0 = 立即返回 `PM_ERR_AGAIN`，>0 = 有界等待）；
- 换板子只改 `platform/stm32f103/board_f103c8.c` 的引脚表，换芯片只换一个 port 目录；
- 核心逻辑可在 PC 上用内存 mock 后端跑主机单测，不需要硬件和 ARM 工具链。

## 不能做什么

**未上板验证的外设**（已实现、已编译、有主机单测，但没接真实器件/负载，不能当作"可用"）：

| 外设 | 尚未验证的部分 |
| --- | --- |
| SPI | 未接从机；只验证了主机初始化、中断/DMA 收发路径与主机 mock |
| I2C | 已用板载 AHT20（0x70）验证读时序；OLED（0x7A）未验证 |
| ADC | 未接真实信号源；只验证了转换流程与主机 mock |
| PWM | 未接负载（电机 / 舵机）；只验证了配置与占空比换算 |
| 软件音调 | 未上板；主机 mock 覆盖生命周期与参数校验，声学效果未验证 |
| 循迹 | 未上板；只验证了极性判定与 PB14/SPI2 互斥逻辑 |
| 编码器 | 未上板；只验证了 AB x4 解码表与可选按键语义（主机单测）；按键实测见板端工程 |
| 继电器 | 未上板；只验证了极性映射、失效安全与两实例重入（主机 mock） |
| 电机 | 未上板；只验证了方向/占空比组合、滑行与刹车、频率冲突（主机 mock）；形态仅适用 DRV8833 |
| 舵机 | 未接真实舵机；只验证了角度↔脉宽换算、帧率约束与通道冲突（主机 mock） |

已在真实板卡上验证的只有：时钟（HSE→PLL 72 MHz）、UART 收发，以及 I2C 对 AHT20 的读时序。

此外当前不提供：DMA 通用抽象、RTC、Flash、看门狗、FreeRTOS 集成、CMake presets；
同一硬件定时器的通道共享周期，不同频率同时使用会返回 `PM_ERR_BUSY`，跨模块抢同一颗
定时器返回 `PM_ERR_CONFLICT`。

---

## 验证状态

| 项目 | 状态 | 证据 |
| --- | --- | --- |
| 时钟 | HSE→PLL 72 MHz | `pm_clock_init()` 配置；HSE 起振失败回落 HSI 8 MHz，`pm_platform_core_clock()` 可读实际主频 |
| ARM 交叉编译 | 通过 | `arm-none-eabi-gcc 14.3.1`，无项目代码警告 |
| 链接 | 通过 | Flash 28536 B / 64 KB（43.54%），RAM 3376 B / 20 KB（16.48%） |
| 产物 | 已生成 | `pm_f103_demo`（ELF）/ `.bin`（28536 B）/ `.hex` / `.map` |
| 主机单测 | 通过 | 689 项断言，0 失败（MSVC 19.51，C11） |
| 动态分配 | 不存在 | `nm` 中无 `malloc` / `free` / `_sbrk` |
| 向量表 | 正确 | 首字 `0x20005000`（20 KB RAM 顶），Reset_Handler `0x08000229`（Thumb） |
| **上板运行** | **部分验证** | 时钟与 UART 收发已在真实板卡上验证；I2C 已用板载 AHT20（0x70）验证触发/读取时序；SPI、ADC、PWM、音调、循迹、编码器、继电器、电机、舵机未接真实器件/负载 |

未做上板验证的部分不写成"可用"。

**I2C 器件事实**（针对本板）：本板 I2C1 上的 OLED（0x7A，驱动芯片 CH1116）与 AHT20
温湿度传感器（0x70）都是**板载器件**，不是外部模块；另有一个 I2C 排针与两者共用 PB6/PB7。
因此 I2C 可以直接用板载器件验证。AHT20 已用于验证"命令触发型器件"的时序
（触发写 + STOP → 等待 → 读，见 `docs/00-contract.md` §12.3）；OLED 尚未验证。

### Flash/RAM 增量

| 版本 | 内容 | Flash | RAM | 相对上一版 |
| --- | --- | --- | --- | --- |
| v0.1 | GPIO + UART | 7848 B（11.98%） | 2224 B（10.86%） | — |
| v0.2 | + SPI（IT + DMA） | 13104 B（20.00%） | 2728 B（13.32%） | +5256 B / +504 B |
| v0.3 | + UART RX 与 SPI MISO 引脚修复、I2C | 18896 B（28.83%） | 2840 B（13.87%） | +5792 B / +112 B |
| v0.4 | + ADC（单次转换）与 PWM（TIM2/TIM4 比较输出） | 24424 B（37.27%） | 3104 B（15.16%） | +5528 B / +264 B |
| v0.5 | + 软件音调 / 循迹 / 编码器 / 定时器仲裁 | 28048 B（42.80%） | 3376 B（16.48%） | +3624 B / +272 B |
| v0.6 | + 定时器外部占用探测 / 编码器可选按键 / 舵机 / I2C 错误分类 | 28436 B（43.39%） | 3384 B（16.52%） | +388 B / +8 B |

v0.6 一行取自本次工作树的构建输出（提交后可用同一命令复现）；增量主要是 `pm_servo`
组合层、`pm_timer` 探测钩子与 `pm_i2c` 分类/计数，RAM 只增一个对象字段的尾部填充。

2026-11-20 复测（同口径：`gnu-tools-for-stm32 14.3.1+st.2`、`-Os -g3
-ffunction-sections -fdata-sections`、全新临时构建目录）：FLASH 28536 B（43.54%）、
RAM 3376 B（16.48%）。比 v0.6 一行多 100 B（时基改 64 位交叉相乘 +44 B，PWM 通道减到
3 路并补 tone 侧检查 +56 B），RAM 少 8 B（少一个通道状态槽）。

v0.5 一行取自本次工作树的构建输出（提交后可用同一命令复现）；增量主要是 HAL TIM 更新
中断路径、三组引脚/上下文表与 `pm_timer` 静态占用表（8 条）。

v0.1–v0.4 四行都在对应提交上重新 checkout、用全新构建目录实测，不是按差值推算。旧表中的
v0.1–v0.3 三行（6440 / 11704 / 18872 B）无法复现，已按实测值更正；时钟代码
（HSE→PLL）在四个版本中相同，不再计入 v0.3 的增量。

v0.3 的 +5792 B 主要来自 HAL I2C 状态机（`HAL_I2C_EV_IRQHandler` 2228 B、
`HAL_I2C_Init` 336 B、`HAL_I2C_ER_IRQHandler` 320 B、四个 `Master_*_IT` 共 828 B）
与模型/端口胶水（约 1 KB），其余为零星代码差异。
RAM 的 +112 B 是静态 `I2C_HandleTypeDef` 与端口状态（`s_i2c`，`nm` 显示 0x70）。
模块是否参与构建由 CMake 按"头文件 + 核心源文件是否存在"判定；模块存在就会被注册，
**即使 APP 不用 I2C/SPI，这部分代码也会被链接**（v0.4 四个模块都在）。

v0.4 的 Flash 增量来自 HAL ADC（`HAL_ADC_Init`、`HAL_ADC_Start_IT`、
`HAL_ADC_IRQHandler`、`HAL_ADCEx_Calibration_Start`）与 HAL TIM PWM
（`HAL_TIM_PWM_Init`、`HAL_TIM_PWM_ConfigChannel`、`HAL_TIM_PWM_Start/Stop`），
以及模型/端口胶水。RAM 的 +264 B 主要是 `ADC_HandleTypeDef`、`TIM_HandleTypeDef`
与端口静态状态（`s_adc`、`s_pwm_timer[2]`、`s_pwm[4]`）。同样地，APP 不用 ADC/PWM
时这部分也会被链接。

> **复核命令**（在仓库里把 `<commit>` 换成对应版本，工具链路径按本机调整）：
>
> ```powershell
> git switch --detach <commit>
> cmake -S . -B build-arm -G Ninja `
>       -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/arm-gcc-toolchain.cmake" `
>       -DTOOLCHAIN_ROOT="<cube>/gnu-tools-for-stm32/14.3.1+st.2/bin"
> cmake --build build-arm
> arm-none-eabi-size build-arm/pm_f103_demo
> ```
>
> Flash/RAM 取链接器打印的 `Memory region` 表（`.bin` 大小与 Flash 占用一致）。
> 对应提交：v0.1 `aa342e5`、v0.2 `917cc09`、v0.3 `7bde865`；v0.4 见 `main` 历史；
> v0.5 `76e4863`、`9b393e6`；v0.6 末提交 `4e0ae8d`。

---

## 目录与职责

```
docs/00-contract.md     接口契约（规范，先读这个）
docs/01-layering.md     目录、依赖、时序、移植
docs/02-changelog.md    结构性变更记录与文件移动留痕
include/pm/             公共头，无厂商类型
src/core/               与硬件无关的模型（可在主机编译）
src/common/             与硬件无关的纯逻辑（时基数学、定时器仲裁），任何层可依赖
src/osal/               Cortex-M3 的时基与临界区
src/port/stm32f1/       HAL 后端、UART/SPI/I2C/ADC/PWM/tone/track/encoder ISR、pm_port_get()
platform/stm32f103/     板级引脚表、启动文件、链接脚本、HAL 配置
tests/host/             内存 mock 后端 + 主机单测
third_party/            CMSIS 与 ST HAL 子集（BSD-3-Clause，原样分发）
```

---

## 接进已有工程

三个约定先记住：

1. **包含路径是 `-Iinclude/pm`，不是 `-Iinclude`**。公开头之间用 `#include "pm_common.h"`
   这类同目录写法，必须把 `${PM_ROOT}/include/pm` 加进 include path；`${PM_ROOT}/include`
   一并加上（个别头会引用它）。
2. **要编译的目录**：`src/common/`（纯逻辑）、`src/core/`（模型）、`src/osal/`（时间与临界区）、
   `src/port/stm32f1/`（HAL 后端）与 `platform/stm32f103/`（引脚表等）。
3. **两处接缝要实现**：`pm_port_get()` 返回各外设的 ops 表（定义见 `include/pm/pm_port.h`）；
   OSAL 直接用 `src/osal/pm_osal_port_cm3.c`（Cortex-M3），换架构另写一份
   （`pm_osal_*` 契约见 `docs/00-contract.md` §9）。

CMake 最小集成（假设本仓 vendored 到 `third_party/stm32-peripheral-model`）：

```cmake
set(PM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stm32-peripheral-model")

file(GLOB PM_CORE   ${PM_ROOT}/src/core/*.c)
file(GLOB PM_COMMON ${PM_ROOT}/src/common/*.c)

add_library(pm STATIC
    ${PM_CORE} ${PM_COMMON}
    ${PM_ROOT}/src/osal/pm_osal_port_cm3.c
    ${PM_ROOT}/src/port/stm32f1/pm_port_stm32f1.c
    ${PM_ROOT}/platform/stm32f103/board_f103c8.c
    ${PM_ROOT}/platform/stm32f103/pm_platform.c
)
target_include_directories(pm PUBLIC
    ${PM_ROOT}/include/pm
    ${PM_ROOT}/include
    ${PM_ROOT}/platform/stm32f103
)
target_compile_definitions(pm PUBLIC USE_HAL_DRIVER STM32F103xB)
# HAL/CMSIS 的头路径与链接脚本见本仓 CMakeLists.txt，按你的工程改
```

APP 侧只 include 公共头与板级表：

```c
#include "pm_uart.h"
#include "board_f103c8.h"
```

- 换板子只改 `platform/stm32f103/board_f103c8.c` 的引脚表，其余不动
  （[01-layering.md §9](docs/01-layering.md)）；
- 换芯片新写 `src/port/<chip>/` 与一份 OSAL，`src/core` 与 `include/pm` 不动（§10）；
- 只做主机测试时不需要 HAL：照 `tests/host/CMakeLists.txt`，用 `pm_port_mock.c` +
  `pm_osal_host.c` 顶替后端。

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
ctest --test-dir build-host -C Debug --output-on-failure
```

`ctest` 与直接跑 `build-host/Debug/pm_host_tests.exe` 等价，二者之一即可；
输出形如 `689 checks, 0 failure(s)`，退出码 0 表示全过。

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
- 实际 SCL 由 `pm_i2c_clock_hz()` 从 CCR 与 PCLK1 反算，不是回显请求值；
- 默认所有硬件错误都返回 `PM_ERR_IO`；`pm_i2c_set_error_counting(&s_i2c, true)` 后，
  AF 返回 `PM_ERR_NACK`，BERR/ARLO/OVR 返回 `PM_ERR_BUS`，计数用 `pm_i2c_get_error_counts` 取；
  开启时计数先清零，`stats.last_error` 与事件 `arg` 仍是原始掩码（契约 §12.7）；
- `pm_i2c_write_read` 的 repeated START 只适用于**寄存器型**器件（写寄存器地址后直接读）；
- **命令触发型**器件（AHT20 等：写一条命令即开始内部转换）必须
  `pm_i2c_write`（STOP 结束）→ `pm_osal_delay_ms()` → `pm_i2c_read`。
  用 `write_read` 会读到上一次结果，并可能带假忙位（板端实测见 `docs/00-contract.md` §12.3）。

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

- 占空比用千分比（0～1000），不引入浮点；对外协议是百分数（0～100），由模型侧适配层按
  `pct × 10` / `permille ÷ 10` 换算（契约 §14.1，决策 0018）；
- 时基由端口决定（本板舵机 PSC 71 / ARR 19999、电机 PSC 0 / ARR 3599，均已实测）；
  端口写寄存器前必须用 `pm_pwm_timing_matches()` 在 10‰ 容差内复核，`timing_compute()` 只是可选工具；
- `pm_pwm_set_pulse_us` 按当前频率换算，超过一个周期截断到 1000‰；
- **同一定时器的通道共享周期**：PA0/PA1 同属 TIM2，PB8 是 TIM4 在本板唯一开出的通道。
  兄弟通道已打开且频率不同时，第二个 `init` 返回 `PM_ERR_BUSY`，不会静默改期；
  蜂鸣器走 TIM3 软件音调（见下文"音调 / 循迹 / 编码器"），不占 TIM4，与舵机可同时运行。

完整规则见 [docs/00-contract.md](docs/00-contract.md) 第 14 节。

### 舵机（角度接口，v0.6）

```c
static pm_servo_t s_sg90;

static const pm_servo_cfg_t servo_cfg = {
    .hw_id = BOARD_PWM_SERVO,             /* 仍是一个 PWM 通道 */
    .freq_hz = 50u,
    .range = {
        .angle_min_deg  = PM_SERVO_SG90_ANGLE_MIN,   /* 0   */
        .angle_max_deg  = PM_SERVO_SG90_ANGLE_MAX,   /* 180 */
        .pulse_min_us   = PM_SERVO_SG90_PULSE_MIN_US,/* 500 */
        .pulse_max_us   = PM_SERVO_SG90_PULSE_MAX_US,/* 2500 */
    },
    .start_angle_deg = 90u,               /* 必填：init 立即驱动到这个位置 */
};

(void)pm_servo_init(&s_sg90, &servo_cfg);
(void)pm_servo_set_angle(&s_sg90, 45u);   /* 越界返回 PM_ERR_RANGE，不夹紧 */

uint16_t us;
(void)pm_servo_pulse_from_angle(&servo_cfg.range, 45u, &us);  /* 纯函数，可单测 */
```

- 换 SG90 以外的舵机只改 `range` 四个端点，逻辑不动；
- `freq_hz` 必须容得下 `pulse_max_us`（1 ms 帧装不下 2.5 ms 脉宽时 init 返回 `PM_ERR_RANGE`）；
- 越界不夹紧、不静默改角度，上一次位置保持不变；
- 与 `pm_pwm` 共用定时器仲裁：同一定时器的通道必须同频率。

完整规则见 [docs/00-contract.md](docs/00-contract.md) 第 24 节。

### 软件音调 / 循迹 / 编码器

```c
static pm_tone_t    s_tone;
static pm_track_t   s_track;
static pm_encoder_t s_enc;

/* 蜂鸣器：TIM3 更新中断翻转 PB9，接口只有频率与占空比 */
static const pm_tone_cfg_t tone_cfg = {
    .hw_id = BOARD_TONE_BUZZER, .freq_hz = 2000u, .duty_permille = 500u,
};
(void)pm_tone_init(&s_tone, &tone_cfg);
(void)pm_tone_set(&s_tone, 1000u, 500u);
(void)pm_tone_off(&s_tone);          /* 静音；不是占空比 0 */

/* 循迹：PB14 数字量，active_level 是配置而不是猜测 */
static const pm_track_cfg_t track_cfg = {
    .hw_id = BOARD_TRACK_LEFT, .active_level = PM_GPIO_LEVEL_LOW,
};
(void)pm_track_init(&s_track, &track_cfg);
bool on_line = false;
(void)pm_track_read_raw(&s_track, &on_line);

/* 编码器：PA8/PA9 双沿，计数在 core 里解码；按键是可选能力 */
static const pm_encoder_cfg_t enc_cfg = {
    .hw_id = BOARD_ENCODER_LEFT, .invert = 0u,
    .has_button = 1u,                /* 0（或不写）= 没接按键，查询返回 PM_ERR_DISABLED */
};
(void)pm_encoder_init(&s_enc, &enc_cfg);
int32_t ticks = pm_encoder_count(&s_enc);

bool pressed = false;
if (pm_encoder_button(&s_enc, &pressed) == PM_OK && pressed) {
    /* 按键按下；极性由端口定义（本板 KY-040 低 = 按下） */
}
```

- 音调的 tick 长度、占空比量化是端口实现细节，**不要在应用里依赖**；
- 音调与 PWM 后端按引脚互斥：若板级表另行给 PB9 加回 PWM 通道，两个后端同时 open 时
  后开的返回 `PM_ERR_CONFLICT`（两侧都检查，不依赖调用顺序）；本板默认只有软件音调，
  PB9 没有 PWM 通道；
- 循迹与 SPI2 在 PB14 上互斥，后开的返回 `PM_ERR_CONFLICT`；
- 编码器 `count` 由 ISR 写、主循环读；`pm_encoder_reset()` 不是中断安全的；
- 按键未配置时 `pm_encoder_init` 仍成功，`pm_encoder_button` 返回 `PM_ERR_DISABLED` 且
  `*pressed = false`——"没装"不是故障；本板按键在 PB15，与 SPI2_MOSI 复用，不能同时用；
- 三者都走定时器/引脚仲裁，规则见契约 §17–§20。

---

## 与四层架构的关系

这套代码对应四层架构里 **BSP 及其接口** 的部分：

```
APP → components → OSAL → BSP(本仓库的 pm_* 契约) → 芯片后端 → HAL
```

- `pm_osal.h` 是 OSAL 的对外面，裸机实现放在 `src/osal/`；
- `pm_port.h` 是 BSP 的接缝，芯片实现放在 `src/port/`；
- `common`（`include/pm/pm_common.h`、`pm_err.h`、`pm_pwm_math.h`、`pm_timer.h`，
  实现 `src/common/`）是任何层可依赖的纯逻辑：错误码、定点数学、定时器仲裁；
  common 不依赖任何层，也不得出现寄存器访问、HAL 调用或厂商头；
- APP 与 components 只 include `include/pm/`。

---

## 四层架构检查与 CI

`tools/check_layering.py` 不是本仓写的，是
[firmware-four-layer-architecture](https://github.com/BiscuiTs2026/firmware-four-layer-architecture)
检查器的**单文件拷贝**（上游提交 `af88491`，文件自述 v3.2.0）。来源与同步信息记在
`tools/CHECKER.lock`（版本、来源仓、来源提交、上游 blob、同步日期、本地补丁）；本仓除
文件头 `source-commit` 与 `SOURCE_COMMIT` 常量外不改脚本，**不修改判定规则**。

本地运行（与 CI 门禁等价，两条都要过）：

```bash
python3 tools/check_layering.py --version --lock tools/CHECKER.lock
python3 tools/check_layering.py . --roles .arch-roles
```

第一条比对文件头与锁文件，任一不一致退出 `1`。扫描命令的退出码：
- 退出码 `0` = ERROR 数为 0；`1` = 存在 ERROR（或工程完全没有分层信息）；`2` = 用法/路径错误。
- **ERROR** 是要修的问题：越层调用、未获准的反向依赖、跨模块全局变量、ISR 内阻塞调用、
  链接脚本固定段冲突等。
- **WARN** 是"需要人看，但按当前设计有意接受"的形态，只打印，不影响退出码与合并。

当前基线：**0 ERROR / 18 WARN**。这 18 条 WARN 是同一类设计形态，不是待修问题：

- 8 条：`src/port/stm32f1/pm_port_stm32f1.c` 第 29/33/36/39/42/45/48/51 行 include 组件层
  头文件（`pm_uart.h` 等八个模块），只取句柄类型与 ISR 钩子声明，不调用组件公开 API；
- 10 条：同文件第 391/401/413/780/792/1161/1165/1385/1394/2428 行调用 `pm_*_hw_*`
  ISR 钩子，这些钩子按契约只允许端口层在中断上下文调用。

即：WARN 记录的是"依赖倒置 + 中断回投"这两条设计选择。WARN 数量或内容变化时，
先确认没有新增真实的越层调用，再同步更新本节。

**v0.5 修过一次真实的越层**：PWM 时基数学（`pm_pwm_timing_*`）与定时器仲裁（`pm_timer_*`）
原先放在 `src/core/`（components），而端口必须直调它们，检查器报了 6 条 ERROR。
修法是把契约头（`pm_pwm_math.h`、`pm_timer.h`）与实现（`src/common/`）判为 `common`，
不是加转发函数。这 6 条当时只在工作树里、尚未提交；一旦提交，layering 作业会因退出码 1
而失败——说明门禁有效，`ci.yml` 无需改动。

`.arch-roles` 用两条 `ignore` 跳过非本仓代码：`ignore = third_party/`（CMSIS 与 ST HAL
子集，厂商代码不参与分层，上游也推荐忽略而非映射成 `bsp`）与 `ignore = build-*/`
（CMake 构建目录）。忽略范围只有这两类，不含任何本仓源码或测试。
实测扫描耗时从 42.7 s 降到 0.24 s（三次 235 / 236 / 248 ms），ERROR/WARN 计数与逐条内容不变。

### 升级检查器

1. 在上游仓库取新提交（**用 `git worktree add --detach <commit>` 或
   `git show <commit>:skill/scripts/check_layering.py`，不要拷别人正在编辑的工作副本**）；
2. 整文件覆盖 `tools/check_layering.py`；
3. 改文件头 `source-commit` **和 `SOURCE_COMMIT` 常量**——`--version` 打印的是常量，
   只改注释会被锁文件校验抓到；
4. 更新 `tools/CHECKER.lock`：`checker-version`、`source-commit`、`source-blob`
   （`git -C <上游仓> rev-parse <commit>:skill/scripts/check_layering.py`）、`synced-at`；
   `local-patches` 保持 `none`，确要本地改就在此写清原因与上游 issue；
5. 本地跑上面两条命令：版本一致、ERROR 必须为 0；WARN 有变化就核对是否仍只覆盖上面两类形态；
6. 提交信息写明旧版本 → 新版本与上游提交哈希。

### CI

`.github/workflows/ci.yml` 三个作业并列，任一失败都视为不可合并：

| 作业 | Runner | 内容 |
| --- | --- | --- |
| 主机单测 | windows-latest | 构建 `tests/host`，用 `ctest` 运行；失败即非零退出 |
| 固件构建 | ubuntu-latest | 固定下载 **GCC 14.3.rel1**（`arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi`，经 `TOOLCHAIN_ROOT` 传入）+ CMake/Ninja 构建 `pm_f103_demo` |
| 四层架构检查 | ubuntu-latest | 先 `--version --lock` 校验无漂移，再扫描；任一步非零即失败 |

CI 的固件工具链钉在 14.3.rel1，与本地 `gnu-tools-for-stm32 14.3.1+st.2`（ST 打包）同主版本；
若该下载地址日后失效，需同步改 `ci.yml` 与本节，不要让版本静默漂移。

**未在本地实测的部分**：主机作业的 `ctest` 命令在本地 Windows 跑过（1/1 通过），但作为
Actions 步骤尚未在 CI 中执行过；固件作业的 14.3.rel1 下载地址与工具链包本地无法获取，
同样未实测。两者首次生效都以 CI 实际结果为准，不要当作已验证。

**项目约定：编译告警即错误，不许为"编过"关掉。** 主机单测用 `/W4`（MSVC）或
`-Wall -Wextra -Wshadow -Werror`（GCC），固件构建同样把隐式声明当错误。v0.5 就有一个
真实例子：`pm_port_stm32f1.c` 用了 `memset` 但漏 `#include <string.h>`，被
`implicit-function-declaration` 挡下才没有进版本。这类漏 include 在"能编过"的宽松配置下
只表现为未定义行为，**告警开关是唯一防线**，任何 PR 都不得关闭或降级这类选项。

---

## 已知限制

- DMA 抽象、RTC、Flash、看门狗尚未实现；
- ADC 只做单通道、单次、软件触发；没有扫描、连续转换、注入通道与 DMA；未上板验证；
- PWM 只做比较输出（PWM 模式 1）；同一 TIM 的通道共享周期，TIM2 两路电机必须同频；
  本板 TIM4 只开舵机一路（蜂鸣器走 TIM3 软件音调，不占 TIM4）；未上板验证；
- 软件音调只做单音方波，不做包络/音量/多音混合；每 10 µs 一次更新中断，满频 5 kHz 时
  中断率约 100 kHz；本板 PB9 没有 PWM 通道，仅板级表另行加回时才与音调按引脚互斥；
  未上板，声学未验证；
- 循迹只做单路数字量，不做多路阵列、阈值自适应与去抖；与 SPI2 在 PB14 上互斥；未上板；
- 编码器只做 AB x4 计数，不做测速、Z 相与方向滤波；`reset` 非中断安全；按键是可选能力，
  本板 PB15 与 SPI2_MOSI 复用；未上板；
- 舵机只做角度→脉宽映射（整数、四舍五入到 µs），不做加减速、多舵机同步与反馈闭环；
  依赖的仍是 `pm_pwm` 通道，同一定时器同频率的约束不变；未接真实舵机；
- 定时器仲裁表固定 8 条、按 `hw_id` 逻辑编号；只判"能不能共存"，不做自动重分配；
  参数兼容判据只有"同 PSC/ARR + 通道不重叠"一种；端口探测能发现 CEN 已置位或重映射位
  非零的裸占用，但"外部代码把引脚改了而没开定时器"这类占用看不见；
- SPI 只做主机、8 位帧、单传输（无队列）；MISO 在主机模式下配为输入上拉
  （RM0008 允许输入浮空或上拉），SCK/MOSI 为复用推挽；未接从机，未上板验证；
- 本板 SPI 硬件 NSS 引脚（PA4 / PB12）已被 NTC、KEY1 占用，`PM_SPI_NSS_HARD` 会返回 `PM_ERR_PARAM`，只能用 GPIO 软件片选；
- I2C 只做主机、7 位地址、无 DMA；超时只复位外设，不做 SCL 脉冲总线恢复；
  错误分类（AF/BERR/超时等）是可选功能、默认关闭，关闭时一律 `PM_ERR_IO`；
  AHT20（0x70）已验证命令触发型时序，板载 OLED（0x7A）未验证；
- UART 目前是逐字节中断，尚未提供 DMA + 空闲中断的接收路径；
- port 注册表无条件引用已实现外设的 ops：不用 SPI/I2C 也会链接对应驱动，后续可按需裁剪；
- 尚未接入 CMake presets（CI 已接入，见「四层架构检查与 CI」）；
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

## 许可

本仓库自有代码以 MIT 许可发布，见 [LICENSE](LICENSE)。
`third_party/` 下的文件保留各自原始许可，不因本仓的 MIT 许可而改变。
