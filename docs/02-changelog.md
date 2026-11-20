# 变更记录

只记录跨版本的结构性变动，以及"事后容易被误判为丢文件"的操作。接口细节以
[docs/00-contract.md](00-contract.md) 为准。

## v0.7.0（2026-11-20）

v0.6 之后的改动，已打 annotated tag `v0.7.0`。

### 删除 PB9 的 PWM 蜂鸣器通道、音调互斥改为双侧检查（2026-11-20）

PB9 那条 PWM 后端在本板没有任何调用者，留着会让"两个后端互斥、两侧都检查"的说法继续与
实现不符。本轮把保留项删掉，并把互斥补成真正对称：

- `board_pwm_id_t` 删除 `BOARD_PWM_BUZZER`（原 PB9 / TIM4_CH4），`board_f103c8.c` 对应表项
  一并删除；`BOARD_PWM_COUNT` 4 → 3，端口 `PM_F1_PWM_COUNT` 同步 4 → 3。
  旧逻辑编号 `hw_id = 3` 现在越界，模型侧（`pwm_count = 3`）直接返回 `PM_ERR_RANGE`，
  端口 `c->count` 是第二道检查。
- **音调互斥补成双侧**：原先只有 `f1_tone_open()` 查 `pwm_uses_gpio()`，PWM 侧没有再查音调，
  "先开音调、再开同引脚 PWM"会夺脚。新增 `tone_uses_gpio()`，由 `f1_pwm_open()` 在通道
  与引脚参数检查之后调用，返回 `PM_ERR_CONFLICT`；与 track/SPI 的互斥同形，不依赖
  调用顺序。§17.3/§17.4 按实际改述：本板 PB9 只有 TIM3 软件音调一个后端，
  只有板级表另行加回 PWM 通道时才回到这条互斥。
- §14.4/§14.5/§14.6 与 README 同步：TIM4 本板只开 PB8（舵机）一路，PB9 没有 PWM 通道，
  蜂鸣器不占 TIM4，二者可同时运行。
- 主机单测：`MOCK_PWM_COUNT` 同步 4 → 3；新增 `hw_id = 3` → `PM_ERR_RANGE` 断言、
  tone 先开则同引脚 PWM 返回 `PM_ERR_CONFLICT`（及反向）断言；`test_motor()` 的第二实例
  因通道只剩 3 路改为拆掉首实例后串行使用。断言总数仍为 **689**（新增 7 条、电机用例
  重构净变化 0）。分层 `0 ERROR / 18 WARN` 不变；固件 FLASH 28536 B（43.54%）、
  RAM 3376 B（16.48%），比上一行 +100 B（交叉相乘 +44 B、删通道补检查 +56 B）/−8 B（少一个通道状态槽）。

### 时基口径、定时器编号与蜂鸣器归属对齐板端（2026-11-20）

跨仓复核后按板端事实改模型侧，行为只动时基判据一处，其余为注释与文档：

- `pm_pwm_timing_matches()` 改为**交叉相乘**比较，不再先把实际频率取整：取整会让合法组合
  在自己的容差边界上判假（449.9 Hz 取整成 449 对 450 Hz）。板端自检就是这个口径。
  补 450 Hz / 500 Hz 各一对边界用例（四个组合四舍五入后都是同一频率，0‰ 判假、1‰ 判真）；
  断言总数 681 → 689；固件 FLASH 28436 → 28492 B（43.48%），RAM 3384 B 不变。
- `board_timer_id_t` 取值改为**定时器序号**（2/3/4 = TIM2/3/4，与参考手册和板端日志一致），
  端口用 `timer_index - PM_F1_TIMER_FIRST` 索引稠密数组；`pm_timer` 探测与主机用例同步。
  §20.7 更新为板端现状：检查已抽到 `pm_timer_probe()`、`MAPR` 查 `TIM2_REMAP` 全 2 位字段、
  覆盖 TIM2/TIM3/TIM4。
- **蜂鸣器归属更正**：本板 PB9 由 TIM3 更新中断做软件音调，不占 TIM4 通道，因此
  "舵机 50 Hz 与蜂鸣器 2 kHz 不能同时在 TIM4"不成立。§14.4–§14.6、§17.3/§17.4、§20.6
  与 README 统一为：TIM4 只有舵机一路；`BOARD_PWM_BUZZER`（TIM4_CH4）标为本板未接线、
  仅为端口保留，只有另行接线时才回到同组同频约束。（该保留项已在上一条中删除。）
