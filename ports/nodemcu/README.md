# NodeMCU ESP32 Port

This directory is an ESP-IDF external component for the NodeMCU ESP32 branch.

Use it from a NodeMCU firmware checkout:

```powershell
cd C:\Users\wzh\Documents\nodemcu-firmware
. .\sdk\esp32-esp-idf\export.ps1
$env:EXTRA_COMPONENT_DIRS='C:/Users/wzh/Documents/viper/ports/nodemcu'
idf.py build
```

Important options:

```text
CONFIG_NODEMCU_CMODULE_VIPER=y
CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=n
```

For ESP32-S3 USB Serial/JTAG Lua console input:

```text
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y
```

Files:

- `viper.cpp`: Lua binding and Viper runtime glue.
- `viper_module.c`: C shim for `module.h`, ROTable, and `NODEMCU_MODULE`.
- `CMakeLists.txt`: external component build rules.
- `Kconfig`: `CONFIG_NODEMCU_CMODULE_VIPER`.

Smoke test:

```lua
print(type(viper), type(viper.compile_c), type(viper.buf))
fn=viper.compile_c("int32_t add(int32_t a, int32_t b) { return a + b; }")
print(fn(7,35))
```

Expected result:

```text
table function function
42
```

See the root [README.nodemcu.md](../../README.nodemcu.md) for the full Windows
build, flash, and verification flow.
