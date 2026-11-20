# 参与本仓开发

本仓是 STM32F1 外设模型库：`include/pm/` 是对外契约，`src/core/` 与硬件无关，
`src/common/` 是任何层可复用的纯逻辑，`src/port/` 与 `platform/` 是芯片/板级实现。
改代码前先读 [docs/00-contract.md](docs/00-contract.md)，目录与依赖见
[docs/01-layering.md](docs/01-layering.md)。

## 提交前必须过的三道门禁

三条都过才算完成；任一失败不带病提交。

```powershell
# 1. 主机单测（689 项断言，0 失败）
cmake -S tests/host -B build-host
cmake --build build-host --config Debug
ctest --test-dir build-host -C Debug --output-on-failure

# 2. 固件构建（需要 arm-none-eabi-gcc 14.x）
cmake -S . -B build-arm -G Ninja `
      -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/arm-gcc-toolchain.cmake"
cmake --build build-arm

# 3. 四层架构检查（0 ERROR；WARN 允许但不许新增类别）
python tools/check_layering.py . --roles .arch-roles
```

新增断言后同步 README 与文档里的断言数；体积数字用实测值，不用推算。

## 代码约定

- **分层**：APP → components(`src/core`) → common(`src/common`) → bsp(`src/port`、`platform`)。
  `src/core` 不得出现 HAL/CMSIS 类型与引脚号；`include/pm/` 里不得出现任何厂商类型；
- **命名**：公开符号一律 `pm_` 前缀；ISR 入口用 `pm_<模块>_hw_<事件>()`，
  只投递事件，不做业务逻辑；
- **内存**：静态分配，库内不调用 `malloc`；
- **错误**：统一返回 `pm_err_t` 负值，不返回布尔；参数错 `PM_ERR_PARAM`，未初始化 `PM_ERR_STATE`；
- **告警即错误**：MSVC `/W4`、GCC `-Wall -Wextra -Wshadow -Werror`，禁止为"编过"关闭或降级；
- **第三方**：`third_party/` 只读，不加改动；`tools/check_layering.py` 不修改判定规则，
  升级只换文件并同步 `tools/CHECKER.lock`。

## 文档同步

- 改公共 API 或语义 → 更新 `docs/00-contract.md` 对应章节；
- 结构性变动、文件移动 → 在 `docs/02-changelog.md` 顶部的未发布段落加条目（发布时该段落
  改名为版本号，如 `v0.7.0`），**不改已发布版本的段落**；
- 新增/删除模块 → 同步 README 的模块清单、模块数与目录树（`docs/01-layering.md`）。

## 提交信息

`type(scope): 中文摘要`，例如 `fix(motor): UNSET 按不交换处理`、
`docs(layering): 补全目录树`。一次提交只做一件事，不把无关重构混进来。

## 行尾

`.gitattributes` 已统一为 LF（提交与检出），不要提交 CRLF 改动；
若本地编辑器改写行尾，先 `git diff --stat` 确认没有整文件翻新再提交。
