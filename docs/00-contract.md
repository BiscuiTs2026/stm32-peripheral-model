# 外设模型接口契约

本文件是这套代码的**规范**，不是说明性文档。上层代码只依赖这里写明的行为；实现可以换（STM32F1、其它芯片、主机 mock），行为不许变。

版本：v0.4（第一阶段 GPIO + UART；第二阶段 SPI、I2C、ADC、PWM）

---

## 1. 适用范围与非目标

**适用**：单核 Cortex-M 裸机、无 RTOS、无动态内存、单生产者/单消费者。

**非目标（本版本明确不做）**：

- 多核、SMP、RTOS 任务间同步；
- 一个外设实例被多个线程或任务同时写；
- 运行期创建/销毁对象、动态缓冲区、`malloc`；
- 热插拔、电源管理、低功耗模式的自动切换；
- 跨芯片的 ABI 兼容（对象结构体布局属于实现细节，跨版本可能变）。

明确不做，是为了让契约可被静态检查、可在主机上单测。

---

## 2. 分层与依赖方向

```
APP           应用逻辑、调度、业务状态机
 │  只能调 pm_* 公共 API
 ▼
pm_core       外设模型：状态机、环形缓冲、事件队列、参数校验
 │  只能调 pm_port_get() 拿到的 ops 表 + pm_osal
 ▼
pm_port       芯片移植层：把 ops 映射到 HAL/寄存器（或主机 mock）
 │
 ▼
HAL / 寄存器   厂商代码，只允许出现在 port 目录内

common        任何层都可依赖的纯逻辑（不在上图的主干上，横向挂载）
```

`common` 的语义（按此判定，不按印象判定）：

> common = 与硬件无关的纯逻辑：错误码、定点/数学运算、资源占用仲裁；
> **不得包含**寄存器访问、HAL 调用、`#include` 任何厂商头、以及任何针对具体外设的行为。

依赖方向两条：**任何层可依赖 common；common 不依赖任何层。**
落盘位置：契约头 `include/pm/pm_common.h`、`include/pm/pm_err.h`、
`include/pm/pm_pwm_math.h`、`include/pm/pm_timer.h`；实现 `src/common/*.c`；
角色名 `common`（见 `.arch-roles`）。

为什么这几项要放进 common：PSC/ARR 规则与定时器占用仲裁必须由端口在写寄存器之前
直调，只有一份实现；放进 `pm_core`（components）会让 bsp 变成反向依赖。

硬性规则：

1. **向下依赖，禁止反向 include**。`pm_core` 不得 include 任何厂商头或 port 内部头。
2. **厂商类型不出 port**。`GPIO_TypeDef`、`UART_HandleTypeDef`、`USART_TypeDef` 不得出现在 `include/pm/` 下的任何头文件里。
3. **上层不 cast 句柄**。`pm_gpio_pin_t` 只有一个 `id` 字段，其含义由板级表定义，上层不得假设 id 与物理引脚号的对应关系。
4. **ISR 反向通知走内部钩子**。port 的 ISR 只能调 `pm_uart_hw_*`，不得直接改模型字段、不得调用用户回调。
5. **只有一个 `pm_port_get()`**。同一份固件里由链接决定后端，不同时存在两个端口实现。
6. **端口只能向上要 common，不能向 components 要实现**。端口可 include components 头里的类型与 `*_hw_*` 钩子（检查器记为 WARN），但不得调用 components 的普通函数；跨层复用一律下沉到 common。

依赖矩阵：

| 层 | 允许依赖 | 禁止依赖 |
| --- | --- | --- |
| APP | `include/pm/*` | port、HAL、寄存器 |
| pm_core | `include/pm/*`、`pm_osal.h`、common | port 内部头、HAL |
| pm_port | `pm_port_get` 的义务、common、HAL、CMSIS | APP、components 的普通函数 |
| board | port 内部头、芯片头、common | APP、pm_core |
| common | 仅 C 标准库与同层头 | 其余所有层、厂商头、寄存器、HAL |

`board` 行在 `.arch-roles` 中判为 `bsp`：板级引脚/外设资源表（`platform/*/board_*`）属于
端口层数据，`glue` 只消费、不定义。启动文件、链接脚本、HAL 配置与演示入口仍为 `glue`。

---

## 3. 错误码模型

全库只有一个错误枚举 `pm_err_t`（`pm_err.h`）：

| 值 | 含义 | 调用方应做什么 |
| --- | --- | --- |
| `PM_OK` (0) | 成功 | — |
| `PM_ERR_PARAM` | 参数值非法（波特率、数据位、大小非 2 的幂等） | 改配置，不要重试 |
| `PM_ERR_NULL` | 必填指针为空 | 修 bug |
| `PM_ERR_RANGE` | id / 索引超出板级表范围 | 改调用，不要重试 |
| `PM_ERR_TIMEOUT` | 有界等待超时，可能已有部分进度（看 `*written`） | 由上层决定是否重试 |
| `PM_ERR_BUSY` | 资源已被占用（重复 init、上一块未发完） | 等待，不要忙轮询 |
| `PM_ERR_IO` | 硬件报错 | 查 `pm_uart_stats_t` |
| `PM_ERR_STATE` | 生命周期不对（未 init 就读写、端口未注册） | 修调用顺序 |
| `PM_ERR_NOMEM` | 固定池耗尽 | 调大静态缓冲 |
| `PM_ERR_AGAIN` | 非阻塞调用当前无法推进 | 下一轮主循环再试 |
| `PM_ERR_OVERFLOW` | 数据被丢弃（队列满） | 看 dropped 计数 |

规则：

- 错误码是**负值**，`if (rc < 0)` 一律等价于失败；`PM_OK == 0`。
- **不允许新增未登记的返回码**。模块内部状态用私有枚举，出口统一翻译成 `pm_err_t`。
- 失败时必须给出可观测证据：要么返回 `PM_ERR_*`，要么计数加一。**不允许静默失败**。
- 参数校验发生在**进入硬件之前**。非法参数不得触碰寄存器。

---

## 4. 对象、句柄与静态分配

模型对象全部由**调用方静态分配**，库内部不使用 `malloc/free`（链接产物里 `_sbrk`、`malloc` 均不存在，见 README 的验证记录）。

```c
static pm_uart_t s_uart;          /* 或 pm_uart_t u = {0}; 局部静态 */
static uint8_t   s_rx[256];       /* 2 的幂，>=32 */
static uint8_t   s_tx[512];       /* 2 的幂，>=64 */
```

约定：

1. `pm_uart_t` 必须**零初始化**（静态存储或 `= {0}`）。`pm_uart_init` 用 `magic` 字段判断对象是否已属于某个实例。
2. 缓冲区指针只被保存，不被复制。调用方必须保证其生命周期覆盖整个对象生命周期。
3. `pm_gpio_pin_t` 是无状态句柄（只有一个 id），不需要初始化函数，直接传值。
4. 一个 `hw_id` 在同一时刻只能被一个对象占用；重复 `init` 返回 `PM_ERR_BUSY`，且**不破坏已初始化对象**。
5. `pm_uart_deinit` 幂等：对未初始化对象调用返回 `PM_OK`。

结构体字段是公开的（C 没有 private），但**上层不得直接读写任何字段**；布局属于实现细节。

---

## 5. 初始化顺序与依赖

固定顺序，前一步失败则不得进入下一步：

```
1. 启动文件 / SystemInit        时钟树（72 MHz 或 HSI 回退）
2. pm_platform_init()           HAL_Init + SysTick 1 ms + OSAL 计数归零
3. board 级引脚/时钟准备        由 port 的 open/configure 按需完成，无需单独调用
4. pm_gpio_configure()          每个要用的逻辑引脚
5. pm_uart_init()               每个要用的串口
6. 主循环：read / write / poll
```

依赖关系：

- `pm_osal_now_ms()` 依赖 `pm_platform_init()`；在它之前调用返回未定义时间（不崩溃，但无意义）。
- `pm_uart_write(timeout > 0)` 依赖 OSAL 时基；没有时基时只能使用 `PM_UART_TIMEOUT_NOW`。
- `pm_uart_init` 内部顺序固定为：参数校验 → 环形缓冲 → 事件队列 → port `open` → 置 READY → `enable_rx`。
  任意一步失败，已获取的资源必须回滚（`open` 失败时对象保持未初始化）。
- 中断使能发生在对象进入 READY **之后**，因此 ISR 看到的对象一定是完整的。

---

## 6. ISR 到主循环：通知模型与所有权

这是本套代码最关键的约定。

**ISR 内允许做的事，仅限三件**：

1. 从数据寄存器取出一个字节（或一段，DMA 场景）；
2. 把字节写入 RX 环（`pm_rb_write`）或从 TX 环推进（`pm_rb_consume`）；
3. 向事件队列投递一个事件（`pm_evtq_push`）。

**ISR 内禁止**：调用用户回调、调用 `pm_osal_delay_ms`、打印、做协议解析、分配内存、长时间循环、调用任何可能阻塞的 HAL 函数。

**所有权**：

| 数据 | 生产者 | 消费者 | 同步方式 |
| --- | --- | --- | --- |
| RX 环 `head` | RX ISR | 主循环 `pm_uart_read` | 单生产者/单消费者，无需临界区 |
| RX 环 `tail` | 主循环 | RX ISR | 同上 |
| TX 环 `head` | 主循环 `pm_uart_write` | TX 完成 ISR | 同上 |
| TX 环 `tail` | TX 完成 ISR | 主循环 | 同上 |
| 事件队列 | 任意 ISR | 主循环 `pm_uart_poll` | 单生产者/单消费者 |
| `tx_inflight` | 主循环 + TX ISR | 两侧 | **需要临界区**（`pm_osal_critical_enter`） |
| `stats` 计数 | ISR 与主循环各自独立字段 | 主循环读取 | 32 位对齐读写在 M3 上原子 |

**事件语义**（`pm_event.h`）：

- `PM_EVT_UART_RX_DATA`：电平型，RX 环非空。连续收到多个字节只保留一个事件。
- `PM_EVT_UART_TX_DONE`：电平型，TX 环变空。
- `PM_EVT_UART_ERROR`：边沿型，硬件错误发生一次投递一次（同一 src 会去重，计数看 `stats.hw_errors`）。
- `PM_EVT_SPI_DONE`：电平型，"当前没有在途传输"，`arg` 为最近一次完成的字节数。
- `PM_EVT_SPI_ERROR`：边沿型，`arg` 为后端错误掩码，计数看 `pm_spi_stats_t.errors`。
- `PM_EVT_I2C_DONE`：电平型，"当前没有在途传输"，`arg` 为本次事务移动的字节数（写读合计）。
- `PM_EVT_I2C_ERROR`：边沿型，`arg` 为后端错误掩码（HAL `ErrorCode`，或 port 私有位），计数看 `pm_i2c_stats_t.errors`。
- 队列满时**丢弃最新事件并累加 `dropped`**。静默丢事件是不允许的。
- 队列容量为 `PM_EVTQ_DEPTH - 1`（留一个槽区分空/满）。

---

## 7. 回调穿层规定

- 用户回调只有一种：`pm_uart_evt_cb_t`，由 `pm_uart_poll` 在主循环上下文调用。
- **回调绝不在 ISR 中执行**。port 层不得保存或调用用户回调。
- 回调参数中的 `pm_uart_t *` 是借用，回调不得 `deinit` 或销毁对象。
- 回调内可以调 `pm_uart_read/write`（同一对象重入只发生在主循环，安全）。
- 不允许"注册回调后就不再 poll"的用法：**事件必须被 poll 消费**，否则队列会饱和并开始计数丢弃。

---

## 8. 超时与重试

- 所有可能阻塞的公共 API 都有 `timeout_ms` 参数：
  - `0`（`PM_UART_TIMEOUT_NOW`）：不等待，能推多少推多少，未完成返回 `PM_ERR_AGAIN`；
  - `> 0`：有界等待，超时返回 `PM_ERR_TIMEOUT`，并通过 `*written` 报告已接受字节数。
- 超时比较一律用无符号差值 `now - start`，禁止 `now > start`（49.7 天回绕）。
- 库内部**不实现自动重试**。重试策略由 APP 决定：重试次数、退避间隔、失败上报。
- 库内部不允许 `while (!flag)` 形式的无界循环。所有等待都必须带时间上界。
- `PM_TIMEOUT_FOREVER` 之类的无限等待在本版本**未提供**；启动阶段如需等待，请给出明确的大超时值并记录原因。

---

## 9. 并发与临界区

