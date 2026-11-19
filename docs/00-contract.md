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
```

硬性规则：

1. **向下依赖，禁止反向 include**。`pm_core` 不得 include 任何厂商头或 port 内部头。
2. **厂商类型不出 port**。`GPIO_TypeDef`、`UART_HandleTypeDef`、`USART_TypeDef` 不得出现在 `include/pm/` 下的任何头文件里。
3. **上层不 cast 句柄**。`pm_gpio_pin_t` 只有一个 `id` 字段，其含义由板级表定义，上层不得假设 id 与物理引脚号的对应关系。
4. **ISR 反向通知走内部钩子**。port 的 ISR 只能调 `pm_uart_hw_*`，不得直接改模型字段、不得调用用户回调。
5. **只有一个 `pm_port_get()`**。同一份固件里由链接决定后端，不同时存在两个端口实现。

依赖矩阵：

| 层 | 允许依赖 | 禁止依赖 |
| --- | --- | --- |
| APP | `include/pm/*` | port、HAL、寄存器 |
| pm_core | `include/pm/*`、`pm_osal.h` | port 内部头、HAL |
| pm_port | `pm_port_get` 的义务、HAL、CMSIS | APP |
| board | port 内部头、芯片头 | APP、pm_core |

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
- 当前覆盖：环形缓冲（含回绕、丢弃、consume）、事件队列（去重、溢出）、GPIO 参数校验、UART 生命周期、TX 流控、RX 投递、超时路径、SPI 传输/中止/DMA 标志、I2C 写/读/写读/中止/错误。共 244 项断言，全部通过。
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
- 不做输入捕获、编码器、单脉冲、死区、互补输出、刹车与中断回调。
- 频率与占空比在运行中可改；占空比单位是千分比（0～1000），不使用浮点。
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

### 14.4 本板通道

| 逻辑通道 | 引脚 | 定时器通道 | 典型用途 |
| --- | --- | --- | --- |
| `BOARD_PWM_MOTOR_A` | PA0 | TIM2_CH1 | DRV8833 A 路 |
| `BOARD_PWM_MOTOR_B` | PA1 | TIM2_CH2 | DRV8833 B 路 |
| `BOARD_PWM_SERVO` | PB8 | TIM4_CH3 | 舵机，50 Hz |
| `BOARD_PWM_BUZZER` | PB9 | TIM4_CH4 | 无源蜂鸣器，建议 20% 占空比 |

- PA0/PA1 同属 TIM2，PB8/PB9 同属 TIM4：**同组通道频率必须一致**。
  舵机（50 Hz）与蜂鸣器（约 2 kHz）不能同时在 TIM4 上以不同频率运行。
- 定时器时钟来自 APB1 ×2（APB1 分频不为 1 时），PSC 先选够大以保证 ARR 落在 16 位内。
- 引脚配置为 AF 推挽、无上下拉、高速。

### 14.5 真实器件的必要条件

**频率与分辨率的相互约束**

- 定时器时钟 `f_tim = 72 MHz`（APB1 = 36 MHz 且预分频 ≠ 1 时 ×2）。周期计数
  `N = f_tim / ((PSC + 1) * freq)`，必须满足 `1 ≤ N ≤ 65536`；port 先选 PSC 让 N 落进 16 位。
- 占空比分辨率是 `1/N`：千分比（0～1000）只有在 `N ≥ 1000` 时才有 1‰ 的台阶；
  `N < 1000` 时实际台阶是 `1/N`，`pm_pwm_set_duty()` 会把请求值截断到最近的 CCR。
- 本板定时器时钟下的典型值：

  | 频率 | PSC / ARR | 周期计数 N | 占空比台阶 | 有效频率 |
  | --- | --- | --- | --- | --- |
  | 1 Hz | 1098 / 65513 | 65 514 | 0.015‰ | 1.000002 Hz |
  | 50 Hz（舵机） | 21 / 65453 | 65 454 | 0.015‰（约 0.3 µs） | 50.0004 Hz |
  | 2 kHz（蜂鸣器） | 0 / 35999 | 36 000 | 0.028‰ | 2 000 Hz |
  | 20 kHz | 0 / 3599 | 3 600 | 0.28‰ | 20 000 Hz |
  | 100 kHz | 0 / 719 | 720 | 1.4‰ | 100 000 Hz |
  | 1 MHz | 0 / 71 | 72 | 13.9‰ | 1 000 000 Hz |

  （表内为 port 的取整结果，`pm_pwm_frequency_hz()` 返回有效值；理论推算，待实测复核。）
- 结论：频率越高，占空比与脉宽越粗。1 MHz 时最小可调占空比约 1.4%，不要用它做 1‰ 级的调节。
- `pm_pwm_set_pulse_us()` 用**有效频率**（`pm_pwm_frequency_hz()`）换算，结果截断到整数千分比：
  50 Hz 下 1‰ = 20 µs，所以脉宽步长是 20 µs（例：1501 µs → 75.05‰ → 75‰ = 1500 µs）。
  CCR 本身的台阶约 0.3 µs，但经千分比换算后以 20 µs 为步长，SG90 可直接使用。
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

- TIM2：PA0（MOTOR_A）、PA1（MOTOR_B）；TIM4：PB8（SERVO）、PB9（BUZZER）。
  同组共享 PSC/ARR（周期），占空比各自独立。
- 兄弟通道已打开且请求频率不同时，`pm_pwm_init()` / `pm_pwm_set_frequency()` 返回
  `PM_ERR_BUSY`，周期保持不变；请求相同频率则允许，且 port 会按各通道保存的千分比
  重算全部 CCR（换频后所有兄弟通道的占空比台阶一起变粗或变细）。
- 因此舵机 50 Hz 与蜂鸣器 2 kHz **不能同时在 TIM4**：先 `deinit` 一个，或把蜂鸣器换到
  未映射的定时器（本板未接）。
- 模型没有中断与事件，改占空比不需要等回调；但 API 不是中断安全的（模型不加临界区），
  统一在主循环调用。

**开漏 / 推挽与上拉的选择**

- 本模型固定 **AF 推挽、无内部上下拉、高速**（§14.2、§14.4），没有配置项，也不新增配置项。
- 推挽输出会主动驱动高低电平，**不能与其他器件"线与"**；I²C 等开漏总线不能用 PWM 通道驱动。
- 需要开漏（共阳 LED、共享信号线）时，用外部三极管 / MOS 反相，或改用 GPIO 软件 PWM
  （不属于本契约）。
- 负载是高阻（MOS 栅极、长线）时，内部上拉不启用，必须在外部加下拉/上拉，否则停止输出后
  电平不确定。
- 本板蜂鸣器经 Q4（SS8050，NPN）驱动，按 HARDWARE 建议保持 20% 占空比；不要长时间 100%。

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
   - 蜂鸣器通道：2 kHz、200‰ → 周期 500 µs ±1%，高电平 100 µs ±2 µs。
   - 判据：占空比误差 ≤ `1/N + 1` 个计数（50 Hz 时 ≈ 0.3 µs，2 kHz 时 ≈ 0.03 µs）。
2. **边界占空比**
   - 0‰：低电平直线，无任何窄脉冲。
   - 1000‰：高电平直线；只有 `N = 65536` 的频点允许每周期 1 个计数（约 14 ns）的低脉冲，
     需 ≥ 100 MHz 带宽才看得到。其余频点应为纯高。
3. **舵机（PB8，TIM4_CH3，50 Hz）**
   - 依次 `pm_pwm_set_pulse_us()` 500 / 1500 / 2500 µs，每档停 2 s。
   - 判据：分别到一端、中位、另一端；**停留 10 s 不回弹、不抖动**；中位与机械中位偏差 < 5°。
     回弹/抖动 → 先看周期是否稳定（供电跌落或频率漂移）。
   - 超过 2500 µs 的请求会被截断到 1000‰（20 ms 常高），舵机应停在极限位置而不是继续转。
4. **蜂鸣器（PB9，TIM4_CH4，2 kHz，20%）**
   - 判据：能听到 2 kHz 单音（手机频谱 App 测 ±3%）；改 500 Hz / 4 kHz 音高随之变化；
     20% 下供电电流明显小于 50%（万用表串在 5 V 路径，只做相对比较）。【待实测】
5. **共定时器约束**
   - 先 init 舵机（50 Hz），再 init 蜂鸣器（2 kHz）→ 期望 `PM_ERR_BUSY`。
   - 蜂鸣器 `deinit` 后以 50 Hz 再 init → 应成功，且舵机占空比不变（示波器复核）。
   - 蜂鸣器打开时对舵机 `pm_pwm_set_frequency(60 Hz)` → 期望 `PM_ERR_BUSY`，
     示波器确认舵机周期仍是 20 ms。
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

1. 公共头放在 `include/pm/pm_<periph>.h`，只含标准类型；
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

- 本仓库当前为 v0.4，接口可能在不通知的情况下调整。
- 结构体布局不承诺 ABI 稳定；跨版本升级请重新编译全部源码。
- 契约条款的修改必须同时更新本文件，不允许"代码改了文档没改"。
