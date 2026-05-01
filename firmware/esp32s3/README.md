# Prebuilt ESP32-S3 NodeMCU Firmware

These files were built from the local NodeMCU ESP32 branch with Viper enabled as
an external component.

Included files:

- `bootloader.bin`
- `partition-table.bin`
- `nodemcu-viper-esp32s3.bin`

Tested board:

- ESP32-S3, USB Serial/JTAG
- 4 MB flash setting
- Lua console on USB Serial/JTAG

Flash:

```powershell
python -m esptool --chip esp32s3 -p COM6 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m 0x0 firmware/esp32s3/bootloader.bin 0x8000 firmware/esp32s3/partition-table.bin 0x10000 firmware/esp32s3/nodemcu-viper-esp32s3.bin
```

Smoke test:

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