- 模型假设**单核、中断优先级固定、无 RTOS**。
- 临界区只用于保护"两侧共同读写的状态"，目前只有 UART 的 `tx_inflight`、SPI 与 I2C 的 `active` 与 flush 操作。
- SPI 的缓冲区所有权按传输分段：传输在途时归 port 借用，ISR 完成后归还；不存在跨传输的共享缓冲。
- I2C 的 `active` 同时是总线占用标志：`write_read` 的写、读两段之间不释放，直到 STOP 后的完成回调。
- 环形缓冲的 SPSC 结构**不需要**临界区；不要为了"保险"把整个写路径包进临界区，那会拉长中断关闭时间。
- 临界区必须成对、可嵌套，`enter` 返回的状态必须原样传给 `exit`。
- 临界区内禁止：调用 HAL 阻塞函数、延时、打印、调用用户代码。
- 中断优先级：UART 使用优先级 1，SPI 使用优先级 2，I2C 事件中断优先级 2、错误中断优先级 1（数值越大优先级越低），SysTick 保持 HAL 默认。若上层引入更高优先级中断，需重新评估临界区覆盖范围。

---

## 10. 可测试性

硬性要求：**核心逻辑不依赖硬件即可编译并运行**。

- 判断标准：`src/core/*.c` 的 include 列表里只能出现 `include/pm/*` 和 C 标准库。
- 主机侧提供两样东西即可跑测试：
  - `tests/host/pm_port_mock.c`：实现 `pm_port_get()` 的内存版；
  - `tests/host/pm_osal_host.c`：时间由测试推进的 OSAL。
- 当前覆盖：环形缓冲（含回绕、丢弃、consume）、事件队列（去重、溢出）、GPIO 参数校验、
  UART 生命周期/TX 流控/RX 投递/超时、SPI 传输/中止/DMA 标志、I2C 写/读/写读/中止/错误、
  ADC 单次转换/超时/错误、PWM 参数校验/输出操作/时基数学、定时器仲裁、软件音调、循迹、
  编码器（含可选按键）、继电器、电机（方向/滑行/刹车/频率冲突）、舵机（角度↔脉宽）。
  共 689 项断言，全部通过。
- 新增外设时，**先写 core 逻辑和 mock 测试，再接真实后端**。做不到就说明该逻辑还不该进 core。

---

## 11. SPI 模型（v0.2 新增）

本节只增加能力，不改变第 1～10 节的任何既有行为。

### 11.1 能力边界

- 只做**主机、全双工、8 位帧**；每次调用完成一个片选窗口内的传输。
- 不做从机、半双工、16 位帧、多主机、TI 模式、CRC。
- 片选：`PM_SPI_NSS_SOFT` 由 APP 用 GPIO 控制。`PM_SPI_NSS_HARD` 在契约里保留，
  但本板两个 SPI 的硬件 NSS 引脚均已被板载器件占用（PA4 NTC、PB12 KEY1），
  因此当前 STM32F1 port 对 `PM_SPI_NSS_HARD` 直接返回 `PM_ERR_PARAM`，不会静默改成软件 NSS。
  换板后可放开，前提是 NSS 引脚可用。
- 单次传输上限 `PM_SPI_MAX_FRAME`（65535）字节，超出返回 `PM_ERR_PARAM`。
- 一次只允许一个在途传输，没有发送队列。

### 11.2 生命周期

```
init -> (transfer / poll)* -> deinit
```

- 对象必须零初始化；`magic` 防止重复 init，重复 init 返回 `PM_ERR_BUSY` 且不破坏已打开实例。
- `init` 顺序：参数校验 → port `open`（时钟、引脚、外设、可选 DMA）→ 置 READY。
  `open` 失败时 port 必须回滚（关闭已开的时钟/中断/DMA），对象保持未初始化。
- `deinit` 先中止在途传输再关闭，幂等。
- 实际 SCK 由 port 按 `PCLK / 2^n` 向下取整，通过 `pm_spi_clock_hz()` 读取；`cfg.clock_hz` 只是请求值。

### 11.3 传输语义

`tx`、`rx` 都必填，长度相同，同一组时钟沿上同时收发。

| 返回码 | 含义 | `*done` |
| --- | --- | --- |
| `PM_OK` | len 字节全部完成（`timeout == 0` 且立即完成也算） | len |
| `PM_ERR_AGAIN` | 已启动、尚未完成（仅 `timeout == 0`） | 0 |
| `PM_ERR_TIMEOUT` | 到期，port 已中止 | 已时钟出的字节数 |
| `PM_ERR_IO` | 硬件错误 | 出错前已完成的字节数 |
| `PM_ERR_BUSY` | 已有传输在途 | 0 |
| `PM_ERR_PARAM` | 长度超限或参数非法 | 0 |

- `timeout == 0` 且返回 `PM_ERR_AGAIN` 时，两个缓冲区归 port 借用，直到 `PM_EVT_SPI_DONE` / `PM_EVT_SPI_ERROR` 被 poll 到；期间调用方不得复写。
- 超时中止后缓冲区立即归还，可以安全复用。
- 错误码复用 `pm_err_t`，没有新增返回码。

### 11.4 ISR 到主循环

| 数据 | 生产者 | 消费者 | 同步方式 |
| --- | --- | --- | --- |
| `active`（在途标志） | 主循环置位；完成/错误 ISR 清零 | 两侧 | 临界区 |
| `last_done` / `last_error` | 完成/错误 ISR | 主循环等待与 `*done` | ISR 先写计数、屏障后清 `active` |
| 事件队列 | 完成/错误 ISR | `pm_spi_poll` | 单生产者/单消费者 |
| `stats` | ISR 与主循环各自字段 | 主循环读取 | 32 位对齐写在 M3 上原子 |

- ISR 只做三件事：清在途标志、更新计数、投递事件；不调用用户代码、不解析协议、不阻塞。
- 用户回调只经 `pm_spi_poll` 在主循环上下文执行。
- 事件语义见第 6 节；`SPI_DONE` 去重，若两次传输在 poll 之前先后完成，`arg` 是较新一次的长度，次数以 `stats.transfers` 为准。

### 11.5 超时与中止

- 同步等待不轮询寄存器：每 1 ms 检查一次在途标志，用无符号时间差判断是否到期。
- 到期后调用 port 的 `abort`，port 先读硬件/DMA 剩余计数再中止，把已完成的字节数回报给模型。
- 中止与 ISR 完成存在竞态：若 ISR 先完成，按 `PM_OK` 处理，不重复计数。
- HAL 1.1.10 的 IT/DMA 完成回调内部会短暂轮询 BSY（`SPI_DEFAULT_TIMEOUT`），这是 HAL 自身行为，模型层无法消除；代价是中断上下文里最多几十微秒的等待。

### 11.6 DMA

- `cfg.use_dma` 是请求，由 port 决定是否接受；当前 F1 后端两个实例都支持。
- F1 映射：SPI1 → DMA1 Channel2/3，SPI2 → DMA1 Channel4/5，NVIC 优先级 2。
- DMA 与中断路径共用同一组完成回调、事件与统计，上层无差别。
- DMA 初始化失败时 `open` 返回 `PM_ERR_IO`，**不静默降级**为中断模式。

---

## 12. I2C 模型（v0.3 新增）

本节只增加能力，不改变第 1～11 节的任何既有行为。

### 12.1 能力边界

- 只做**主机、7 位地址**。每次调用完成一个完整的总线事务（START…STOP）。
- 三种事务：`write`（addr+W，数据）、`read`（addr+R）、`write_read`
  （addr+W 写若干字节，**repeated START**，addr+R 读若干字节，最后 STOP）。
  `write_read` 是寄存器读的标准形态，写读之间不发 STOP。
- 不做从机、10 位地址、SMBus、多主机仲裁、DMA。总线恢复（9 个 SCL 脉冲）也**未实现**。
- 单段长度上限 `PM_I2C_MAX_FRAME`（65535），超出返回 `PM_ERR_PARAM`；`write_read` 两段都受此限制。
- 一次只允许一个在途事务，没有队列。

### 12.2 生命周期

```
init -> (write / read / write_read / poll)* -> deinit
```

- 对象必须零初始化；重复 `init` 返回 `PM_ERR_BUSY`，不破坏已打开实例。
- `cfg.clock_hz` 合法区间 10 kHz～400 kHz，越界返回 `PM_ERR_PARAM`。
- `init` 顺序：参数校验 → port `open`（GPIO、时钟、`HAL_I2C_Init`、NVIC）→ 置 READY。
  `open` 失败时 port 由 HAL 回滚，对象保持未初始化。
- 实际 SCL 由 `pm_i2c_clock_hz()` 返回，来源是外设 `CCR` 与 `PCLK1` 反算，
  不是把请求值原样回显。
- `deinit` 先中止在途事务再关闭，幂等。

### 12.3 事务语义

| 函数 | 总线动作 | `expected` |
| --- | --- | --- |
| `pm_i2c_write` | START, addr+W, data[len], STOP | len |
| `pm_i2c_read` | START, addr+R, data[len], STOP | len |
| `pm_i2c_write_read` | START, addr+W, wr[wr_len], repeated START, addr+R, rd[rd_len], STOP | wr_len + rd_len |

| 返回码 | 含义 | `*done` |
| --- | --- | --- |
| `PM_OK` | 事务完整结束（`timeout == 0` 且立即完成也算） | expected |
| `PM_ERR_AGAIN` | 已启动、尚未结束（仅 `timeout == 0`） | 0 |
| `PM_ERR_TIMEOUT` | 到期，port 已复位外设 | 已移动的字节数 |
| `PM_ERR_IO` | NACK 或其它硬件错误 | 出错前已移动的字节数 |
| `PM_ERR_BUSY` | 已有事务在途 | 0 |
| `PM_ERR_PARAM` | 地址 > 0x7F、长度越界、`write_read` 有空段 | 0 |

- `timeout == 0` 且返回 `PM_ERR_AGAIN` 时，缓冲区归 port 借用，直到
  `PM_EVT_I2C_DONE` / `PM_EVT_I2C_ERROR` 被 poll 到；期间不得复写。
- 超时或错误结束后缓冲区立即归还，可以复用。
- 错误码复用 `pm_err_t`，没有新增返回码；`PM_ERR_IO` 时具体原因看 `pm_i2c_stats_t.last_error`。

#### 器件类型：寄存器型与命令触发型

`write_read` 的 repeated START 只对**寄存器型**器件成立，对**命令触发型**器件是错的。两类器件的判别方法：

| | 寄存器型 | 命令触发型 |
| --- | --- | --- |
| 典型器件 | EEPROM、传感器寄存器、OLED 命令/数据（0x7A） | AHT20（0x70）、单次转换的 ADC、需要"启动→转换→读取"的传感器 |
| 读时序 | `S + addrW + reg + Sr + addrR + data + P` | 触发和读取是两个独立事务，中间隔着转换时间 `t_conv` |
| 怎么写 | `pm_i2c_write_read`，写段是寄存器地址 | `pm_i2c_write`（STOP 结束）→ 等待 → `pm_i2c_read` |

判别方法，按可靠性排序：

1. 看数据手册的读时序图：出现 `Sr`（repeated START）就是寄存器型；把触发命令和读取画成两段、中间标 `t_conv` 的就是命令触发型。
2. 实测：对同一器件连续做两次 `write_read`，若两次结果相同、且状态字节的 busy 位（AHT20 是 bit7）仍为 1，说明读发生在转换完成之前，属于命令触发型误用。

#### 命令触发型必须用 STOP 分隔

- 命令触发型的写段是"启动一次内部转换"，不是"设置读地址"。写段必须**以 STOP 结束**（`pm_i2c_write`），再另起一次读事务（`pm_i2c_read`）。
- 用 `write_read` 把触发命令和读粘在一起时，repeated START 不会让器件"重新开始"，读回的是**上一次**转换结果，甚至带上假忙位。
- 本版**不新增**"写后自动等待"的组合 API：`pm_i2c_write` / `pm_i2c_read` / `pm_i2c_write_read` 三个函数与 `pm_i2c_cfg_t` 保持不变。

#### 转换等待由调用方实现

- 模型不内置延时、不做状态位轮询。一次只允许一个在途事务，等待转换期间不得再发起 I2C 事务。
- 调用方用 OSAL 原语实现等待：`pm_osal_delay_ms(t_conv)` 定长等待，或 `pm_osal_now_ms()` 起循环读状态字节、轮询 busy 位，并设上限超时。
- 等待策略属于器件知识，不属于总线语义，因此留在应用层。

#### 契约反例（板端实测，AHT20 0x70，I2C1 100 kHz）

- **错误形态**：`pm_i2c_write_read(&i2c, 0x70, {0xAC,0x33,0x00}, 3, buf, 6, ...)` 连续做两次，
  两次都读回 `98 68 A7 F5 A3 11`——与上一次转换结果相同，状态字节 `0x98` 的 bit7 = 1（busy）。
