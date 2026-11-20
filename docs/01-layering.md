# 目录、分层与移植

## 1. 目录结构

```
work/
├── docs/                         契约与移植说明
│   ├── 00-contract.md            接口契约（规范）
│   └── 01-layering.md            本文件：目录、依赖、时序、移植
├── include/pm/                   公共头（不含任何厂商类型）
│   ├── pm_common.h               基础类型、编译期工具、内存屏障
│   ├── pm_err.h                  统一错误码
│   ├── pm_osal.h                 时基、延时、临界区
│   ├── pm_event.h                事件类型与事件队列（SPSC）
│   ├── pm_ringbuf.h              环形缓冲（SPSC）
│   ├── pm_port.h                 后端 ops 与注册表（唯一接缝）
│   ├── pm_gpio.h                 GPIO 模型 API
│   ├── pm_uart.h                 UART 模型 API
│   ├── pm_spi.h                  SPI 主机模型 API
│   ├── pm_i2c.h                  I2C 主机模型 API
│   ├── pm_adc.h                  ADC 单通道单次转换 API
│   ├── pm_pwm.h                  PWM 输出通道 API
│   ├── pm_pwm_math.h             PWM 时基数学（PSC/ARR、CCR 换算，common 层契约头）
│   ├── pm_timer.h                定时器占用与仲裁（common 层契约头）
│   ├── pm_tone.h                 软件音调 API
│   ├── pm_track.h                单路循迹 API
│   ├── pm_encoder.h              正交编码器 API（AB x4，可选按键）
│   ├── pm_relay.h                单路继电器 API
│   ├── pm_motor.h                电机模型 API（DRV8833 双 PWM 输入）
│   └── pm_servo.h                舵机角度接口（组合 pm_pwm）
├── src/
│   ├── core/                     与硬件无关的模型实现
│   │   ├── pm_err.c
│   │   ├── pm_event.c
│   │   ├── pm_ringbuf.c
│   │   ├── pm_gpio.c             参数校验 + 转发到 port
│   │   ├── pm_uart.c             生命周期、TX 流控、RX 投递、事件
│   │   ├── pm_spi.c              传输生命周期、超时中止、完成/错误事件
│   │   ├── pm_i2c.c              写/读/写读事务、超时复位、完成/错误事件
│   │   ├── pm_adc.c              单次转换生命周期、超时停止、完成/错误事件
│   │   ├── pm_pwm.c              频率/占空比校验与转发
│   │   ├── pm_tone.c             软件音调（定时器中断翻转）
│   │   ├── pm_track.c            单路循迹极性判定
│   │   ├── pm_encoder.c          AB x4 解码、计数、可选按键
│   │   ├── pm_relay.c            单路继电器开关与失效安全
│   │   ├── pm_motor.c            DRV8833 方向/滑行/刹车状态机
│   │   └── pm_servo.c            角度↔脉宽换算，组合 pm_pwm
│   ├── common/                   与硬件无关的纯逻辑（任何层可依赖）
│   │   ├── pm_pwm_math.c         时基计算、自检、CCR 换算
│   │   └── pm_timer.c            定时器占用表与冲突仲裁
│   ├── osal/
│   │   └── pm_osal_port_cm3.c    Cortex-M3 实现（PRIMASK + SysTick 计数）
│   ├── port/stm32f1/             STM32F1 后端（唯一允许厂商类型的地方）
│   │   ├── pm_port_stm32f1.h     内部头：GPIO/SPI/I2C/ADC/PWM/tone/track/encoder 映射表、上下文
│   │   └── pm_port_stm32f1.c     HAL 实现、UART/SPI/I2C/ADC/PWM/tone/track/encoder ISR、pm_port_get()
│   ├── port/mock/                预留给第二阶段的 mock 目录
│   └── app/
│       └── main.c                示例：LED 闪烁 + 串口回显 + 心跳
├── platform/stm32f103/           板级与芯片级文件
│   ├── board_f103c8.h/.c         逻辑引脚表、UART/SPI/I2C/ADC/PWM 编号
│   ├── pm_platform.h/.c          平台启动与 SysTick
│   ├── stm32f1xx_hal_conf.h      HAL 模块开关
│   ├── stm32f103c8.ld            自行编写的链接脚本（64K/20K）
│   └── startup_stm32f103xb.s     启动文件（来自 CMSIS 设备包）
├── tests/host/                   主机测试
│   ├── test_main.c               681 项断言
│   ├── pm_port_mock.c            内存后端（GPIO/UART/SPI/I2C/ADC/PWM）
│   ├── pm_osal_host.c            可控时间
│   └── CMakeLists.txt            独立构建（MSVC/GCC 均可）
├── third_party/
│   ├── cmsis/                    CMSIS Core + STM32F1 设备头
│   └── stm32f1xx_hal/            ST HAL 子集（仅编译用到的模块）
├── cmake/arm-gcc-toolchain.cmake
├── CMakeLists.txt                ARM 固件构建
└── README.md
```

