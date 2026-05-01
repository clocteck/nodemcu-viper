# 去掉 std::string 并移植到 NodeMCU 编译测试

这份文档说明如何把当前 Viper 模块从“ESP-IDF/C++ 实验实现”逐步改成更适合 NodeMCU 的模块。目标是先删除 `std::string`，再放进 NodeMCU ESP32/S3 固件中编译和运行最小测试。

当前建议顺序：

```text
1. 先在当前仓库删除 std::string
2. 用 ESP-IDF / NodeMCU ESP32-S3 编译验证
3. 再考虑 NodeMCU upstream 风格注册
4. 最后才考虑 ESP8266
```

不要一开始就做 ESP8266。当前 native 后端、heap API、可执行内存处理都更接近 ESP-IDF ESP32-S3。

## 1. 找出 std::string 使用点

在仓库根目录执行：

```sh
rg -n "std::string|#include <string>|to_string" src ports
```

当前主要位置：

- `src/viper_internal.h`
- `src/viper_cparser.cpp`
- `src/viper_native.cpp`
- `ports/nodemcu/viper.cpp`
- `ports/arduino/lua_viper.cpp`

其中 `ports/arduino/` 可以最后处理，因为 Arduino 当前只是 compile-only 集成实验。真正要先跑通的是 `src/` 和 `ports/nodemcu/`。

## 2. 增加固定长度字符串结构

推荐在 `src/viper_internal.h` 里增加两个小结构。

```cpp
struct ViperName
{
    char text[32];
};

struct ViperError
{
    char text[160];
};
```

再加几个 helper：

```cpp
void viper_name_set(ViperName &dst, const char *src, size_t len);
bool viper_name_eq(const ViperName &name, const char *src, size_t len);
void viper_error_set(ViperError &err, const char *msg);
void viper_error_set_at(ViperError &err, const char *msg, size_t pos);
```

注意：

- 名字太长时直接报错，例如 `viper.compile_c: identifier too long`。
- 错误信息可以截断，但变量名不建议静默截断。
- `std::to_string()` 全部改成 `snprintf()`。

## 3. 替换 Token 文本

当前 parser token 类似：

```cpp
std::string text;
```

建议改成：

```cpp
ViperName text;
uint8_t text_len;
```

对于操作符 token，也可以不用保存字符串，直接保存 `TokenKind` 或一个小 enum。例如 `+=`、`==`、`&&` 这种操作符最好在 lexer 阶段就变成明确的 token kind，后面不要再用字符串比较。

改法优先级：

```text
关键字判断: std::string -> char* + len
类型解析: std::string -> char* + len
变量名查找: std::string -> ViperName
操作符判断: std::string -> TokenKind/enum
```

## 4. 替换变量名和函数名

当前结构里有：

```cpp
std::string name;
```

建议改成：

```cpp
ViperName name;
uint8_t name_len;
```

临时变量不需要真的保存字符串名。现在类似 `$ct123` 的名字主要是为了调试和避免重复，移植时可以改成：

```cpp
bool is_named;
```

或保留空名字，只对用户声明的参数/局部变量做重复检查。

也就是说：

- 参数名、局部变量名：必须保存，用于查找和重复检查。
- 常量、临时值：不需要字符串名。
- 函数名：可以保存，也可以只检查存在，不参与 native 执行。

## 5. 替换错误传递接口

当前接口大概是：

```cpp
bool parse_viper_c(const char *src, bool unsafe_ptr, bool bounds_check,
                   CompiledFunction &fn, std::string &err);

bool compile_native(CompiledFunction &fn, std::string &err);
```

建议改成：

```cpp
bool parse_viper_c(const char *src, bool unsafe_ptr, bool bounds_check,
                   CompiledFunction &fn, char *err, size_t err_len);

bool compile_native(CompiledFunction &fn, char *err, size_t err_len);
```

调用侧从：

```cpp
std::string err;
if (!parse_viper_c(src, unsafe_ptr, bounds_check, *fn, err)) {
    return luaL_error(L, "%s", err.c_str());
}
```

改成：

```cpp
char err[160];
err[0] = '\0';
if (!parse_viper_c(src, unsafe_ptr, bounds_check, *fn, err, sizeof(err))) {
    return luaL_error(L, "%s", err[0] ? err : "viper.compile_c: compile error");
}
```

`viper_native.cpp` 里的 `m_error` 也改成固定数组：

```cpp
char m_error[160];
```

`fail(err, msg)` 改成往外部 `char *err` 里写入，而不是赋值给 `std::string`。

## 6. 清理 include

改完以后执行：

```sh
rg -n "std::string|#include <string>|to_string" src ports/nodemcu
```

期望结果为空。

如果 `src/` 和 `ports/nodemcu/` 已经没有 `std::string`，再考虑是否同步修改 `ports/arduino/`。

## 7. 放入 NodeMCU ESP32/S3

推荐先用外部组件方式，不要直接复制进 NodeMCU 源码树：

```sh
git clone --recurse-submodules -b dev-esp32 https://github.com/nodemcu/nodemcu-firmware.git
cd nodemcu-firmware
export EXTRA_COMPONENT_DIRS="/absolute/path/to/viper/ports/nodemcu"
make menuconfig
make
```

如果你的 NodeMCU 分支不支持 `EXTRA_COMPONENT_DIRS`，再使用复制方式：

```sh
cp -r /absolute/path/to/viper/ports/nodemcu components/viper
cp -r /absolute/path/to/viper/src components/viper/src
make menuconfig
make
```

复制方式下需要同步调整 `components/viper/CMakeLists.txt` 里的源码路径。

## 8. menuconfig 重点检查

Viper 需要生成并执行 native code。ESP32-S3 下至少要重点检查：

- 关闭会阻止 executable heap 的 memory protection 选项。
- 如果使用大 buffer，启用并确认 PSRAM 可用。
- 保留足够 internal RAM 给 native code 和调用栈。

如果运行时报：

```text
viper native: executable memory allocation failed; disable ESP_SYSTEM_MEMPROT_FEATURE
```

优先在 `menuconfig` 里搜索 memory protection / memprot，并关闭对应保护选项。

## 9. 编译测试

先只编译：

```sh
make -j
```

如果报 C++ 标准库、`std::string` 或 `to_string`，说明还有遗漏：

```sh
rg -n "std::string|#include <string>|to_string" .
```

如果报 `lua.h` 或 `module.h` 找不到，说明模块没有在 NodeMCU 固件树里编译，或者 include path 没有接上。

## 10. 运行最小 Lua 测试

刷机后先测试模块是否存在：

```lua
print(viper)
```

再测试 buffer：

```lua
local b = viper.buf(4)
b:set8(0, 10)
b:set8(1, 20)
print(b:get8(0), b:get8(1), b:len())
```

最后测试 native 编译和调用：

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
print(sum(b, 4))
```

期望输出：

```text
100
```

## 11. 建议提交顺序

建议拆成小提交，方便定位问题：

```text
commit 1: add fixed error/name helpers
commit 2: remove std::string from parser tokens and names
commit 3: remove std::string from native compiler errors
commit 4: remove std::string from NodeMCU binding
commit 5: build in NodeMCU ESP32-S3 and add test notes
```

每个提交后都跑：

```sh
rg -n "std::string|#include <string>|to_string" src ports/nodemcu
```

最终目标是 `src/` 和 `ports/nodemcu/` 完全没有 `std::string`。

