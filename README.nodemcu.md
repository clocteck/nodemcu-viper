# Build Viper As A NodeMCU Module

This document records the tested Windows flow for building Viper into the
NodeMCU ESP32 firmware as an external ESP-IDF component.

Tested hardware:

- ESP32-S3, USB Serial/JTAG console
- NodeMCU firmware branch: `dev-esp32`
- ESP-IDF: the version bundled by NodeMCU, tested with IDF 5.3

Classic ESP32 may work but is not verified. ESP8266 is not supported.

## 1. Clone NodeMCU

```powershell
cd C:\Users\wzh\Documents
git clone --recurse-submodules -b dev-esp32 https://github.com/nodemcu/nodemcu-firmware.git
cd nodemcu-firmware
```

Install ESP-IDF tools from the NodeMCU tree:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\sdk\esp32-esp-idf\install.ps1 esp32s3
```

Open a new PowerShell or export the environment in the current one:

```powershell
. .\sdk\esp32-esp-idf\export.ps1
```

Install Python requirements if the build asks for them:

```powershell
python -m pip install -r requirements.txt
```

The Windows build may also need GNU `make` for NodeMCU's `luac_cross` step. One
working option is Scoop:

```powershell
scoop install make
```

## 2. Add Viper As An External Component

Set `EXTRA_COMPONENT_DIRS` to the Viper NodeMCU port directory. Use forward
slashes on Windows because NodeMCU's CMake parsing treats backslashes as escapes.

```powershell
$env:EXTRA_COMPONENT_DIRS='C:/Users/wzh/Documents/viper/ports/nodemcu'
```

The component directory contains:

```text
ports/nodemcu/
  CMakeLists.txt
  Kconfig
  README.md
  viper.cpp
  viper_module.c
```

`viper_module.c` is a small C shim that registers the module with NodeMCU's
`module.h` / ROTable / `NODEMCU_MODULE` system. The larger implementation stays
in C++.

## 3. Required sdkconfig Options

Viper generates Xtensa instructions at runtime, so executable RAM must be
allowed:

```text
CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=n
CONFIG_NODEMCU_CMODULE_VIPER=y
```

For ESP32-S3 boards where the Lua console should use the same USB Serial/JTAG
port as flashing, set the primary console to USB Serial/JTAG:

```text
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y
```

If you use a separate UART0 adapter for the Lua console, keep NodeMCU's default
UART console instead.

## 4. Build

```powershell
cd C:\Users\wzh\Documents\nodemcu-firmware
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
. .\sdk\esp32-esp-idf\export.ps1
$env:PATH='C:\Users\wzh\scoop\shims;C:\Program Files\Git\usr\bin;' + $env:PATH
$env:EXTRA_COMPONENT_DIRS='C:/Users/wzh/Documents/viper/ports/nodemcu'

idf.py set-target esp32s3
idf.py build
```

Successful output should generate:

```text
build/nodemcu.bin
build/bootloader/bootloader.bin
build/partition_table/partition-table.bin
```

You can confirm the module is linked by checking the ELF symbols:

```powershell
$nm = "$env:USERPROFILE\.espressif\tools\xtensa-esp-elf\esp-13.2.0_20240530\xtensa-esp-elf\bin\xtensa-esp32s3-elf-nm.exe"
& $nm build\nodemcu.elf | rg "lua_lib_VIPER|VIPER_module_selected1|viper_ROTable"
```

Expected symbols:

```text
lua_lib_VIPER
VIPER_module_selected1
viper_ROTable
```

## 5. Flash

```powershell
idf.py -p COM6 flash
```

Or flash the copied prebuilt files from this repository:

```powershell
python -m esptool --chip esp32s3 -p COM6 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m 0x0 firmware/esp32s3/bootloader.bin 0x8000 firmware/esp32s3/partition-table.bin 0x10000 firmware/esp32s3/nodemcu-viper-esp32s3.bin
```

The prebuilt image in `firmware/esp32s3/` was built for the local ESP32-S3 test
board with USB Serial/JTAG as the Lua console.

## 6. Smoke Test

Open the Lua console at `115200` baud and run:

```lua
print("VIPER_TYPE", type(viper), type(viper and viper.compile_c), type(viper and viper.buf))
fn=viper.compile_c("int32_t add(int32_t a, int32_t b) { return a + b; }")
print("VIPER_ADD", fn(7,35))
```

Expected result:

```text
VIPER_TYPE table function function
VIPER_ADD 42
```

## 7. Performance Expectation

On the tested ESP32-S3 setup, simple hot loops written in the supported C-like
subset can be about 10-20x faster than equivalent Lua. This is not guaranteed
for every function. Viper does limited optimization, so write code with simple
indexes, explicit temporaries, few divisions/modulos, and enough work per native
call.

Run the benchmark demo after uploading `examples/coremark.lua`:

```lua
ROUNDS = 100
dofile("coremark.lua")
```

## Troubleshooting

- `EXTRA_COMPONENT_DIRS` not picked up: use forward slashes and point to
  `ports/nodemcu`, not the repository root.
- `module.h` not found: make sure `base_nodemcu` is in `PRIV_REQUIRES`.
- `lua.h` C++ ROTable typedef conflict: use the patched NodeMCU headers or keep
  NodeMCU module registration in the C shim.
- Flash works but Lua input does not work on ESP32-S3 USB port: set
  `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`. The secondary USB Serial/JTAG console
  is output-only and does not provide REPL input.
- Runtime allocation fails: reduce generated function size or test on a board
  with more internal RAM.
