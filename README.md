# Viper

[中文说明](README.zh-CN.md)

Viper is an experimental Lua module that compiles one small C-like function into
native Xtensa code at runtime. It is intended for hot integer loops on
ESP32-class chips, especially ESP32-S3 with ESP-IDF, Arduino, or NodeMCU.

> Status: experimental. The current code has been tested on ESP32-S3 only.
> Classic ESP32 may work because it is also an Xtensa ESP-IDF target, but it is
> not verified. ESP8266 is not supported.

## Highlights

- `viper.compile_c(src[, opts])` compiles one restricted C-like function.
- `viper.buf(size)` allocates memory that can be passed to pointer parameters.
- Integer scalars, integer pointers, pointer indexing, loops, and branches are
  supported by the native backend.
- Raw integer pointers are rejected by default; pass `{ unsafe = true }` only
  when you really need them.
- In the tested ESP32-S3 setup, well-written C-like hot loops can be about
  10-20x faster than Lua. This depends heavily on the C-like code shape. Viper
  does limited optimization and will not automatically optimize code like GCC.

## Compatibility

| Target | Status |
| --- | --- |
| ESP32-S3 + ESP-IDF | Tested and usable |
| ESP32-S3 + NodeMCU ESP32 branch | Tested with external component build |
| ESP32-S3 + Arduino | Usable when the Arduino-ESP32 core is rebuilt with the required ESP-IDF config |
| ESP32 + ESP-IDF | Likely portable, not verified |
| ESP32 + Arduino | Likely portable when the Arduino-ESP32 core is rebuilt, not verified |
| ESP8266 | Not supported |

Viper generates code at runtime and allocates executable internal RAM. For
ESP32-S3 builds, memory protection must allow executable RAM:

```text
CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=n
```

## Quick Example

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

## Repository Layout

```text
src/              Parser, IR, Xtensa assembler, and native backend
ports/nodemcu/    NodeMCU ESP32 external component
ports/arduino/    Arduino LuaWrapper binding
docs/             API, C-like subset, and build notes
examples/         Lua examples and benchmark scripts
firmware/         Prebuilt test firmware images
```

## NodeMCU

For NodeMCU ESP32/S3, use this repository as an ESP-IDF external component:

```powershell
cd C:\Users\wzh\Documents\nodemcu-firmware
. .\sdk\esp32-esp-idf\export.ps1
$env:EXTRA_COMPONENT_DIRS='C:/Users/wzh/Documents/viper/ports/nodemcu'
idf.py set-target esp32s3
idf.py build
```

See [README.nodemcu.md](README.nodemcu.md) for the full Windows build, flash,
and smoke-test flow.

## Prebuilt ESP32-S3 Firmware

A tested ESP32-S3 NodeMCU image is included in
[firmware/esp32s3](firmware/esp32s3). It was built with Viper enabled and the
Lua console on USB Serial/JTAG. See
[firmware/esp32s3/README.md](firmware/esp32s3/README.md) for the flash command.

## Arduino

The Arduino port can be used on ESP32-class targets, but normal Arduino package
installs do not expose the ESP-IDF `sdkconfig` options required by executable
RAM. Rebuild the Arduino-ESP32 core with the required lower-level ESP-IDF config
before expecting generated native code to run correctly.

## C-like Subset

Viper is not a C compiler. It supports a small single-function subset:

- one function per source string;
- `int32_t`, `uint32_t`, `int16_t`, `uint16_t`, `int8_t`, `uint8_t`, `bool`,
  `void`, and scalar pointers;
- declarations, assignment, `if/else`, `for`, `return`, integer arithmetic,
  bit operations, comparisons, and `p[i]`.

Not supported: `#include`, macros, `struct`, `union`, `enum`, `typedef`, local
arrays, function calls, `sizeof`, casts, `switch`, `while`, `break`,
`continue`, `goto`, `*p`, `&x`, `.` and `->`.

## Performance Notes

To get the expected 10-20x speedup over Lua on ESP32-S3, keep the C-like code
simple:

- process a batch of data per native call;
- prefer `int32_t` and `uint32_t` for hot local variables;
- avoid division and modulo in inner loops;
- keep pointer indexes simple;
- rewrite complex expressions into a few explicit steps.

More details are in [docs/clike.md](docs/clike.md).

## Documentation

- [API](docs/api.md)
- [C-like subset](docs/c-like-subset.md)
- [C-like performance guide](docs/clike.md)
- [NodeMCU module build](README.nodemcu.md)
- [NodeMCU ESP32 notes](docs/build-nodemcu-esp32.md)
- [NodeMCU ESP8266 notes](docs/build-nodemcu-esp8266.md)
- [Examples](docs/examples.md)

## License

Viper is released under the GNU General Public License v3.0 only. See
[LICENSE](LICENSE).
