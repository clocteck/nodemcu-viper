# Arduino Port

This port keeps the original `LuaWrapper` integration.

```cpp
#include "lua_viper.h"

lua_nodemcu_viper::register_api(lua);
```

Add both `src/` and `ports/arduino/` to your include/source paths.

Status: usable on ESP32-class targets when the Arduino-ESP32 core is rebuilt
with the required lower-level ESP-IDF config. Normal Arduino package installs
usually cannot change the `sdkconfig` options required by executable RAM, so a
plain install may compile Viper but fail to run generated native code.