- `include/pm/pm_motor.h` 的 `pm_motor_init` 注释补正：`invert = UNSET` 等同 OFF，
  不再写"UNSET 返回 `PM_ERR_PARAM`"（上一条已改实现，本条只改注释）。

### 模型 API 接受 `invert = UNSET`（2026-11-20）

- `pm_motor_init()` 不再把 `PM_MOTOR_INVERT_UNSET` 当非法值：语义等同 `OFF`（不交换），
  零初始化配置可直接使用，默认行为不变；`UNSET/OFF/ON` 之外的取值仍返回 `PM_ERR_PARAM`。
- `docs/00-contract.md` §23.3 与 `include/pm/pm_motor.h` 注释同步；
  主机单测覆盖 UNSET、OFF、ON 三种取值与非法值。

### 契约对齐 0018（2026-11-20，仅文档，未改代码）

按协作区决策记录 0018（外设契约七条对齐定案）改述契约，代码与测试未动：

- §14.5：时基改为"由端口决定"，`pm_pwm_timing_matches()`（10‰）是必要复核，
  `timing_compute()` 降为可选工具；删去把 `PSC 21 / ARR 65453` 当标准的表述，
  50 Hz 行改用板端实测的 `71 / 19999`；
- §14.1/§14.3：写明对外协议 `%`、模型内部 `‰`，换算（`pct × 10` / `permille ÷ 10`）
  归模型侧适配层；
- §19.2/§19.4：AB 位序按板端 `a | (b << 1)` 与模型 `A << 1 | B` 分写，适配层负责重排；
  `invert` 标为可选；补 encoder `EXTI9_5` 的模块守卫（优先级保留 4）；
- §20.5/§20.7：补"停用模块必须连中断一起撤掉"的反例（sonar `EXTI10`）；
  探测判据写死 `CEN` 与 `MAPR` 两条，运行期占用表标注延后；
- §21.2：补舵机 50 Hz 时基、电机 CCR 落点、编码器计数三行板端证据；
- §23.3/§23.4/§23.6：`FORWARD = IN1（板端 CH1）`、`invert` 非板端必填、
  `stop(BRAKE)` 标注"板端未实现、未实测"并登记技术债。

未新增 `% ↔ ‰` 适配文件：换算只有乘除以 10，且本仓当前没有板端 API 的调用方；
规则已写进 §14.1，等真实适配层出现时再落地并补单测。

## 检查器升级 3.2.0 → 3.3.1（2026-11-19）

- 覆盖 `tools/check_layering.py`（上游 `3c507b7`，blob `9740b0a`），只改文件头
  `source-commit` 与 `SOURCE_COMMIT` 两处，其余与上游逐字一致；
- `tools/CHECKER.lock` 同步三处（版本、提交、blob），
  `python tools/check_layering.py --version --lock tools/CHECKER.lock` → `[LOCK] 一致`，退出码 0；
- **基线对比**：同参扫描新旧的输出逐行相同，均为 0 ERROR / 18 WARN，**无新增命中**，
  因此本期不建存量清单；
- 首轮全部保持 WARN 级，未把任何规则调成 ERROR；调级需单独立项。

## v0.6（2026-11-19，提交 `8f8b9e9` … `4e0ae8d`，本文档收尾提交另计）

### 新增

- `pm_timer` 外部占用探测：端口通过 `pm_timer_set_probe()` 安装探测，模型在**零 live
  claim** 时才询问"该定时器是否已被非模型代码启动/重配"，忙则返回 `PM_ERR_CONFLICT`，
  表与寄存器都不动（契约 §20.7）；解决板端 `pm_motor_stm32f1.c` 只查 `TIM2->CR1.CEN`
  的裸占用对模型不可见的问题；
- `pm_encoder` 可选按键：`pm_encoder_cfg_t.has_button` 与可选 `read_button` 端口钩子；
  未配置时 `init` 仍成功、`pm_encoder_has_button()` 为 false、`pm_encoder_button()`
  返回 `PM_ERR_DISABLED` 且 `*pressed = false`（契约 §19.6）；
- `pm_servo` 新模型：给定角度范围与脉宽范围（默认 SG90 0–180° ↔ 500–2500 µs），
  做角度→脉宽换算并校验越界；单独成章（契约第 24 章），不并入 `pm_pwm`；