- **正确形态**：`pm_i2c_write(0x70, {0xAC,0x33,0x00})`（STOP 结束）→ `pm_osal_delay_ms(80)` →
  `pm_i2c_read(0x70, buf, 6)`，读回 `18 68 A7 F5 A3 11`，换算 40.8 %RH / 20.4 °C，重复测量一致。
- 两组字节只差状态字节的最高位（`0x98` → `0x18`），其余 5 字节相同，说明 repeated START 形态确实读到的是旧值而不是通信错误。

### 12.4 ISR 到主循环

| 数据 | 生产者 | 消费者 | 同步方式 |
| --- | --- | --- | --- |
| `active` | 主循环置位；完成/错误 ISR 清零 | 两侧 | 临界区 |
| `last_done` / `last_error` | 完成/错误 ISR | 主循环等待与 `*done` | ISR 先写计数、屏障后清 `active` |
| 事件队列 | 完成/错误 ISR | `pm_i2c_poll` | 单生产者/单消费者 |
| `stats` | ISR 与主循环各自字段 | 主循环读取 | 32 位对齐写在 M3 上原子 |

- 中断路径：`I2C1_EV_IRQHandler → HAL_I2C_EV_IRQHandler`。写阶段完成时 HAL 回调
  `HAL_I2C_MasterTxCpltCallback` 在 **ISR 内**发起读阶段（`HAL_I2C_Master_Seq_Receive_IT`，
  `I2C_LAST_FRAME`），此时不向模型报完成；读阶段结束的 `HAL_I2C_MasterRxCpltCallback`
  才调用 `pm_i2c_hw_done()`。
- 错误路径：`I2C1_ER_IRQHandler → HAL_I2C_ER_IRQHandler → HAL_I2C_ErrorCallback →
  pm_i2c_hw_error()`；`arg` 是 HAL `ErrorCode`。读阶段在 ISR 里启动失败时用 port 私有位
  `0x8000`，不与 HAL 错误码混淆。
- ISR 不调用用户代码、不解析协议、不阻塞；用户回调只在 `pm_i2c_poll` 里执行。
- `I2C_DONE` 去重，多次事务在 poll 之前完成时 `arg` 是较新一次，次数以 `stats.transfers` 为准。

### 12.5 为什么不是忙等，代价是什么

- 没有使用 `HAL_I2C_Master_Transmit()` / `Master_Receive()` / `Mem_Read()` 这类阻塞函数。
  字节推进全部由 HAL 的 IT 状态机在中断里完成；模型只在 `timeout_ms > 0` 时每 1 ms
  查一次 `active`，等待本身不碰寄存器。
- 代价一：HAL 1.1.10 的 F1 I2C IT 状态机在 NACK（AF）后可能停留在 `HAL_I2C_STATE_BUSY`。
  模型的下一次事务会因此拿不到总线。`abort` 采用 `HAL_I2C_DeInit` + `HAL_I2C_Init`
  复位外设来兜底，而不是依赖 HAL 的异步 `Master_Abort_IT`。
- 代价二：超时只复位外设，**不做总线清除**。从机把 SDA 长时间拉低时，复位外设也无法
  让总线恢复；需要上层断电重启从机或加 9 个 SCL 脉冲的恢复例程，本版不提供。
- 代价三：`write_read` 的读阶段是在 TX 完成中断里启动的，若 `HAL_I2C_Master_Seq_Receive_IT`
  返回非 OK（例如状态机仍在 BUSY），本次事务以 `PM_ERR_IO` 结束，`last_error = 0x8000`。

### 12.6 本板引脚

- `BOARD_I2C_1`：PB6 = SCL，PB7 = SDA，与 OLED（0x7A）、AHT20（0x70）、外部 I2C 排针共用。
- I2C2 不暴露：PB10/PB11 在本板上是 USART3 / BLE 接口。
- 引脚配置为 AF 开漏 + 内部弱上拉；总线仍需要外部上拉（模块板自带），内部上拉只作兜底。

### 12.7 可选错误分类与计数（v0.6 新增）

**问题**：12.1～12.6 里所有硬件失败都返回 `PM_ERR_IO`，调用方只能从
`stats.last_error` 的原始掩码里翻。设备不在（AF）与接线/总线故障（BERR/ARLO）
的处理动作完全不同：前者查地址或断电，后者查线与上拉。混在一个错误码里，
上层只能都当"IO 挂了"。

**取舍**：分类是**可选**能力，默认关闭。理由不是省事，而是既有调用形态
（以及 18 条告警所覆盖的路径）都按 `PM_ERR_IO` 写好了分支；默认改变返回码
等于在移植轮之前改动调用方契约。开启后只影响本次调用之后的返回码与计数，
不改变 `stats` 字段语义、不改变事件 `arg`。

| 判据 | 开启状态 | 返回码 | 计数 |
| --- | --- | --- | --- |
| 任意硬件错误掩码 | 关（默认） | `PM_ERR_IO` | 不动 |
| `AF`（从机不响应） | 开 | `PM_ERR_NACK` | `af++` |
| `BERR`（START/STOP 错位） | 开 | `PM_ERR_BUS` | `berr++` |
| `ARLO`（仲裁丢失） | 开 | `PM_ERR_BUS` | `arlo++` |
| `OVR`（字节被覆盖） | 开 | `PM_ERR_BUS` | `ovr++` |
| 仅 `LOCAL`/`DMA`/无法识别的位 | 开 | `PM_ERR_BUS` | `other++` |
| 端口自身的 `TIMEOUT` 位 | 开 | `PM_ERR_BUS` | `other++` |
| 截止时间到期（模型侧 abort） | 开 | `PM_ERR_TIMEOUT` | `timeout++` |
| 传输完成但字节数不足 | 开 / 关 | `PM_ERR_IO` | 不动 |

- 归类优先级：`AF` > `BERR` > `ARLO` > `OVR` > `TIMEOUT` > `OTHER`。一个掩码里
  有多个位时按最可诊断的那个算，不重复计数。
- `timeout++` 只记模型侧截止时间到期；端口在 `ErrorCode` 里自带 `TIMEOUT` 位时
  仍归 `other`，因为那是外设报错，不是本次等待超时。
- `pm_i2c_set_error_counting(i, true)` **先把计数清零**再开启，避免把开启前的
  历史混进统计；`false` 只关开关，不清计数（读数仍可查看上次结果）。
- 计数只在 ISR 钩子（`pm_i2c_hw_error`）与超时路径里累加，主循环只读；
  32 位对齐写在 M3 上原子，读取无需临界区。
- `pm_i2c_set_error_counting` / `pm_i2c_get_error_counts`：NULL → `PM_ERR_NULL`，
  未初始化 → `PM_ERR_STATE`。关闭状态下读取合法，返回当前（可能全零）快照。
- 端口侧不新增回调：`pm_i2c_hw_error(i, mask, done)` 的 `mask` 就是
  `PM_I2C_EERR_*`（见 12.4），F1 端口原样传 `hi2c->ErrorCode`，手写端口必须翻译。
- 本节的分类结果**不替代** `stats.last_error` 与事件 `arg`：后两者仍是原始掩码，
  便于上板对日志。

---

## 13. ADC 模型（v0.4 新增）

本节只增加能力，不改变第 1～12 节的任何既有行为。

### 13.1 能力边界

- 只做**单通道、单次转换、软件触发**。没有扫描序列、连续转换、注入通道、模拟看门狗与 DMA。
- 一个对象绑定一个逻辑通道；通道号由板级表映射到 `(GPIOx, GPIO_PIN_n, ADC_Channel)`。
- 原始值是右对齐 12 位结果（0～4095）。`vref_mv` 只用于 `pm_adc_to_mv()`，
  port 不测量参考电压；默认按 3300 mV 换算。
- 采样时间用 `pm_adc_sample_t` 表达（1.5～239.5 个 ADC 时钟周期），由 port 映射到 HAL 宏。
  高源阻抗（如 NTC 分压）应选较长的采样时间。

### 13.2 生命周期

```
init -> (read / read_mv / poll)* -> deinit
```

- 对象必须零初始化；重复 `init` 返回 `PM_ERR_BUSY`。
- `cfg.sample_time` 超出枚举范围、`vref_mv` 不在 1000～5000，返回 `PM_ERR_PARAM`；
  `hw_id` 或 `channel` 越界返回 `PM_ERR_RANGE`（都由 port 判定）。
- `init` 顺序：参数校验 → port `open`（GPIO 模拟输入、ADCCLK 分频、`HAL_ADC_Init`、
  通道配置、校准、NVIC）→ 置 READY。
- F1 端口的 `open` 会调用 `HAL_ADCEx_Calibration_Start()`，该调用内部等待校准完成，
  **在 `init` 期间阻塞若干微秒**；这是 F1 的要求，不放到每次转换里。
- `deinit` 先中止在途转换再关闭，幂等。

### 13.3 转换语义

| 返回码 | 含义 |
| --- | --- |
| `PM_OK` | 转换完成，`*raw` 有效（`timeout == 0` 且立即完成也算） |
| `PM_ERR_AGAIN` | 已启动、尚未完成（仅 `timeout == 0`）；结果稍后由 `ADC_DONE` 事件给出 |
| `PM_ERR_TIMEOUT` | 到期，port 已停止转换，结果无效 |
| `PM_ERR_IO` | HAL 错误回调，具体掩码见 `stats.last_error` 与 `ADC_ERROR.arg` |
| `PM_ERR_BUSY` | 已有转换在途 |
| `PM_ERR_STATE` | 对象未初始化 |

- `timeout == 0` 返回 `PM_ERR_AGAIN` 后，结果只能通过 `pm_adc_poll()` 取
  `PM_EVT_ADC_DONE`（`arg` = 原始值），或再调用一次 `pm_adc_read()` 读取 `last_value`。
- `*raw` / `*mv` 只在 `PM_OK` 时写入；其它返回码不修改调用方缓冲区。
- `pm_adc_to_mv()` 是纯函数：`raw * vref_mv / 4095`，四舍五入；对象无效时按 3300 mV 计算。

### 13.4 ISR 到主循环

| 数据 | 生产者 | 消费者 | 同步方式 |
| --- | --- | --- | --- |
| `active` | 主循环置位；EOC/错误 ISR 清零 | 两侧 | 临界区 |
| `last_value` / `last_error` | EOC/错误 ISR | 主循环等待与返回值 | ISR 先写值、屏障后清 `active` |
| 事件队列 | EOC/错误 ISR | `pm_adc_poll` | 单生产者/单消费者 |

- 中断路径：`ADC1_2_IRQHandler → HAL_ADC_IRQHandler → HAL_ADC_ConvCpltCallback →
  pm_adc_hw_done()`；错误走 `HAL_ADC_ErrorCallback → pm_adc_hw_error()`。
- ISR 不调用用户代码、不阻塞；用户回调只在 `pm_adc_poll` 里执行。
- `ADC_DONE` 去重；两次转换都在 poll 之前完成时 `arg` 是较新一次，次数以
  `stats.conversions` 为准。

### 13.5 本板引脚

- `BOARD_ADC_NTC`：PA4 = ADC1_IN4，NTC 与 10 kΩ 分压（β 3950）。
- `BOARD_ADC_POT`：PA5 = ADC1_IN5，电位器；该引脚与 SPI1_SCK 复用，
  用 SPI1 前必须先释放 ADC，反之亦然。port 不检查跨外设的引脚冲突。
- ADC 时钟：PCLK2 = 72 MHz 时固定 /6 = 12 MHz（≤ 14 MHz 上限）。

### 13.6 真实器件的必要条件

**参考电压与量程**

- `vref_mv` 是**换算参数**，不是测量值：量程固定为 0～`vref_mv`，raw 为 0～4095，
  `pm_adc_to_mv()` 只做 `raw * vref_mv / 4095`（四舍五入）。port 不测量 VDDA。
- 上板前用万用表量 3.3 V 轨的实际电压（典型 3.25～3.35 V），把实测值填进 `cfg.vref_mv`；
  不要因为"板子标称 3.3 V"就直接写 3300。允许范围 1000～5000（§13.2 会校验）。
- 供电会变（USB / 电池 / 外设负载）时，`vref_mv` 必须跟着更新；本版本没有内部参考通道，
  不能自动补偿。做法是由应用层重新 `init`，或按下面的两点法换算。
- **两点标定（应用层实现，不改 core）**：输入接 GND 读 `raw0`；输入接一个用万用表实测的
  电压 `Vk`（如 1.000 V）读 `rawk`；之后按
  `V = (raw - raw0) * Vk / (rawk - raw0)` 换算。标定条件（日期、仪表、室温）写在调用处注释里。

**校准与偏置**

- 端口在每次 `init` 里调用 `HAL_ADCEx_Calibration_Start()`，这是 F1 要求的硬件自校准，
  失败返回 `PM_ERR_IO`。它消除的是 ADC 内部失调，不是板级误差。