## 2. 每个目录的职责边界

| 目录 | 职责 | 不负责 |
| --- | --- | --- |
| `include/pm/` | 对外承诺的行为与类型 | 任何实现细节 |
| `src/core/` | 状态机、缓冲、事件、参数校验 | 寄存器、HAL、引脚 |
| `src/common/` | 任何层可复用的纯逻辑（时基数学、定时器仲裁） | 寄存器、HAL、组件状态 |
| `src/osal/` | 时间与临界区 | 业务时间语义 |
| `src/port/stm32f1/` | 把 ops 映射到 HAL；ISR 搬运 | 协议解析、业务判断 |
| `platform/` | 板级引脚表、时钟启动、链接脚本、启动文件 | 外设行为 |
| `tests/host/` | 证明 core 可脱离硬件运行 | 替代上板验证 |
| `third_party/` | 原样随仓库分发的依赖 | 修改厂商代码 |

## 3. 依赖关系

```
main.c ──► pm_gpio.h / pm_uart.h / pm_spi.h / pm_i2c.h / pm_adc.h / pm_pwm.h
           / pm_tone.h / pm_track.h / pm_encoder.h / pm_relay.h / pm_motor.h / pm_servo.h
           / pm_platform.h / board_f103c8.h
              │
              ▼
          pm_core (src/core) ──► pm_common (src/common)
              │  pm_port_get()
              ▼
      pm_port_stm32f1 ──► HAL ──► CMSIS ──► 寄存器
              │
              └──► board_f103c8.c（引脚表）
```

- `src/core` 编译时**看不到** `stm32f1xx.h`（CMake 的 include 路径里没有它）；
- `src/core` 可以依赖 `src/common`（时基数学、定时器仲裁），`src/common` 不依赖任何层；
- `src/port` 编译时看不到 APP；
- `platform/` 同时被 port 与可执行目标引用，但只提供表与启动代码。

这条链路可以用编译失败来验证：把 `stm32f1xx_hal` 的 include 路径从 `pm_core` 移除后，`pm_core` 仍应编译通过。

## 4. 一次 UART 收发的完整时序

**发送 5 个字节**（`pm_uart_write(&u, "hello", 5, &n, 0)`）：

1. `pm_core`：校验参数与 state；
2. 临界区：`pm_rb_write` 把 5 字节写入 TX 环，`uart_kick_tx_locked` 取一段连续数据调用 `ops->start_tx`，记录 `tx_inflight = 5`；
3. 临界区退出；
4. port：`HAL_UART_Transmit_IT` 启动，返回 `PM_OK`；
5. 硬件发完一字节触发 TXE 中断，HAL 继续取下一字节；
6. 最后一字节发完，HAL 调用 `HAL_UART_TxCpltCallback`；
7. port 回调调用 `pm_uart_hw_tx_complete`；
8. `pm_core`（ISR 上下文）：`tx_inflight` 清零，`pm_rb_consume(5)`，`tx_bytes += 5`；TX 环为空则投递 `PM_EVT_UART_TX_DONE`；
9. 主循环 `pm_uart_poll` 取到事件，回调在主循环执行。

**接收 1 个字节**：

1. RXNE 中断 → `HAL_UART_IRQHandler` → `HAL_UART_RxCpltCallback`；
2. port 调 `pm_uart_hw_rx_byte(u, byte)`；
3. `pm_core`（ISR 上下文）：`pm_rb_write` 写入 RX 环，`rx_bytes++`，投递 `PM_EVT_UART_RX_DATA`（已存在则去重）；
4. port 立即重新 `HAL_UART_Receive_IT` 备好下一字节；
5. 主循环 `pm_uart_read` 取数据、`pm_uart_poll` 取事件。

**环满**：`pm_rb_write` 返回 0，`stats.rx_dropped++`，同时投递 `PM_EVT_UART_ERROR`。数据丢失是可观测的。

## 5. 一次 SPI 全双工传输的完整时序（中断路径）

`pm_spi_transfer(&s, tx, rx, 4, &done, 100)`：

