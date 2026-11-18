# 第三方代码说明

本目录只包含编译所需的最小集合，文件内容未做修改。

| 目录 | 来源 | 版本 | 许可 |
| --- | --- | --- | --- |
| `cmsis/include` | CMSIS Core（ARM） | 随 STM32CubeF1 1.8.0 分发 | Apache License 2.0 |
| `cmsis/device` | CMSIS Device STM32F1xx | 同上 | Apache License 2.0 |
| `stm32f1xx_hal` | STM32F1 HAL Driver | 1.1.10（CubeF1 1.8.0） | BSD-3-Clause |

完整许可矩阵见 [ST_LICENSE.md](ST_LICENSE.md)。各组件的原始许可文本随源码保留：
`stm32f1xx_hal/LICENSE.txt`（BSD-3-Clause）、`cmsis/LICENSE.txt` 与
`cmsis/device/LICENSE.txt`（Apache-2.0）。

未包含的内容：

- ST 的链接脚本模板（其条款禁止再分发，本工程使用自行编写的 `platform/stm32f103/stm32f103c8.ld`）；
- 与本工程无关的 ST 组件：USB Device/Host、Ethernet、BSP、FreeRTOS、FatFS、LwIP、STemWin
  与各类示例工程。只保留 HAL 驱动与 CMSIS 头文件/启动所需的最小集合。

每个源文件都保留了 ST/ARM 原始的版权与许可头；完整许可矩阵见
[ST_LICENSE.md](ST_LICENSE.md)。替换或升级 HAL 时，请同步更新上表与
`src/port/stm32f1/` 的适配代码。