- 软件偏置/增益**只允许出现在 app 或板级换算函数里**：`src/core/` 与 `include/pm/`
  不得出现任何针对某块板的校准常量，也不得按板号分支。
- 单板标定值若需固化，放在 `platform/stm32f103/` 的板级表或 app 配置里，换板必须重测；
  未标定的板子按 `vref_mv` 原始换算使用，误差写进测试记录，不要"猜一个偏置"。
- 不要用减常数掩盖接线错误（分压比算错、输入悬空、引脚映射错）。先过 13.7 的两个锚点，再做偏置。

**采样时间与源阻抗**

- 采样时间必须满足 `t_sample ≥ (R_source + R_adc) * C_adc * ln(2^12)`。F1 取
  `R_adc ≈ 1 kΩ`、`C_adc ≈ 8 pF`，系数约 8.3；ADCCLK = 12 MHz（1 个 ADC 周期 ≈ 83.3 ns）。
- 理论源阻抗上限（单次采样，未留余量）：

  | `pm_adc_sample_t` | 采样时间 | 源阻抗上限 |
  | --- | --- | --- |
  | `PM_ADC_SAMPLE_1CYCLE_5` | 0.125 µs | ≈ 0.9 kΩ |
  | `PM_ADC_SAMPLE_7CYCLES_5` | 0.625 µs | ≈ 8.4 kΩ |
  | `PM_ADC_SAMPLE_13CYCLES_5` | 1.125 µs | ≈ 15.9 kΩ |
  | `PM_ADC_SAMPLE_28CYCLES_5` | 2.375 µs | ≈ 34.7 kΩ |
  | `PM_ADC_SAMPLE_41CYCLES_5` | 3.458 µs | ≈ 50.9 kΩ |
  | `PM_ADC_SAMPLE_55CYCLES_5` | 4.625 µs | ≈ 68.5 kΩ |
  | `PM_ADC_SAMPLE_71CYCLES_5` | 5.958 µs | ≈ 88.5 kΩ |
  | `PM_ADC_SAMPLE_239CYCLES_5` | 19.96 µs | ≈ 299 kΩ |

- 本板源阻抗：电位器（3.3 V—GND 分压，中点接 PA5）最大 2.5 kΩ；
  NTC 与 10 kΩ 分压（PA4）随温度在 0.9～7.5 kΩ 之间。NTC 接在分压**低端（GND 侧）**、
  10 kΩ 在上端接 3.3 V，因此升温 → NTC 阻值下降 → PA4 分压下降 → 原始值**下降**
  （实测见 13.7 第 3 条）。两路都要求**从 `PM_ADC_SAMPLE_55CYCLES_5` 起步**，
  不要用 1.5 / 7.5 周期档。
- 单次转换时间 ≈（采样周期 + 12.5）/ 12 MHz；55.5 档约 5.7 µs，1 kHz 轮询无压力。
  采样时间加长会等比例降低最大转换率，这是换取源阻抗余量的代价。

**平均与"稳定错值"**

- 平均只对**随机噪声**有效：N 次平均把标准差降到约 `1/√N`，但**不改变均值偏差**。
- **稳定错值**是每次读数几乎相同、却不随被测量变化的值。板端实测出现过：探头前有近反射体时，
  读数每次都一样，平均后更"稳"，但离真值更远。同类来源还有采样时间不足、源阻抗过高、
  分压比/引脚映射错、`vref_mv` 与 VDDA 不符、相邻通道串扰。
- 判据：改变输入（转动电位器 / 加热 NTC）时读数应有可重复的变化；**输入变了而读数不动，
  就是稳定错值，禁止用平均"修"**。此时平均只会把错值固化成更可信的错值。
- 允许平均的前提：13.7 的两个锚点通过；且单个平均窗口内输入变化 ≤ 1 LSB。
  窗口内输入在动（电位器正在转、NTC 正在升降温）时，先降采样率或改用滑动平均并标注滞后。

**真实器件上的失败返回码**（与 §13.3 一致，不新增错误码）

| 现象 | 返回码 | 定位入口 |
| --- | --- | --- |
| 未 `init` / 已 `deinit` 后读写 | `PM_ERR_STATE` | 调用顺序 |
| 上一转换未完成又调用 `pm_adc_read` | `PM_ERR_BUSY` | `stats.conversions` 是否推进 |
| `timeout_ms > 0` 到期 | `PM_ERR_TIMEOUT`（port 已停止转换，`*raw` 不写） | `stats.aborted`；查 ADC 时钟与 NVIC |
| HAL 转换错误（过采样、DMA 误配等） | `PM_ERR_IO`（`*raw` 不写） | `stats.last_error` 与 `ADC_ERROR.arg` 的 HAL 掩码 |
| `sample_time` / `vref_mv` 越界 | `PM_ERR_PARAM`（`init` 阶段，未碰寄存器） | 配置值 |
| `hw_id` / `channel` 越界 | `PM_ERR_RANGE`（`init` 阶段） | 板级表 |
| `HAL_ADC_Init` / 通道配置 / 校准失败 | `PM_ERR_IO`（`init` 返回） | 供电、ADCCLK、引脚是否被别的外设占用 |

### 13.7 怎么在真器件上验证

不需要改代码，全部用现有 API。**按顺序做，前一步不过不进入下一步。**

1. **输入锚点（必做）**
   - PA5 接 GND，连读 20 次：raw 最大值 ≤ 10、均值 ≤ 5。【待实测阈值】
   - PA5 接一个万用表实测的 1.000 V（不要直接接 3.3 V 轨）：均值与
     `1000 * 4095 / vref_mv` 之差 ≤ 10 LSB。【待实测阈值】
   - 判据不过 → 先查地线、`vref_mv`、通道映射；**不要做后续测试，也不要加平均**。
2. **电位器单调性**
   - 螺丝刀从一端缓慢转到另一端，至少取 9 个位置，每个位置连读 20 次取均值。
   - 判据：9 点单调（相邻两点差 < 3 LSB 视为平段，允许）；首尾分别 ≤ 80 与 ≥ 4015
     （电位器两端有残余电阻，不要求贴 0 / 4095）。【待实测阈值】
   - 中间出现 > 100 LSB 的跳变 → 先加长采样时间；整段恒定 → 查通道/引脚映射。
3. **NTC 方向性与灵敏度（已实测）**
   - 手指捏住 NTC，每 0.5 s 读一次，持续 90 s，然后松手继续观察。
   - 本板拓扑（已确认）：NTC 在分压**低端（GND 侧）**，10 kΩ 在上端接 3.3 V。
     升温 → NTC 阻值下降 → 分压下降 → 原始值**下降**。
   - 判据：升温时 raw **单调下降**，90 s 内约 -500 LSB（幅度不足 -300 LSB 视为无效），
     中途不回升；松手后 raw 向反方向回升，且每 0.5 s 的读数可分辨（不是同一值反复出现）。
   - **实测依据**（STM32F103C8T6 + 板载 10 kΩ 分压，PA4，`PM_ADC_SAMPLE_55CYCLES_5`）：
     手指捏住后 raw 从 2293 单调下降到 1792（约 -500 LSB），90 s 内无回升。
   - 若升温时读数上升 → 与实测拓扑相反或通道映射错，先核对原理图；变化明显但来回跳 →
     查采样时间与电源噪声。
4. **区分噪声与稳定错值**
   - 电位器不动，连读 100 次：极差 ≤ 6 LSB 视为正常（12 MHz、55.5 周期）。【待实测阈值】
   - 极差大 → 加长采样时间；极差小但均值偏 → 按 13.6 做两点标定，**不要靠平均**。
5. **失败路径**
   - 同一对象在转换未完成时再调一次 `pm_adc_read`：期望 `PM_ERR_BUSY`，且第一次结果仍能从
     `ADC_DONE` 取到（`stats.conversions` 只加一次）。
   - 用 `PM_ADC_TIMEOUT_NOW` 启动后连续 `poll`：应拿到 `ADC_DONE`（`arg` = raw）；
     拿不到就查 NVIC 与 ADC 时钟，而不是反复重试。

---

## 14. PWM 模型（v0.4 新增）

### 14.1 能力边界

- 只做**定时器比较输出（PWM 模式 1）**：一个对象对应一个定时器通道。
- 不做输入捕获、单脉冲、死区、互补输出、刹车与中断回调。正交编码器是独立模型（§19）。
- 频率与占空比在运行中可改；占空比单位是千分比（0～1000），不使用浮点。
- **单位边界（决策 0018）**：对外串口协议与板端 API 用百分数（`0..100`）；千分制只属于模型内部。
  模型接板端时由**模型侧适配层**换算：`permille = pct × 10`、`pct = permille ÷ 10`
  （整数向下取整，例 305‰ → 30%）。板端组件不承担换算，模型也不为此新增对外单位。
- **同一硬件定时器的多个通道共享周期**：`open` 时若兄弟通道已打开且请求频率不同，
  返回 `PM_ERR_BUSY`；`set_frequency` 会改动兄弟通道的周期，因此同样返回
  `PM_ERR_BUSY`，不会静默改期。占空比各通道独立。

### 14.2 生命周期

```
init -> (set_frequency / set_duty / set_pulse_us / enable)* -> deinit
```

- 对象必须零初始化；重复 `init` 返回 `PM_ERR_BUSY`。
- `cfg.freq_hz` 合法区间 1 Hz～1 MHz，`duty_permille` 0～1000，越界返回 `PM_ERR_PARAM`；
  `hw_id` 越界返回 `PM_ERR_RANGE`。
- `init` 配置引脚（AF 推挽）、时基（PSC/ARR）与比较通道，并**直接启动输出**。
- 有效频率由 `pm_pwm_frequency_hz()` 返回，是定时器时钟与 `PSC/ARR` 反算的结果，
  可能与请求值有少量整数舍入误差。
- `deinit` 停止输出并释放通道；同一通道再次 `init` 前必须 `deinit`；幂等。

### 14.3 占空比与脉冲宽度

- `pm_pwm_set_duty(p, permille)`：`CCR = (ARR + 1) * permille / 1000`，上限截断。
  1000‰ 在 65536 个计数周期的极端情况下会损失一个计数（CCR 寄存器 16 位），
  其余情况为真正的常高。
- `pm_pwm_set_pulse_us(p, us)`：按当前频率换算成千分比，
  `permille = us * freq_hz / 1000`，超过一个周期时截断到 1000‰。
  舵机（50 Hz、500～2500 µs）直接用这个接口。
- `pm_pwm_enable(p, false/true)` 只开关比较输出，不改配置。
- 本模型的占空比接口一律收千分比；对外百分数由适配层换算（见 14.1），模型不新增百分数接口。

### 14.4 本板通道

| 逻辑通道 | 引脚 | 定时器通道 | 典型用途 |
| --- | --- | --- | --- |
| `BOARD_PWM_MOTOR_A` | PA0 | TIM2_CH1 | DRV8833 A 路 |
| `BOARD_PWM_MOTOR_B` | PA1 | TIM2_CH2 | DRV8833 B 路 |
| `BOARD_PWM_SERVO` | PB8 | TIM4_CH3 | 舵机，50 Hz |

- TIM2 上 PA0/PA1 同组，频率必须一致（电机两路）；TIM4 本板只开 PB8（SERVO）一路。
- PB9 没有 PWM 通道（`BOARD_PWM_BUZZER` 已删除）。本板蜂鸣器只有 **TIM3 更新中断
  软件音调**一个后端（§17），不占 TIM4，因此与舵机可同时运行。
- 定时器时钟来自 APB1 ×2（APB1 分频不为 1 时），PSC 先选够大以保证 ARR 落在 16 位内。
- 引脚配置为 AF 推挽、无上下拉、高速。

### 14.5 真实器件的必要条件

**时基自检（必要条件，不是建议）**

- **时基由端口决定**：本板舵机走 `PSC = 71 / ARR = 19999`（1 MHz tick、50 Hz），电机走
  `PSC = 0 / ARR = 3599`（20 kHz），均已在板上验证；模型不规定端口必须采用哪一对寄存器值。
- 板端实测反例（2026-11）：舵机 50 Hz 写成 `PSC = 0`、`ARR = 19999`。配置"看起来有
  PSC、有 ARR"，实际是把 72 MHz 当 1 MHz 用：`72 MHz / (0+1) / (19999+1) = 3.6 kHz`，
  舵机完全不动作。这是"寄存器里都有值、时基却错了"的典型陷阱。