1. `pm_core`：校验 state、长度与 `active`；临界区内置 `active = 1`，清 `last_done/last_error`；
2. port：`HAL_SPI_TransmitReceive_IT` 启动，SPI 外设开始时钟；
3. TXE/RXNE 交替中断：HAL 每字节取一写一，数据直接进出调用方缓冲区；
4. 最后一字节完成 → `SPIx_IRQHandler` → `HAL_SPI_IRQHandler` → `HAL_SPI_TxRxCpltCallback`；
5. port 回调调 `pm_spi_hw_done(s, len)`（ISR 上下文）：写 `last_done`、屏障、清 `active`、`stats` 累加、投递 `PM_EVT_SPI_DONE`；
6. 主循环的等待循环看到 `active == 0`，返回 `PM_OK`，`*done = len`；
7. 主循环 `pm_spi_poll` 取出 `SPI_DONE`，若有回调则在主循环执行。

DMA 路径只把第 3、4 步换掉：`DMA1_ChannelN_IRQHandler` → `HAL_DMA_IRQHandler` →
SPI DMA 完成回调 → 同一个 `HAL_SPI_TxRxCpltCallback`，第 5 步起完全相同。

**超时**：等待循环每 1 ms 检查 `active`，到期调用 port 的 `abort`。port 先读
`RxXferSize - RxXferCount`（中断）或 `RxXferSize - DMA 剩余计数`（DMA），再
`HAL_SPI_Abort`，把已完成的字节数回填给 `*done`。若 ISR 在中止期间先完成，
`active` 已被清零，模型按成功返回，不重复计数。

**错误**：`HAL_SPI_ErrorCallback` → `pm_spi_hw_error()`，同样清 `active`、
记录掩码、投递 `PM_EVT_SPI_ERROR`，同步等待的调用方拿到 `PM_ERR_IO`。

## 6. 一次 I2C 寄存器读的完整时序（中断状态机）

`pm_i2c_write_read(&i, 0x70, &reg, 1, buf, 6, &done, 50)`：

1. `pm_core`：校验 state、地址、长度与 `active`；临界区内置 `active = 1`，清 `last_done/last_error`；
2. port：`HAL_I2C_Master_Seq_Transmit_IT(..., I2C_FIRST_FRAME)` 启动写阶段；
3. 写阶段逐字节由 EV 中断推进；最后一字节后 HAL 调 `HAL_I2C_MasterTxCpltCallback`（ISR）；
4. port 发现还有读长度，在 **ISR 内**调 `HAL_I2C_Master_Seq_Receive_IT(..., I2C_LAST_FRAME)`；
   HAL 产生 repeated START、切到读方向；此时**不**通知模型；
5. 第 6 字节读入 `buf`，STOP 产生，HAL 调 `HAL_I2C_MasterRxCpltCallback`（ISR）；
6. port 调 `pm_i2c_hw_done(i, 1, 6)`：写 `last_done`、屏障、清 `active`、累加统计、投递 `PM_EVT_I2C_DONE`；
7. 主循环等待循环看到 `active == 0`，返回 `PM_OK`，`*done = 7`；
8. 主循环 `pm_i2c_poll` 取事件，回调在主循环执行。

**超时**：每 1 ms 查 `active`；到期调 port 的 `abort`。port 先按
`XferSize - XferCount` 算出当前阶段已移动字节（读阶段再加写长度），再
`DeInit + Init` 复位外设并重新使能中断。若 ISR 在中止期间先完成，模型按完成处理。

**NACK / 错误**：`I2C1_ER_IRQHandler → HAL_I2C_ErrorCallback → pm_i2c_hw_error()`，
清 `active`、记录 `ErrorCode`、投递 `PM_EVT_I2C_ERROR`；等待中的调用方拿 `PM_ERR_IO`。

## 7. 一次 ADC 转换的完整时序（EOC 中断）

`pm_adc_read(&a, &raw, 10)`：

1. `pm_core`：校验 state 与 `active`；临界区内置 `active = 1`、清 `last_error`；
2. port：`HAL_ADC_Start_IT` 启动一次软件触发转换；
3. 转换结束 → `ADC1_2_IRQHandler` → `HAL_ADC_IRQHandler` → `HAL_ADC_ConvCpltCallback`；
4. port 读 `HAL_ADC_GetValue`，调 `pm_adc_hw_done(a, value)`（ISR 上下文）；
5. `pm_core`：写 `last_value`、屏障、清 `active`、`stats.conversions++`、投递 `PM_EVT_ADC_DONE`（`arg` = 原始值）；
6. 主循环等待循环看到 `active == 0`，返回 `PM_OK`，`*raw = last_value`；
7. `pm_adc_poll` 取出事件，回调在主循环执行。

**超时**：每 1 ms 查 `active`；到期调 port 的 `abort`（`HAL_ADC_Stop_IT`）。
若 ISR 在中止期间先完成，模型按完成处理，不重复计数。

