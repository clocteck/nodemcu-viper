# Build With ESP-IDF

ESP-IDF is the most direct environment for a working Viper build because it lets
you control the `sdkconfig` options required by generated native code and
executable memory handling.

This repository can be added as an ESP-IDF component. Include:

```text
src/
ports/nodemcu/ or your own Lua binding
```

The current tested target is ESP32-S3. Classic ESP32 may work but has not been
validated on real hardware.

Arduino projects can use the same sources, but the Arduino-ESP32 core must be
rebuilt with the required lower-level ESP-IDF config. A normal Arduino package
install may compile Viper but will not necessarily allow generated native code
to execute correctly.