- 因此 port 必须：
  1. **写寄存器之前**用 `pm_pwm_timing_matches()`（容差 10‰）复核"这对寄存器实际产生多少 Hz"，
     与请求值不匹配则返回 `PM_ERR_PARAM`，不启动输出；容差内通过即可，不要求等于请求值。
     比较用**交叉相乘**、不先把实际频率取整——取整会让合法组合在自己的容差边界上判假
     （449.9 Hz 取整成 449 对 450 Hz），板端自检也是这个口径；
  2. 有效频率由 `pm_pwm_frequency_hz()` 暴露（寄存器反算），调用方据此判断，不要相信请求值。
- `pm_pwm_timing_compute()`（common 层，契约头 `pm_pwm_math.h`）是**可选工具**：按目标频率
  选 PSC/ARR 时可用它，端口用固定常量同样合法，它不是唯一入口。自行选 PSC 时必须让整个
  周期落进 16 位 ARR。
- 判据示例：72 MHz、PSC 0、ARR 19999 对 50 Hz 的 `matches` 必须为 `false`（实为 3.6 kHz）；
  板端 50 Hz 的 `PSC 71 / ARR 19999` 与 20 kHz 的 `PSC 0 / ARR 3599` 必须为 `true`。
- 主机单测覆盖：`test_pwm_math()` 固定了上述反例，以及交叉相乘的容差边界（450 Hz / 500 Hz
  各一对：0‰ 判假、1‰ 判真，四组四舍五入后都是同一整数频率）。

**频率与分辨率的相互约束**

- 定时器时钟 `f_tim = 72 MHz`（APB1 = 36 MHz 且预分频 ≠ 1 时 ×2）。周期计数
  `N = f_tim / ((PSC + 1) * freq)`，必须满足 `1 ≤ N ≤ 65536`；port 先选 PSC 让 N 落进 16 位。
- 占空比分辨率是 `1/N`：千分比（0～1000）只有在 `N ≥ 1000` 时才有 1‰ 的台阶；
  `N < 1000` 时实际台阶是 `1/N`，`pm_pwm_set_duty()` 会把请求值截断到最近的 CCR。
- 下列数值是 `pm_pwm_timing_compute()` 的取整示例，只说明频率与分辨率的取舍，
  **不是端口必须采用的寄存器值**：

  | 频率 | PSC / ARR | 周期计数 N | 占空比台阶 | 有效频率 |
  | --- | --- | --- | --- | --- |
  | 1 Hz | 1098 / 65513 | 65 514 | 0.015‰ | 1.000002 Hz |
  | 50 Hz（舵机） | 71 / 19999（板端实测） | 20 000 | 0.05‰（1 µs） | 50 Hz |
  | 20 kHz | 0 / 3599 | 3 600 | 0.28‰ | 20 000 Hz |
  | 100 kHz | 0 / 719 | 720 | 1.4‰ | 100 000 Hz |
  | 1 MHz | 0 / 71 | 72 | 13.9‰ | 1 000 000 Hz |

  （除 50 Hz 行外，表内为 `pm_pwm_timing_compute()` 的取整结果；`pm_pwm_frequency_hz()` 返回有效值。
  50 Hz 行取板端实测的 71 / 19999；`compute()` 会给出 21 / 65453，两者都在 10‰ 容差内，
  任选其一即可，不以 21 / 65453 为标准。）
- 结论：频率越高，占空比与脉宽越粗。1 MHz 时最小可调占空比约 1.4%，不要用它做 1‰ 级的调节。
- `pm_pwm_set_pulse_us()` 用**有效频率**（`pm_pwm_frequency_hz()`）换算，结果截断到整数千分比：
  50 Hz 下 1‰ = 20 µs，所以脉宽步长是 20 µs（例：1501 µs → 75.05‰ → 75‰ = 1500 µs）。
  CCR 本身的台阶是 1 个计数（板端 50 Hz 的 1 MHz tick 下为 1 µs），但经千分比换算后以 20 µs 为步长，
  SG90 可直接使用。
  换频后同一对象的脉宽会随周期一起变，需要固定脉宽时应在换频后重新调用。

**占空比 0% 与 100% 的边界**

- 0‰：`CCR = 0`，PWM 模式 1 输出恒低（引脚仍被推挽驱动为低，不是高阻）。
- 1000‰：`CCR = N`。`N ≤ 65535` 时恒高；`N = 65536`（ARR = 0xFFFF 的最高位边界）时
  CCR 被截到 0xFFFF，每周期有 1 个计数（约 13.9 ns）的低电平，等效占空比 99.998%。
  这是 16 位 CCR 的已知边界，不是错误。
- 0‰ / 1000‰ 仍是"输出有效电平"，不是"关闭输出"：这两档下 `pm_pwm_enable(true)` 的引脚
  有确定电平，`pm_pwm_enable(false)` 才停止输出（见下）。
- `pm_pwm_set_duty()` 写 CCR 后，新占空比**最迟在下一个更新事件（≤ 1 个周期）生效**；
  不要假设多通道在同一时刻切换，也不要在换频的同一周期内测量占空比。

**同一定时器多通道**

- TIM2：PA0（MOTOR_A）、PA1（MOTOR_B）；TIM4：PB8（SERVO）。
  同组共享 PSC/ARR（周期），占空比各自独立。
- 兄弟通道已打开且请求频率不同时，`pm_pwm_init()` / `pm_pwm_set_frequency()` 返回
  `PM_ERR_BUSY`，周期保持不变；请求相同频率则允许，且 port 会按各通道保存的千分比
  重算全部 CCR（换频后所有兄弟通道的占空比台阶一起变粗或变细）。
- TIM4 本板只有舵机一路；PB9 没有 PWM 通道，蜂鸣器走 TIM3 软件音调（§17），两者可同时运行。
  要让 PB9 输出 PWM，需自行在板级表加一条 TIM4_CH4 通道，届时回到上面的同组约束，
  并与音调后端互斥（§17.3 两侧都会拒绝）。
- 模型没有中断与事件，改占空比不需要等回调；但 API 不是中断安全的（模型不加临界区），
  统一在主循环调用。

**开漏 / 推挽与上拉的选择**

- 本模型固定 **AF 推挽、无内部上下拉、高速**（§14.2、§14.4），没有配置项，也不新增配置项。
- 推挽输出会主动驱动高低电平，**不能与其他器件"线与"**；I²C 等开漏总线不能用 PWM 通道驱动。
- 需要开漏（共阳 LED、共享信号线）时，用外部三极管 / MOS 反相，或改用 GPIO 软件 PWM
  （不属于本契约）。
- 负载是高阻（MOS 栅极、长线）时，内部上拉不启用，必须在外部加下拉/上拉，否则停止输出后
  电平不确定。
- 本板蜂鸣器经 Q4（SS8050，NPN）驱动，由 TIM3 软件音调控制（§17），同样建议保持
  20% 占空比；不要长时间 100%。

**停止输出时应达到的状态**

- `pm_pwm_enable(false)`：停止该通道的比较输出，同一定时器的其它通道继续运行；
  引脚不再有 PWM 波形，具体电平不由本模型定义（F1 上预期为低，见 14.6 实测项）。
- `pm_pwm_deinit()`：等效于停止该通道；同一定时器最后一个通道 deinit 时，定时器整体
  DeInit（PSC/ARR 清零）。port **不会把引脚改回 GPIO**，引脚保持 AF 推挽。
- 因此"停在哪"不是契约保证：需要确定静态电平（电机停转、舵机松开、蜂鸣器不响）时，
  调用方在 `deinit` 后用 `pm_gpio_configure()` 把该引脚设为 GPIO 输出低（或电路要求的高），
  不要依赖"停止后自动为低"。
- DRV8833 这类 H 桥：PWM 停止 ≠ 刹车。滑行还是刹车由方向脚决定，方向脚不在本模型内，
  由 app 或板级代码管理。

### 14.6 怎么在真器件上验证

用示波器或逻辑分析仪，先空载看波形，再接负载。**每一步记录有效频率（`pm_pwm_frequency_hz()`）
与实际测量的周期/高电平，两者对不上就是 port 问题，不是负载问题。**

1. **空载波形**
   - 舵机通道：50 Hz、75‰（1.5 ms）→ 周期 20 ms ±1%，高电平 1.5 ms ±20 µs。
   - 蜂鸣器**不走 PWM**，本节没有它的波形项：PB9 由 TIM3 更新中断软件翻转，验证见 §17.4。
   - 判据：占空比误差 ≤ `1/N + 1` 个计数（50 Hz、1 MHz tick 时约 1 µs）。
2. **边界占空比**
   - 0‰：低电平直线，无任何窄脉冲。
   - 1000‰：高电平直线；只有 `N = 65536` 的频点允许每周期 1 个计数（约 14 ns）的低脉冲，
     需 ≥ 100 MHz 带宽才看得到。其余频点应为纯高。
3. **舵机（PB8，TIM4_CH3，50 Hz）**
   - 依次 `pm_pwm_set_pulse_us()` 500 / 1500 / 2500 µs，每档停 2 s。
   - 判据：分别到一端、中位、另一端；**停留 10 s 不回弹、不抖动**；中位与机械中位偏差 < 5°。
     回弹/抖动 → 先看周期是否稳定（供电跌落或频率漂移）。
   - 超过 2500 µs 的请求会被截断到 1000‰（20 ms 常高），舵机应停在极限位置而不是继续转。
4. **蜂鸣器（PB9，本板唯一后端是 TIM3 更新中断软件音调）**
   - 本节不验证：PWM 通道的周期/占空比判据对软件音调不适用，方法见 §17.4。
   - 本板没有 PB9 的 PWM 通道（`BOARD_PWM_BUZZER` 已删），所以蜂鸣器与 TIM4 舵机无共组关系。
5. **共定时器约束（TIM2 双电机通道）**
   - 先 init PA0（20 kHz），再 init PA1（20 kHz）→ 两路都成功，示波器确认同频不同占空比。
   - 对 PA1 `pm_pwm_set_frequency(10 kHz)` → 期望 `PM_ERR_BUSY`，示波器确认两路周期仍是 50 µs。
   - PA0 `deinit` 后对 PA1 再请求 10 kHz → 应成功；示波器确认 PA1 周期变 100 µs，PA0 已无波形。
6. **停止状态**
   - `pm_pwm_enable(false)` 后看 PB8：应无 PWM 波形；记录实际电平。【待实测，预期低】
   - `deinit` 后再记录一次；随后按 14.5 用 `pm_gpio_configure()` 拉低，确认舵机/电机进入预期状态。
7. **电机（PA0 / PA1，TIM2，DRV8833）**
   - 0‰ 与 1000‰ 时示波器确认占空比；接驱动后两路都 0‰ 电机滑行，单路 PWM + 另一路低
     为单向驱动。不要在没有方向控制时长时间 1000‰ 堵转。
   - PA0/PA1 与 ADC 的 PA4/PA5 无引脚冲突；PA5 只与 SPI1_SCK 冲突（见 §13.5）。

---

## 15. 新增一个外设的检查清单

第二阶段铺开 SPI/I2C/TIM/ADC/DMA/RTC/Flash/WDT 时，每个外设必须逐条满足：

1. 公共头放在 `include/pm/pm_<periph>.h`，只含标准类型；若模块含**端口必须直调**的原语
   （时基数学、资源仲裁），把该原语单独放进 common 契约头（如 `pm_pwm_math.h`、
   `pm_timer.h`），实现放 `src/common/`，并在 `.arch-roles` 判为 `common`（见 §2）；
2. 错误码复用 `pm_err_t`，不新增；
3. 对象静态分配，必须有 `magic` 与 `deinit`（无状态外设除外，如 GPIO）；
4. 阻塞 API 必须带 `timeout_ms`，非阻塞必须返回 `AGAIN`；
5. ISR 只做搬运 + 投递事件，事件必须可 poll、可计数丢弃；
6. 参数校验在触硬件之前；
7. port 侧只加 ops 函数与 ISR 钩子，不改 core 行为；
8. 主机 mock 测试通过；
9. ARM 侧编译通过，并在 README 的资源表里更新 Flash/RAM 占用；
10. 确认器件是**寄存器型**还是**命令触发型**：命令触发型必须用 STOP 分隔两次事务，
    转换等待由调用方用 OSAL 延时/轮询实现（见 §12.3）。

---

## 16. 版本与兼容

- 本仓库当前为 v0.5，接口可能在不通知的情况下调整。
- 结构体布局不承诺 ABI 稳定；跨版本升级请重新编译全部源码。
- 契约条款的修改必须同时更新本文件，不允许"代码改了文档没改"。

---

## 17. 软件音调模型（tone，v0.5 新增）

### 17.1 能力边界

- 只做**单路方波发声**：频率 + 占空比。不播音乐、不做包络/音量、不做多音混合。
- 后端是**定时器周期中断翻转 GPIO**，不是硬件比较通道。因此公共接口只有频率与占空比；
  tick 长度、tick 数、占空比量化步长都是**端口实现细节**，调用方不得依赖、也不得写进
  应用代码。
