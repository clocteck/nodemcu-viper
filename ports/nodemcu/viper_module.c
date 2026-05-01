#include "module.h"

extern int viper_nodemcu_compile_c(lua_State *L);
extern int viper_nodemcu_buf(lua_State *L);
extern int viper_nodemcu_init(lua_State *L);

LROT_BEGIN(viper, NULL, 0)
    LROT_FUNCENTRY(compile_c, viper_nodemcu_compile_c)
    LROT_FUNCENTRY(buf, viper_nodemcu_buf)
LROT_END(viper, NULL, 0)

NODEMCU_MODULE(VIPER, "viper", viper, viper_nodemcu_init);