**错误**：`HAL_ADC_ErrorCallback → pm_adc_hw_error()`，清 `active`、记录掩码、
投递 `PM_EVT_ADC_ERROR`，等待中的调用方拿 `PM_ERR_IO`。

## 8. 一次 PWM 配置的完整时序

`pm_pwm_init(&p, &cfg)`：

1. `pm_core`：校验频率（1 Hz～1 MHz）与占空比（0～1000‰），清零对象；
2. port：逻辑通道 → `(GPIOx, GPIO_PIN_n, TIMx, TIM_CHANNEL_n, timer_index)`；
3. 若该定时器还没有打开的通道：先按 `tim_clk / freq / 65536` 选 PSC，
   保证 `ARR` 落在 16 位内，再使能定时器时钟并 `HAL_TIM_PWM_Init`；
4. 若已有兄弟通道：要求请求频率与已记录值一致，否则返回 `PM_ERR_BUSY`；
5. GPIO 配为 AF 推挽；`HAL_TIM_PWM_ConfigChannel`（PWM 模式 1，
   `Pulse = (ARR + 1) * permille / 1000`）；
6. `HAL_TIM_PWM_Start` 启动输出；
7. 有效频率 `= tim_clk / ((PSC + 1) * (ARR + 1))`，由 port 回填给模型。

`pm_pwm_set_frequency`：兄弟通道已打开且请求频率不同则返回 `PM_ERR_BUSY`；
否则写 PSC/ARR、置 `EGR.UG` 立即生效，并按各通道保存的千分比重算 CCR。

`pm_pwm_set_duty` 只写 CCR，不改时基。PWM 没有中断与事件队列。

## 9. 移植到另一块 STM32F1 板

只需改 `platform/stm32f103/board_f103c8.c` 与 `.h`：

1. 在 `board_*.h` 里定义逻辑引脚枚举与 UART/SPI/I2C/ADC/PWM 编号；
2. 在 `board_*.c` 里填写 id → `(GPIOx, GPIO_PIN_n)` 的映射表；SPI 的 SCK/MISO/MOSI、I2C 的 SCL/SDA、
   ADC 的 `(port, pin, ADC_Channel)` 与 PWM 的 `(port, pin, TIMx, TIM_CHANNEL_n, timer_index)` 也在这张表里；
3. 若使用不同的串口引脚，修改 `src/port/stm32f1/pm_port_stm32f1.c` 的 `uart_gpio_init()`；
4. PWM 的 `timer_index` 必须与 port 内 `s_pwm_timer[]` 的顺序一致（当前 0 = TIM2，1 = TIM4）；
5. 改链接脚本里的 Flash/RAM 大小（`stm32f103c8.ld`）。

上层 `main.c` 与 `pm_core` 不需要改。

本板的 SPI 引脚与板载器件复用（SPI1 与电位器/RGB，SPI2 与 KEY2/循迹/编码器按键），
`board_f103c8.h` 已注明；真正接 SPI 从机前必须先腾出对应引脚。
SPI 引脚方向：SCK/MOSI 为 `AF_PP`，MISO 在主机模式下必须让出线路，配为
`GPIO_MODE_INPUT` + 上拉（RM0008 允许浮空或上拉；配成 `AF_PP` 会顶住从机输出）。
I2C 只暴露 I2C1（PB6/PB7），板上挂 OLED（0x7A）与 AHT20（0x70）两个**板载器件**，
外部 I2C 排针与它们共用总线；I2C2 的 PB10/PB11 是 USART3/BLE。

## 10. 移植到另一种芯片

1. 新建 `src/port/<chip>/`，实现 `pm_gpio_ops_t`、`pm_uart_ops_t`、`pm_spi_ops_t`、`pm_i2c_ops_t`、
   `pm_adc_ops_t`、`pm_pwm_ops_t` 和 `pm_port_get()`；
2. 新建 `src/osal/pm_osal_port_<arch>.c`（用该芯片的关中断原语）；
3. 新建 `platform/<chip>/` 放启动文件与链接脚本；
4. `pm_core` 与 `include/pm` 不动；
5. 主机测试仍然跑 mock 版本，用于回归。

## 11. 编译期校验建议

- `pm_core` 的 include 路径里不放 `third_party/`，从构建层面阻断厂商头泄漏；
- 用 `-Werror=implicit-function-declaration` 防止漏 include；
- 链接后用 `nm` 检查 `malloc/free/_sbrk` 不存在；
- 用 `size`/`--print-memory-usage` 记录每次改动的 Flash/RAM 增量。
