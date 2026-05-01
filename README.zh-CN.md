# Viper

[English](README.md)

Viper 是一个实验性的 Lua 模块，可以在运行时把一个小型 C-like 函数编译成 Xtensa 原生机器码。它主要面向 ESP32 系列芯片上的热点整数循环，尤其是 ESP32-S3 + ESP-IDF / Arduino / NodeMCU。

> 当前状态：实验性。当前代码只在 ESP32-S3 上测试过。经典 ESP32 理论上可能可用，但尚未验证；ESP8266 暂不支持。

## 功能

- `viper.compile_c(src[, opts])`：编译一个受限制的 C-like 函数。
- `viper.buf(size)`：分配可传给指针参数的 buffer。
- native 后端支持整数标量、整数指针、指针索引、循环和条件分支。
- 默认禁止把整数当作裸指针传入；只有显式传 `{ unsafe = true }` 才允许。
- 在已测试的 ESP32-S3 环境中，写得好的 C-like 热循环通常可以比 Lua 快约 10-20 倍。这个提升很依赖代码写法，因为 Viper 只做有限优化，不会像 GCC 那样自动优化复杂表达式。

## 兼容性

| 目标 | 状态 |
| --- | --- |
| ESP32-S3 + ESP-IDF | 已测试，可用 |
| ESP32-S3 + NodeMCU ESP32 分支 | 已用外部组件方式测试 |
| ESP32-S3 + Arduino | 可用，但需要重新编译 Arduino-ESP32 core，并修改底层 ESP-IDF 配置 |
| ESP32 + ESP-IDF | 理论上可能可用，尚未验证 |
| ESP32 + Arduino | 理论上可能可用，但需要重新编译 Arduino-ESP32 core，尚未验证 |
| ESP8266 | 暂不支持 |

Viper 会在运行时生成机器码，并申请可执行内部 RAM。ESP32-S3 构建中必须允许可执行 RAM：

```text
CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=n
```

Arduino 默认安装通常不能直接修改这些底层 `sdkconfig` 选项，所以需要重新编译 Arduino-ESP32 core/底层内核配置后再使用 native code。

## 快速示例

```lua
local sum = viper.compile_c([[
int32_t sum(uint8_t *p, int32_t n) {
  int32_t s = 0;
  int32_t i;
  for (i = 0; i < n; i = i + 1) {
    s = s + p[i];
  }
  return s;
}
]])

local b = viper.buf(4)
b:set8(0, 10)
b:set8(1, 20)
b:set8(2, 30)
b:set8(3, 40)

print(sum(b, 4)) -- 100
```

## 仓库结构

```text
src/              parser、IR、Xtensa assembler、native 后端
ports/nodemcu/    NodeMCU ESP32 外部组件
ports/arduino/    Arduino LuaWrapper 绑定
docs/             API、C-like 子集、编译说明
examples/         Lua 示例和跑分脚本
firmware/         预编译测试固件
```

## NodeMCU

NodeMCU ESP32/S3 推荐把本仓库作为 ESP-IDF 外部组件接入：

```powershell
cd C:\Users\wzh\Documents\nodemcu-firmware
. .\sdk\esp32-esp-idf\export.ps1
$env:EXTRA_COMPONENT_DIRS='C:/Users/wzh/Documents/viper/ports/nodemcu'
idf.py set-target esp32s3
idf.py build
```

完整的 Windows 编译、烧录、串口验证流程见 [README.nodemcu.md](README.nodemcu.md)。

## 预编译 ESP32-S3 固件

仓库里包含一份已测试的 ESP32-S3 NodeMCU 固件：[firmware/esp32s3](firmware/esp32s3)。它已经启用 Viper，并把 Lua 控制台配置到 USB Serial/JTAG。烧录命令见 [firmware/esp32s3/README.md](firmware/esp32s3/README.md)。

## Arduino

Arduino 适配层可在 ESP32 系列目标上使用，但需要重新编译 Arduino-ESP32 core，并在底层 ESP-IDF 配置中允许可执行 RAM。普通 Arduino 包管理器安装通常不能直接修改这些 `sdkconfig` 选项，所以“能编译”不一定等于“能运行 native code”。

## C-like 子集

Viper 不是完整 C 编译器，只支持单函数小子集：

- 每次源码只能包含一个函数；
- 支持 `int32_t`、`uint32_t`、`int16_t`、`uint16_t`、`int8_t`、`uint8_t`、`bool`、`void` 和这些标量的指针；
- 支持变量声明、赋值、`if/else`、`for`、`return`、整数算术、位运算、比较和 `p[i]`。

暂不支持：`#include`、宏、`struct`、`union`、`enum`、`typedef`、局部数组、函数调用、`sizeof`、cast、`switch`、`while`、`break`、`continue`、`goto`、`*p`、`&x`、`.` 和 `->`。

## 性能写法

想达到 ESP32-S3 上相对 Lua 约 10-20 倍的提升，C-like 代码要尽量朴素：

- 一次 native 调用尽量处理一批数据；
- 热路径局部变量优先用 `int32_t` / `uint32_t`；
- 内层循环少用除法和取模；
- 指针索引保持简单；
- 复杂表达式拆成几个显式步骤。

详细建议见 [docs/clike.md](docs/clike.md)。

## 文档

- [API](docs/api.md)
- [C-like 子集](docs/c-like-subset.md)
- [C-like 性能写法](docs/clike.md)
- [NodeMCU 模块编译](README.nodemcu.md)
- [NodeMCU ESP32 说明](docs/build-nodemcu-esp32.md)
- [NodeMCU ESP8266 说明](docs/build-nodemcu-esp8266.md)
- [示例](docs/examples.md)

## 开源协议

Viper 使用 GNU General Public License v3.0 only 发布。详见 [LICENSE](LICENSE)。
