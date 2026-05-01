# Arduino Port

This port keeps the original `LuaWrapper` integration.

```cpp
#include "lua_viper.h"

lua_nodemcu_viper::register_api(lua);
```

Add both `src/` and `ports/arduino/` to your include/source paths.

Status: compile-only for now. Arduino projects usually cannot change the
required ESP-IDF `sdkconfig` options, so the module can build but is not expected
to run generated native code correctly. Use ESP-IDF for a working target.
