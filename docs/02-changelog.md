# 变更记录

只记录跨版本的结构性变动，以及"事后容易被误判为丢文件"的操作。接口细节以
[docs/00-contract.md](00-contract.md) 为准。

## 检查器升级 3.2.0 → 3.3.1（2026-11-19）

- 覆盖 `tools/check_layering.py`（上游 `3c507b7`，blob `9740b0a`），只改文件头
  `source-commit` 与 `SOURCE_COMMIT` 两处，其余与上游逐字一致；
- `tools/CHECKER.lock` 同步三处（版本、提交、blob），
  `python tools/check_layering.py --version --lock tools/CHECKER.lock` → `[LOCK] 一致`，退出码 0；
- **基线对比**：同参扫描新旧的输出逐行相同，均为 0 ERROR / 18 WARN，**无新增命中**，
  因此本期不建存量清单；
- 首轮全部保持 WARN 级，未把任何规则调成 ERROR；调级需单独立项。

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