- 本板端口参数（实现细节，仅备查）：TIM3 更新中断，10 µs tick（100 kHz）；
  `period_ticks = 100000 / freq`，`duty_ticks = period_ticks * permille / 1000`。
  量化台阶 = 10 µs / 周期：100 Hz 约 0.1%，1 kHz 约 1%，5 kHz 约 5%。
- 合法范围：频率 100～5000 Hz，占空比 1～990‰。0 与 1000 拒收（`PM_ERR_PARAM`）：
  静音用 `pm_tone_off()`，不是"占空比 0"。
- 占用定时器时必须走 §20 的仲裁（role = TONE，channels = 0）。与任意 PWM 抢同一颗 TIM
  返回 `PM_ERR_CONFLICT`，不覆盖寄存器。

### 17.2 生命周期

```
init（开始发声）-> set（改频率/占空比，保持发声）-> off（停，引脚空闲低）
                -> set（重新开始）-> deinit
```

- 对象必须零初始化；重复 `init` 返回 `PM_ERR_BUSY`；未 `init` 调用其它 API 返回 `PM_ERR_STATE`。
- `deinit` 停止中断、释放 TIM 声明、引脚保持低；幂等。
- `init` 失败（含 `PM_ERR_CONFLICT`）后对象仍是未初始化态，可直接换配置重试。

### 17.3 与 PWM 蜂鸣器后端的选择

| 需求 | 用哪个 |
| --- | --- |
| 本板（PB9 经 Q4 驱动蜂鸣器） | 软件音调（本节，PB9 = TIM3 中断）——**本板唯一后端**，不占 TIM4，可与舵机同时用 |
| 另行让 PB9 输出 PWM（需改接线并在板级表加 TIM4_CH4 通道） | PWM 后端（§14）；与舵机同组，频率必须一致，且与音调互斥 |

- 本板**只有软件音调**这一条实际路径：`BOARD_PWM_BUZZER` 已删除，PB9 不是 TIM4_CH4 输出，
  不占 TIM4 通道，因此与舵机（§14，PB8 = TIM4_CH3）没有定时器冲突，两者可同时运行。
- 若板级表里出现与音调同引脚的 PWM 通道，两个后端互斥：同时 open，后开的那个返回
  `PM_ERR_CONFLICT`。端口两侧都检查——`f1_tone_open()` 查 `pwm_uses_gpio()`，
  `f1_pwm_open()` 查 `tone_uses_gpio()`，不依赖调用顺序。
- 音调后端每 10 µs 进一次中断，满频 5 kHz 时中断率约 100 kHz，换频期间会占用可观 CPU；
  主循环有硬实时任务时需评估中断负载（本板没有可用的 PWM 蜂鸣器后端可退）。

### 17.4 怎么在真器件上验证

1. **波形**：示波器/逻辑分析仪测 PB9，100 / 1000 / 5000 Hz 三档，周期误差 ≤ 1 个 tick（10 µs）
   加取整误差；50% 请求下高电平 = 周期 ±1 tick。
2. **静音**：`off()` 后 PB9 恒低；`deinit` 后仍恒低。【待实测】
3. **互斥（仅另行给 PB9 加 PWM 通道时）**：板级表加了 TIM4_CH4 通道后，先开 PWM 再
   `pm_tone_init()` → 期望 `PM_ERR_CONFLICT`；反过来先开音调再开 PWM → 同样
   `PM_ERR_CONFLICT`。两个方向都由端口拒绝；本板默认表里没有这条通道。
4. **声学**：能听到对应音高（手机频谱 App ±3%）。【未验证：板端只证明了 TIM3 10 µs 中断
   翻转这一形态能发声，模型仓这份实现未上板，声学效果未测】

---

## 18. 循迹模型（track，v0.5 新增）

### 18.1 能力边界

- 单路数字量输入：`init` + `read_raw(&on_line)`。不做多路阵列、不做阈值自适应、
  不做去抖（需要时由调用方在主循环做）。
- 极性是**配置项** `active_level`（电平等于它即"有效/压线"），不在驱动里猜。
- 端口只读一个 GPIO，并把引脚配成板级需要的输入形态（PB14 用内部上拉）；不滤波、不中断。

### 18.2 生命周期

- 对象必须零初始化；重复 `init` 返回 `PM_ERR_BUSY`；未 `init` 读取返回 `PM_ERR_STATE`。
- `deinit` 释放引脚占用；幂等。
- `read_raw` 每次直接读电平，不缓存、不排队，可在主循环任意频率调用。

### 18.3 本板引脚与互斥（既有隐患）

- `BOARD_TRACK_LEFT` = PB14，同时是 **SPI2_MISO**。track 与 SPI2 不能同时 open：
  后开的一侧返回 `PM_ERR_CONFLICT`（SPI2 open 时检查 track 是否在用，track open 时
  检查 SPI2 是否在用）。这是板端接线复用造成的既有限制，不是本轮新增。

### 18.4 怎么在真器件上验证

1. 白纸/黑线来回移动，`read_raw` 输出随传感器指示灯翻转；同时用万用表核对 PB14 电平。
2. 打开 SPI2 后再 `pm_track_init()` → 期望 `PM_ERR_CONFLICT`；反之亦然。
3. 拔掉传感器：上拉应让读数稳定在"非有效"，不得随机翻转。
4. 板端证据：PB14 读取路径已在板上走过；模型仓实现未上板，去抖行为未验证。

---

## 19. 正交编码器模型（encoder，v0.5 新增）

### 19.1 能力边界

- AB 两相 **x4 解码**，只维护增量计数 `count`。不做测速、不做低通、不做 Z 相。
  按键是**可选能力**（19.6），默认不启用；本板 PB15 已接按键，调用方置 `has_button = 1`
  即可读取。
- 解码逻辑在 core（纯函数 `pm_encoder_hw_edge(handle, ab)`），端口只做"边沿中断 + 读 AB
  电平 + 调钩子"，因此主机单测能覆盖全部状态转移。

### 19.2 解码规则

模型 core 的状态编码固定为 `AB = A << 1 | B`（bit1 = A、bit0 = B），数值序 00 / 01 / 10 / 11。
合法相邻状态每边沿 ±1：

- 正向：00 → 01 → 11 → 10 → 00（每步 +1）
- 反向：00 → 10 → 11 → 01 → 00（每步 −1）
- 重复同一状态、对角线跳变（00↔11、01↔10）计 0：后者视为丢边/抖动，只做重同步，
  不猜方向、不补偿。

**位序差（板端为准，决策 0018）**：板端 `pm_encoder_ab()` 的打包是 `a | (b << 1)`（bit0 = A、
bit1 = B），与模型相反。模型接板端时由**模型侧适配层**把 `a | (b << 1)` 重排成 `A << 1 | B`；
不允许用 `invert` 抵消位序差——那会把端口打包错误伪装成装配方向反，换板后无法复现。

`invert`（默认 0）是**可选**能力：置 1 时交换 A/B 两相，用于纠正装配相序，不改电气配置。
板端不提供、也不需要 `invert`；没有装配方向证据时保持 0。"正/反"只描述计数符号，
不承诺旋钮的视觉方向（未验证）。

### 19.3 生命周期

- `init` 时端口返回当前 AB 电平，core 采纳为初始状态，**不假设 00**。
- `count` 是 `volatile int32_t`：ISR 写、主循环读；Cortex-M3 上 32 位对齐读写是原子的。
- `reset` 不是中断安全的，只能在边沿中断停用或主循环内调用。
- 未 `init` 时 `pm_encoder_hw_edge` 直接返回，不写状态。

### 19.4 本板引脚

`BOARD_ENCODER_LEFT`：PA8 = A、PA9 = B，内部上拉，双边沿触发，共用 EXTI9_5 中断；
按键 = **PB15**（EC11 的 SW，HARDWARE.md「旋转编码器」行），无外部上拉，端口必须开内部上拉。

PB15 同时是 SPI2_MOSI 的复用脚：按键与 `BOARD_SPI_2` **互斥**，不是两个身份并存。
本板没有空闲 SPI 排针（见 board_f103c8.h），因此按键可用；一旦启用 SPI2，PB15 由外设
驱动，按键既读不到也不能再按（按下等于把 SPI 输出短到地）。

- `EXTI9_5_IRQHandler` 与 `pm_encoder_init()` 必须共用**同一个模块开关**：模块停用/缺席时
  不定义、不使能该中断，不能只关业务；优先级保留 4，不改成 0（决策 0018 第 6 条）。
  板端执行与 `ENC` 计数回归由板端第 5 轮负责。

### 19.5 怎么在真器件上验证

1. 手动正转/反转各 100 格，`count` 符号与数值一致（x4：一格 = 4 个计数）。
2. 悬空或拔线：不得出现随机大跳变（对角线跳变计 0）。
3. 板端证据：计数已有（PATCHES §31.3：正转 25 / 64 / 89，反转 −16 / −28 / −32）；模型实现
   **未上板**，适配层位序重排未实现，`cw/ccw` 对应的视觉方向未验证。

### 19.6 可选按键（v0.6 新增）

**问题**：EC11 / KY-040 类模块普遍带 SW 脚，但不是所有装配都接；板端已经实现了
`pm_encoder_button()`。模型若假定"一定有按键"，没有 SW 的板子会在 `init` 就失败。

**判据（写死）**：

| 配置 | init | `pm_encoder_has_button()` | `pm_encoder_button()` |
| --- | --- | --- | --- |
| `has_button = 0`（默认） | `PM_OK` | `false` | `PM_ERR_DISABLED`，`*pressed = false` |
| `has_button != 0`，端口有 `read_button` | `PM_OK` | `true` | `PM_OK`，返回真实状态 |
| `has_button != 0`，端口无 `read_button` | `PM_ERR_DISABLED` | — | — |
| 未 `init` / 参数为空 | — | `false` | `PM_ERR_STATE` / `PM_ERR_NULL` |

- **"无按键"是答案不是故障**：`PM_ERR_DISABLED` 只表示这条能力没装，调用方不得重试，
  也不应把它当异常上报（与 §20.4 的 DISABLED 语义一致）。
- **极性归端口**：`read_button` 返回逻辑值（`true` = 按下）。端口既然负责配上下拉，
  就由端口定义电气含义；EC11/KY-040 这类模块的 SW 对地闭合、内部上拉，因此"低 = 按下"。
- **不引入中断**：按键只做轮询，不做消抖、不做边沿中断；需要消抖的用法由调用方在主循环做。
- **本板已接按键**：PB15 = EC11 的 SW（HARDWARE.md 实测接线），引脚表按
  `button_port = GPIOB / button_pin = GPIO_PIN_15` 记录；`has_button` 仍由调用方显式置 1，
  默认 0 只表示"不用这个能力"，不表示"板上没有"。PB15 的 AF 是 SPI2_MOSI，与 SPI2 互斥
  （见 19.4）。

---

## 20. 定时器资源仲裁

### 20.1 为什么需要：TIM2 事故

- 事实：本板 PA0/PA1 只能复用 **TIM2_CH1/CH2** 做电机 PWM；同一颗 TIM2 又被 HC-SR04
  当作 1 µs 自由运行计数器（读 `TIM2->CNT` 算回波宽度）。两者共用 PSC/ARR。
- 事故：测距时 TIM2 以 1 µs 计数、16 位满量程 65 ms；电机要 20 kHz PWM，PSC = 71 时
  周期只有 50 个计数，`ARR ≈ 49`。ARR 一改，测距量程从 65 ms 掉到几十 µs，功能直接废掉，
  且**没有任何报错**。
- 结论：定时器是共享资源，必须显式声明与仲裁，不许后写者静默覆盖前者。

### 20.2 占用声明

- 任何占用定时器的模块，在 `init`/`open` 里、写寄存器**之前**调用
  `pm_timer_claim()`，声明：`hw_id`（逻辑定时器号）、`owner_id`（占用方自选，同一实例内
  唯一）、`role`、`psc`、`arr`、`channels`（bit0..3 = CH1..CH4，无比较通道填 0）。
- `owner_id` 用于 release/retime 定位；同一 owner 同配置重复声明是幂等的，不同配置
  返回 `PM_ERR_CONFLICT`。
- 所有失败路径（含 HAL 初始化失败）都必须 `pm_timer_release()`，不留脏声明。
- 表是固定的 8 条静态表，`claim`/`release` 在主循环调用，不在 ISR 里调用。
- 占用表只能看见"经本模型申请"的占用；裸用寄存器的代码看不见。端口必须提供外部占用
  探测，见 20.7。
