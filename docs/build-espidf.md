# Build With ESP-IDF

ESP-IDF is the recommended environment for a working Viper build. Unlike
Arduino, ESP-IDF lets you control the `sdkconfig` options required by generated
native code and executable memory handling.

This repository can be added as an ESP-IDF component. Include:

```text
src/
ports/nodemcu/ or your own Lua binding
```

The current tested target is ESP32-S3. Classic ESP32 may work but has not been
validated on real hardware.

Arduino projects may compile the same sources, but they usually cannot adjust
the required `sdkconfig`, so generated native code is not expected to run
correctly there.

