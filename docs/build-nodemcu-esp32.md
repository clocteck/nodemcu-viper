# Build With NodeMCU ESP32

This page is a short reference. The full tested Windows flow is in
[../README.nodemcu.md](../README.nodemcu.md).

## Status

- Tested on ESP32-S3 only.
- Classic ESP32 may work but still needs hardware validation.
- ESP8266 is not supported.

## External Component Build

Use the NodeMCU ESP32 branch and point `EXTRA_COMPONENT_DIRS` to the Viper
NodeMCU component directory:

```powershell
cd C:\Users\wzh\Documents\nodemcu-firmware
. .\sdk\esp32-esp-idf\export.ps1
$env:EXTRA_COMPONENT_DIRS='C:/Users/wzh/Documents/viper/ports/nodemcu'
idf.py set-target esp32s3
idf.py build
```

Use forward slashes in `EXTRA_COMPONENT_DIRS` on Windows.

## Required Configuration

```text
CONFIG_NODEMCU_CMODULE_VIPER=y
CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=n
```

For ESP32-S3 USB Serial/JTAG console input:

```text
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y
```

## Smoke Test

```lua
print("VIPER_TYPE", type(viper), type(viper and viper.compile_c), type(viper and viper.buf))
fn=viper.compile_c("int32_t add(int32_t a, int32_t b) { return a + b; }")
print("VIPER_ADD", fn(7,35))
```

Expected:

```text
VIPER_TYPE table function function
VIPER_ADD 42
```