- **查询占用方**：`pm_timer_claims(hw_id)` 取该定时器的 live claim 数量；
  `pm_timer_claim_at(hw_id, index, &claim)` 取出第 `index` 条（0 起，按表内顺序）的
  `owner_id`、`role`、`psc`、`arr`、`channels`。`hw_id` 越界或 `index` 超出数量返回
  `PM_ERR_RANGE`，`out` 为 `NULL` 返回 `PM_ERR_NULL`。
- **未 claim 直接使用：允许**（写死）。表只约束"走本 API 的调用方"，模型不会、也无法拦截
  裸写寄存器的代码；这类使用一旦发生，端口探测（20.7）会让后续 `claim` 返回
  `PM_ERR_BUSY`，模型不会替它接管。放弃 claim 就等于放弃冲突保护，这是使用者的选择，
  不是模型的静默降级。

### 20.3 判据（写死，不靠实现推断）

| 已占用 | 新请求 | 结果 |
| --- | --- | --- |
| 空闲 | 任意合法声明 | 允许 |
| PWM | PWM，**同 PSC/ARR 且通道不重叠** | 允许（共用时基） |
| PWM | PWM，PSC 或 ARR 不同 | `PM_ERR_CONFLICT` |
| PWM | PWM，通道重叠 | `PM_ERR_CONFLICT` |
| PWM | TONE / COUNTER | `PM_ERR_CONFLICT` |
| TONE | TONE / PWM / COUNTER | `PM_ERR_CONFLICT` |
| COUNTER | 任意（含 COUNTER） | `PM_ERR_CONFLICT` |

- 角色定义：`PWM` = 比较输出，必须有通道；`TONE` = 只用更新中断，`channels = 0`；
  `COUNTER` = 自由运行计数，`channels = 0` 且 `arr = 0xFFFF`。
- 判据来源：一颗 TIM 只有一个 PSC/ARR，所以"同周期 + 不同通道"可共存，其余一律互斥；
  自由运行计数与任何输出互斥（ARR 拉满，改 ARR 就毁量程）。
- 表满返回 `PM_ERR_NOMEM`；非法声明（越界 hw_id、PWM 无通道、COUNTER 的 ARR 不是满量程）
  返回 `PM_ERR_PARAM`。

### 20.4 错误码语义（写死）

- `PM_ERR_CONFLICT`：**共享资源已被不兼容配置占用，重试不会成功**。要么换配置，要么等
  对方 release。**不允许**用它表达"同一对象重复 init"，也不再用它表达"定时器已被外部
  代码启动"（那是 `PM_ERR_BUSY`，见下条与 20.7）。
- `PM_ERR_BUSY`：同一对象/同一硬件实例重复 init；PWM 兄弟通道请求不同频率；或该定时器
  已被**非本模型代码**启动/重配（20.7 探测为真）。前两种是本模型内部的状态冲突，第三种
  是"资源不归本模型管"，三者都**不该重试**，区别在于：前两种自己 release/换通道即可解，
  第三种要么让人去停掉外部使用者，要么按 20.5 报 `PM_ERR_DISABLED`。
- `PM_ERR_DISABLED`：功能被**有意关闭**（例如测距因 TIM2 让给电机而停用），调用方不得
  当作可用，也不该重试。与 `PM_ERR_CONFLICT` 的区别：CONFLICT 可修（换配置/换资源），
  DISABLED 需要换设计。

### 20.5 功能降级原则

- 功能被仲裁挡住时**必须显式报错，不许静默失效**：`init` 返回 `PM_ERR_CONFLICT`；
  已经被设计性停用的功能返回 `PM_ERR_DISABLED` 并说明原因。
- 正面例子：测距停用时返回 `err = disabled`，而不是返回 0 让上层以为在测。
- 反面例子：本轮事故里 ARR 被覆盖、测距照常返回"数据"，只是量程变成几十 µs。
- **停用模块必须连中断一起撤掉**：`init` 不使能、ISR 不定义（与构建开关同条件），不能只关
  业务而留下 EXTI/NVIC 使能。反例（板端缺陷）：sonar 已设计性停用，`EXTI10` 仍使能，
  `EXTI15_10_IRQHandler` 未设优先级（默认为 0，最高），ISR 还在读电机占用的 `TIM2`。
  encoder 的 `EXTI9_5_IRQHandler` 同理，见 §19.4（决策 0018 第 6 条）。

### 20.6 怎么验证

- 主机单测 `test_timer_arbiter()`：两颗模块抢同一颗 TIM 时第二个 `claim` 失败；同 PSC/ARR
  不同通道共存；通道重叠失败；`retime` 在混合角色下失败；`release` 后可重新声明（含换
  `owner_id`）；表满返回 `PM_ERR_NOMEM`；`pm_timer_claim_at()` 按 index 取回 `owner_id`
  与配置，越界返回 `PM_ERR_RANGE`；探测为真时 `claim`/`check` 返回 `PM_ERR_BUSY`，而表内
  不兼容占用仍返回 `PM_ERR_CONFLICT`——两种拒绝在同一轮里可区分。
- 板上：舵机（TIM4）+ 音调（TIM3）应同时可用；把音调的 `hw_id` 改成 4（TIM4）再 `init`
  → 期望 `PM_ERR_CONFLICT`。【待实测】

### 20.7 外部占用探测（v0.6 新增）

**问题**：占用表只记"经模型申请"的占用。板端早期（v0.6 之前）`pm_motor_stm32f1.c`
只查 `TIM2->CR1.CEN` 就接管 TIM2，`main.c` 的测距 ISR 也只读 `TIM2->CNT`。这类**裸占用
不经过本 API**，表里是空的；若模型此时直接写 PSC/ARR，就重现 20.1 的事故。

**硬性要求**：端口必须提供一条探测：

```c
typedef bool (*pm_timer_probe_fn)(void *ctx, uint8_t hw_id);
void pm_timer_set_probe(pm_timer_probe_fn fn, void *ctx);
```

| 项 | 判据（写死） |
| --- | --- |
| 探测输入 | 逻辑定时器号 `hw_id`（与 `claim` 同一个编号体系） |
| 返回 `true` | 该定时器已被**非本模型代码**启动或重配 |
| 副作用 | 只读寄存器，不许写；不许清标志、不许停计数 |
| 安装者 | 端口；核心不碰硬件 |
| 撤销 | `pm_timer_set_probe(NULL, NULL)`；`pm_timer_reset()` 一并撤销 |

- **探测时机**：仅当该 `hw_id` 在表里**没有任何 live claim** 时调用。一旦模型自己已经
  启动时基，`CR1.CEN` 是模型置的，再探测会把自家兄弟通道判成外部占用。
- **返回值（v0.8.0 起）**：探测为 `true` → `pm_timer_claim()` 与 `pm_timer_check()` 都返回
  `PM_ERR_BUSY`，**表不变、寄存器不变**。不静默接管，也不自动降级。v0.7.1 及之前这里返回
  的是 `PM_ERR_CONFLICT`：外部裸占用被归进"换配置可解"一类，实际换配置也解不了，属于早期
  措辞便宜，现已改正。接入方若用 `== PM_ERR_CONFLICT` 判断"外部忙"，需改判 `PM_ERR_BUSY`。
- **探测判据（只有两条，满足任一即判占用）**：`TIMx->CR1.CEN != 0`，或 `AFIO->MAPR` 中
  对应重映射位非 0；未知 `hw_id` 一律返回 `true`（不认识就不接管）。
- **本板实现**（`pm_port_stm32f1.c`）：`hw_id` **2/3/4** 直接对应 TIM2/TIM3/TIM4
  （取值即定时器序号，日志与参考手册读法一致）；该探测在 `pm_port_get()` 里安装一次。
- **板端现状**：检查已从 `pm_motor_stm32f1.c` 抽出为独立函数 `pm_timer_probe()`；
  `AFIO->MAPR` 查的是 `TIM2_REMAP` 的**全 2 位字段**（不是只查 `TIM2_REMAP_0`，
  只置高位的组合同样算外部重配）；覆盖 TIM2/TIM3/TIM4 三颗。
- **运行期占用表延后（决策 0018 第 5 条）**：板端当前只有探测，没有 claim/release/retime
  表；§20.2/20.3 的表是本模型仓的实现，板端接入留独立一轮，探测与表不混做。
- 与 20.5 的关系：外部占用属于 `PM_ERR_BUSY`（v0.8.0 起；v0.7.1 及之前误归
  `PM_ERR_CONFLICT`）。它不是 `PM_ERR_DISABLED`（那是指功能被设计性关闭），也不该被当成
  可重试的 `PM_ERR_CONFLICT`：外部使用者不归本模型管，重试多少次都不会变。

---

## 21. 与板端仓库的已知差异与证据

### 21.1 已知差异

- **API 形态分叉**：模型仓用句柄 + `pm_err_t`（`pm_pwm_t *`、返回负值错误码）；板端仓用
  单例 + `int` 返回。原因写死为"板端为验证速度选择单例形态"。**不回改板端**，以模型仓
  自身一致性为准。
- **板端仓的 CI 覆盖缺口**：板端仓没有主机单测作业，也没有固件构建作业；
  `tests/host/test_main.c` 里 PWM / track / encoder 一条测试都没有。这些模块的回归保护
  目前只存在于模型仓。

### 21.2 证据表

| 条目 | 证据 | 状态 |
| --- | --- | --- |
| PWM 时基 3.6 kHz 反例 | 板端实测：舵机完全不动作，PSC 0 / ARR 19999 | 板端已验证；模型实现未上板（数学有主机单测） |
| 舵机 50 Hz 时基 | 板端实测：PSC 71 / ARR 19999 改后 50 Hz、舵机动作（PATCHES §32.3） | 板端已验证；模型的 `matches()` 复核未上板 |
| TIM2 被电机与测距共用 | 板端现象：ARR 改动后测距量程 65 ms → 几十 µs | 板端已验证；仲裁代码有主机单测 |
| 电机 CCR 落点（20 kHz） | 板端：30% → 1080、50% → 1800、80% → 2880、100% → 3600（PATCHES §35.3） | 板端已验证 PWM 落点；机械方向、转速与 brake 未验证 |
| 蜂鸣器软件音调 | 板端 TIM3 10 µs 中断翻转 PB9 能发声 | **命令与参数路径有证据；声学未验证**，模型实现未上板 |
| 循迹 PB14 数字量读取 | 板端读数与指示灯一致 | 板端已验证；模型实现未上板，去抖未验证 |
| 编码器 AB x4 解码 | 板端串口：正转 25 / 64 / 89，反转 −16 / −28 / −32（PATCHES §31.3） | 板端已验证计数；模型实现未上板，位序适配层未实现，视觉方向未验证 |
| NTC 方向 | 手指捏住后 ADC 原始值 2293 → 1792 单调下降 | 板端已验证（§13.7） |
| PWM 0‰/1000‰ 与停止电平 | 无板端证据 | 未验证（§14.6 中标注【待实测】） |
| 音调/PWM 在 PB9 上的互斥 | 无板端证据 | 未验证（代码写死，主机单测以 mock 覆盖） |

### 21.3 维护规则

- 新增板端证据时在 §21.2 追加一行并写日期与测量条件；没有证据的条款一律按"未验证"处理。
- 板端与模型仓的差异只记录、不顺手统一：统一需要板端负责人同意，走协作流程。

### 21.4 可静态判定条目标注（供检查器线取材）

判定口径：**能在不运行、不看板端的前提下，仅凭源码结构/符号位置/头文件字段给出结论**才算
"可静态判定"。只能靠单测或板端现象判断的写"单测/运行时"，说不清的写"判不出"，不包装成规则。

| 条款 | 静态可判定 | 判据 / 做法 | 备注 |
| --- | --- | --- | --- |
| §20.2 占用表唯一实现 | 是（强） | `pm_timer_claim/release` 的函数定义只允许出现在 `src/common/pm_timer.c` | 符号定义去重，误报低 |
| §19.3 `count` 为 `volatile int32_t` | 是（强） | `pm_encoder_t` 字段声明匹配 `volatile` + `int32_t` | 头文件字段检查 |
| §2 common 层纯度 | 是（强） | `src/common/*.c` 与 common 契约头不得出现厂商头、`HAL_` 前缀符号、寄存器访问 | 扩展现有 include 规则 |
| §18.1 极性是配置项 | 是（弱） | `pm_track_cfg_t` 必须含 `active_level` 字段 | 只证明"有配置项"，不证明被正确使用 |
| §17.1 接口不含 tick 常量 | 是（弱） | `pm_tone.h` 公开类型/签名中不出现 `tick` 类命名 | 改命名即可绕过 |
| §19.1 解码在 core | 是（弱） | `pm_encoder_hw_edge` 定义在 `src/core/pm_encoder.c`，端口文件无状态转移表 | 表匹配易误报 |
| §17.1 tone 必须走仲裁 | 是（弱） | 端口 tone open 函数体内出现 `pm_timer_claim` 且 role 为 TONE | "函数体内必须含某调用"，误报中等 |
| §17.1 频率/占空比范围 | 否（单测） | 运行时参数校验，由主机单测覆盖 | 非静态 |
| §20.3 兼容判据 | 否（单测） | 表驱动行为，由 `test_timer_arbiter()` 覆盖 | 非静态 |
| §19.3 未 init 时钩子返回 | 否（单测） | 运行时状态判断 | 非静态 |
| §20.2 "claim 成功后才写 PSC/ARR" | 否 | 需要 claim 返回值到写寄存器之间的支配关系 | 弱结构检查误报多，不建议做 ERROR |
| §18.3 track/SPI2 两侧互斥 | 否 | 需要跨文件"两处都检查"的数据流 | 建议单测覆盖 |
| §17.3 PB9 两后端互斥 | 否 | 跨模块资源关系 | 运行时/单测 |
| §20.4 错误码语义不混用 | 否 | 静态无法区分调用点意图 | 人工评审 |
| §20.5 降级必须显式报错 | 否 | 需要"该功能本应工作"的语义知识 | 人工评审 |
| §21.2 证据表 | 否 | 人工维护的文档事实 | — |