- `pm_i2c` 可选错误分类与计数：默认关闭保持 `PM_ERR_IO` 与零计数，开启后
  AF → `PM_ERR_NACK`，BERR/ARLO/OVR → `PM_ERR_BUS`，截止时间到期 → `PM_ERR_TIMEOUT`，
  并提供分项计数（契约 §12.7）；
- 错误码：`PM_ERR_NACK`、`PM_ERR_BUS`，`pm_err_str` 同步。

### 纠正

- **PB15 引脚身份**：`HARDWARE.md` 的实测接线与板端 `pm_encoder_button()`
  （`(GPIOB->IDR >> 15) & 1`）一致认定 PB15 是 EC11 按键；模型库此前称其为 SPI2_MOSI
  并据此不启用按键，属模型库侧错误。本轮只改模型库侧（`platform/stm32f103/board_f103c8.c`、
  `board_f103c8.h`、`include/pm/pm_encoder.h`、`include/pm/pm_port_stm32f1.h`、
  契约 §19.1/§19.4/§19.6），板端工程与 `HARDWARE.md` 未动；
- 电气含义：PB15 是**复用互斥**，不是两个身份同时成立——同一时刻只能作 SPI2_MOSI
  或按键输入。模型库引脚表按"按键 = PB15，与 `BOARD_SPI_2` 互斥"记录，不再写成"不属于
  本板编码器"。

### 验证基线

- 主机单测：`676 checks, 0 failure(s)`；
- 固件构建：FLASH 28436 B（43.39%），RAM 3384 B（16.52%）；
- 分层：`0 ERROR / 18 WARN`（WARN 最后一条行号随端口文件改动变为 2428，README 已同步）。

## v0.5（2026-11-19，提交 `76e4863`、`9b393e6`）

### 新增

- 模型：`pm_tone`（软件音调，TIM3 更新中断）、`pm_track`（单路循迹）、
  `pm_encoder`（AB x4 解码）；
- 定时器仲裁：`pm_timer`（运行期占用表，跨模块冲突返回 `PM_ERR_CONFLICT`）；
- 错误码：`PM_ERR_CONFLICT`、`PM_ERR_DISABLED`；
- 纯时基数学：`pm_pwm_math`（PSC/ARR 计算、时基自检、CCR 换算）。

### 分层变动

- 新增 `common` 层的落盘位置 `src/common/`，契约头 `include/pm/pm_pwm_math.h`、
  `include/pm/pm_timer.h`；
- 原因：端口在写寄存器前必须直调这两类原语，原先放在 `src/core/`（components）会让
  bsp 出现反向依赖，检查器 3.2.0 报了 6 条 ERROR；
- 修复后：`0 ERROR / 18 WARN`。

### 文件移动留痕

| 原路径 | 新路径 | 性质 |
| --- | --- | --- |
| `src/core/pm_timer.c` | `src/common/pm_timer.c` | **移动，不是删除** |

判断依据（可逐条复核）：

1. 该文件是本轮新建，从未进入任何提交；
2. `git grep -n "pm_timer_" cf21986 -- src include` 无输出（退出码 1）——`cf21986` 是 v0.4
   的末提交，说明**移动前的已提交树**里不存在该符号的任何声明或调用；
3. 移动发生在首次提交之前，`76e4863` 中该路径以
   `create mode 100644 src/common/pm_timer.c` 出现；
4. 因此 `git log --follow -- src/common/pm_timer.c` 只会显示一次新增、没有可追的旧路径
   ——这是"首次提交即在新位置"，不是丢文件。

复核命令：

```bash
git grep -n "pm_timer_" cf21986 -- src include        # 期望：无输出，退出码 1
git ls-tree -r HEAD --name-only | grep pm_timer       # 期望：仅 include/pm/pm_timer.h 与 src/common/pm_timer.c
```

### 验证基线

- 主机单测：`458 checks, 0 failure(s)`；
- 固件构建：FLASH 28048 B（42.80%），RAM 3376 B（16.48%）；
- 四层架构检查：`python tools/check_layering.py . --roles .arch-roles` → 0 ERROR / 18 WARN。

### 经验

- `-Werror` 系列开关是"漏 include / 隐式声明"这类错误的唯一防线。v0.5 的
  `pm_port_stm32f1.c` 用了 `memset` 却漏 `#include <string.h>`，正是被
  `implicit-function-declaration` 挡下的；不得为"编过"关闭或降级这类告警。