优先级建议：前三条（定义唯一、`volatile` 字段、common 纯度）误报低，适合直接做成 ERROR 级
规则；其余"是（弱）"的做成 WARN，并接受改名即可绕过的局限。

---

## 22. 继电器模型（pm_relay）

### 22.1 形态

- **设备级模型**：组合 `pm_gpio` 这类已有原语，不新增端口结构，也不新增板级表。
  引脚由调用方以 `pm_gpio_pin_t` 给出；继电器只占一个 GPIO，再造一张端口表等于把板级事实
  复制两遍。
- **本仓取舍**：设备级模型只组合基础模型、不新造端口。缺端口文件不是漏写，是这类模型的
  既定形态；需要新端口原语的才配端口适配。
- 句柄形态：`struct pm_relay` 可实例化多个，互不影响；无单例。

### 22.2 极性（必填，不许默认）

- `pm_relay_cfg_t.active_level` 是三值枚举：`UNSET / LOW / HIGH`。
- **`UNSET` 表示"调用方没有决定"，`init` 返回 `PM_ERR_PARAM`，模型不猜。** 零初始化的配置
  天然是 `UNSET`，因此"忘了填"一定会在 init 处暴露，而不是悄悄按高有效吸合。
- 理由：极性接反意味着上电即吸合，属于不可静默默认的硬件事实。

### 22.3 失效安全

- `init` 成功后继电器处于**断开（去激励）**状态；需要闭合必须显式调 `pm_relay_on()`。
- `deinit` 先把引脚写回无效电平再释放对象，拆解过程不会把负载留在通电状态；`deinit` 幂等。
- 未初始化调用 `on/off/toggle/set` 返回 `PM_ERR_STATE`；`is_on` 对未初始化对象返回 `false`。

### 22.4 验证

- 主机单测 `test_relay()`：高低有效双向映射、`UNSET` 拒绝、未定义引脚报 `PM_ERR_RANGE`、
  两实例交叉操作互不影响、`deinit` 回无效电平、重复 init 报 `PM_ERR_BUSY`。
- 板端：**未验证**（本轮无板端证据）。

---

## 23. 电机模型（pm_motor）

### 23.1 器件形态依赖（先看这条）

`pm_motor` **只针对 DRV8833 形态**：每桥两个输入脚（IN1/IN2），两脚都是 PWM 输入，
没有独立使能脚。真值表（TI 数据手册）：

| IN1 | IN2 | OUT1/OUT2 | 含义 |
| --- | --- | --- | --- |
| 0 | 0 | Z / Z | 滑行（coast，快速衰减） |
| 1 | 0 | H / L | 正转 |
| 0 | 1 | L / H | 反转 |
| 1 | 1 | L / L | 刹车（brake，两低边导通，慢速衰减） |

**不适用于 L298N（ENA + IN1 + IN2）**。原因不是接线口味：DRV8833 反向时占空比要移到
**另一只输入脚**上，而 L298N 的占空比固定在 ENA，两个方向脚是纯 GPIO——GPIO 给不出占空比。
拿 L298N 套这个模型，反向只能变成"全速反转"，无法调速。用 L298N 的人看到这一段就该换模型，
而不是改模型。

### 23.2 通道与频率

- 模型持**两个** `pm_pwm_t*`，分别接 IN1/IN2；通道由调用方初始化，模型不拥有、不改频率。
- `init` 读取两通道频率：任一未打开返回 `PM_ERR_STATE`；两通道频率不等返回
  **`PM_ERR_CONFLICT`**。这与 §20 的定时器仲裁同构——同一颗定时器两个通道却在两个 PSC/ARR
  上跑，就是板端 TIM2 事故的翻版，必须在 init 处挡住，不许靠调用方自觉。
- 两个指针相同返回 `PM_ERR_PARAM`：同一通道做两路输入，滑行与刹车都表达不出来。

### 23.3 方向与占空比

- `set(duty, FORWARD)`：IN1（板端 CH1）= duty，IN2（板端 CH2）= 0；`REVERSE` 镜像。
  `FORWARD` 只表示"选了 IN1 这一路"，不表示机械向前——机械方向未实测（决策 0018 第 1 条）。
- 换向时先把两路都写 0 再驱动目标路，任何换向都不会经过 `1/1`（刹车）这个中间态。
- `invert` 交换两路角色（左轮/右轮反装用），是**模型可选能力**，不是板端迁移条件：
  板端不提供 `invert`，`FWD` 固定走 CH1，接板端时保持零初始化（`PM_MOTOR_INVERT_UNSET`）即可。
- `PM_MOTOR_INVERT_UNSET`（零初始化天然值）等同 `OFF`，不做交换；`OFF`/`ON` 语义不变；
  `UNSET/OFF/ON` 之外的取值一律 `PM_ERR_PARAM`。默认行为与旧版显式 `OFF` 完全一致。

### 23.4 两种停止

| 调用 | 电气状态 | `pm_motor_state` |
| --- | --- | --- |
| `set(0, 任意方向)` | 两路 0 / 0（Z） | `STOP_COAST` |
| `stop(COAST)` | 两路 0 / 0（Z） | `STOP_COAST` |
| `stop(BRAKE)` | 两路 1000 / 1000（L/L） | `STOP_BRAKE`（板端未实现） |

- **`duty 0` 与 `stop(COAST)` 电气等价、与 `stop(BRAKE)` 不等价**，三条都有断言
  （见 §23.6），不是注释。
- `stop(BRAKE)` 的实现是给两路各写 `duty = 1000`，即"**近似恒高**"：`pm_pwm` 在 1000 处
  会丢一个 tick，brake 期间这个抖动不影响结果；但它是"100% 占空比请求"，**不是切断 PWM
  输出**，也不是把引脚切成静态电平。
- brake 期间调用 `set()` 必须立即退出 brake：实现先把两路写 0 再驱动，duty 不会残留在 1000。
- 板端当前 `stop` 是 `0/0`，与 `stop(COAST)` 对齐；`stop(BRAKE)` **板端未实现、未实测**，
  登记为技术债（决策 0018 第 4 条）。触发条件：电机带载后完成刹车电流、温升、电源跌落实测，
  再开独立一轮。在此之前板端 `stop` 一律按 COAST 理解，不新增 `MOTOR brake` 命令。

### 23.5 衰减模式

只支持**快速衰减**（`PWM / 0`，关断期滑行），即板端现在的驱动方式。慢速衰减
（`1 / PWM`，关断期刹车）**未实现、不提供开关**——留半成品开关比不提供更难排查。

### 23.6 验证

- 主机单测 `test_motor()`：`duty 0` / coast / brake 三者关系、反向确实把占空比移到另一路
  （`REV 300` → IN1=0 / IN2=300）、频率不等报 `PM_ERR_CONFLICT`、同通道复用报
  `PM_ERR_PARAM`、未打开通道报 `PM_ERR_STATE`、brake 后 `set()` 立即退出、invert 交换、
  两实例交叉操作互不影响、`deinit` 回滑行。
- 板端：PWM 落点已验证（30% → 1080、50% → 1800、80% → 2880、100% → 3600，PATCHES §35.3）；
  电机是否转动、机械方向、转速与 brake 均**未验证**（PATCHES §35.4；DRV8833 真值表依据数据手册）。

---

## 24. 舵机模型（pm_servo，v0.6 新增）

### 24.1 为什么单独成模型

舵机的电气接口不变（一根 PWM，50 Hz 或更高帧率），变的是**角度与脉宽的映射**：SG90
是 0–180° ↔ 500–2500 µs，MG996R 端点不同，数字舵机要 333 Hz。把这个映射塞进 `pm_pwm`
会让"通用 PWM"绑死一种器件；参数化后换舵机只改四个数，不改逻辑。这就是本仓
"设备级模型组合基础模型"（§22.1）的第二个实例：`pm_servo` 组合 `pm_pwm`，不新造端口、
不建板级表。

### 24.2 角度 → 脉宽（纯函数，契约核心）

```c
pm_err_t pm_servo_pulse_from_angle(const pm_servo_range_t *range,
                                   uint16_t angle_deg, uint16_t *pulse_us);
```

| 项 | 判据（写死） |
| --- | --- |
| 定义域 | `angle_min_deg < angle_max_deg`、`0 < pulse_min_us < pulse_max_us`，否则 `PM_ERR_PARAM` |
| 越界 | `angle_deg` 不在闭区间内 → `PM_ERR_RANGE`；**不夹紧、不取最近端** |
| 换算 | `pulse = p_min + (angle − a_min) × (p_max − p_min) / (a_max − a_min)` |
| 取整 | 四舍五入到 1 µs：`(offset × p_span + a_span/2) / a_span`，整数运算，无浮点 |
| 端点 | `a_min → p_min`、`a_max → p_max` 必须精确成立 |
| 空指针 | `PM_ERR_NULL` |

参考端点：`PM_SERVO_SG90_*` = 0–180° ↔ 500–2500 µs。90° 必须得 1500 µs。

### 24.3 设备形态与帧率约束

- `pm_servo_cfg_t { hw_id, freq_hz, range, start_angle_deg }`，四项全部必填；
  `start_angle_deg` 不设默认值——由调用方显式给出初始位置，避免"init 偷偷动一下"。
- `init` 先按 0‰ 打开 `pm_pwm`，再驱动 `start_angle_deg`；任一步失败都 `pm_pwm_deinit`
  并清空对象，不留半开通道。
- **帧率约束**：`pulse_max_us` 必须 ≤ `1 000 000 / freq_hz`（一个周期内放得下整个脉宽
  范围），否则 `PM_ERR_RANGE`。`freq_hz` 为 0 或 > 1 MHz → `PM_ERR_PARAM`。
- 脉宽 → 占空比按**通道实际帧率**换算：`duty‰ = round(pulse_us × 1000 / period_us)`；
  若实际周期短于脉宽 → `PM_ERR_RANGE`，不夹到 1000‰。
- `pm_pwm` 的既有约束（同一 TIM 共享周期、通道不重叠）原样继承，冲突由 `pm_pwm` /
  `pm_timer` 报 `PM_ERR_CONFLICT`（§20）。

### 24.4 生命周期与状态

- `init`：`PM_ERR_NULL` / `PM_ERR_PARAM` / `PM_ERR_RANGE` / `PM_ERR_BUSY`（已打开）/
  透传 `pm_pwm_init` 的错误。
- `set_angle`：越界 → `PM_ERR_RANGE`，**保持上一位置**（不写占空比）；成功后才更新
  `angle_deg`。
- `get_angle`：返回最后成功下发的角度；未初始化 → `PM_ERR_STATE`。
- `deinit`：停输出、关通道、清空对象；未初始化 → `PM_ERR_STATE`。
- 本模型不做斜坡、不做限速、不做堵转检测；这些属于上层运动控制。

### 24.5 验证

- 主机 `test_servo()`：端点精确性（0/90/180 → 500/1500/2500）、自定义区间
  （10–170° ↔ 600–2400 µs）、越界 `PM_ERR_RANGE` 且占空比不变、非法区间/零脉宽
  → `PM_ERR_PARAM`、`freq_hz=0` → `PM_ERR_PARAM`、`pulse_max` 超过帧长 → `PM_ERR_RANGE`、
  非 50 Hz 帧率（333 Hz）换算、`init` 后 `angle_deg` 与占空比一致、重复 `init` → `PM_ERR_BUSY`、
  `deinit` 后查询 → `PM_ERR_STATE`。
- 板端：**未验证**。本板舵机在 PB8 / TIM4_CH3（`BOARD_PWM_SERVO`），需上板看实际转角。
