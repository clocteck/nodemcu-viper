#pragma once

#include <LuaWrapper.h>

namespace lua_nodemcu_viper
{

/**
 * @brief Register the `viper` typed IR module in the Lua global environment.
 * @param lua Lua wrapper object.
 */
void register_api(LuaWrapper &lua);

/**
 * @brief Clean up viper module state.
 *
 * Compiled artifacts and buffers for the current viper module are managed by
 * Lua userdata lifetimes, so this remains as a unified lifecycle entry point.
 */
void cleanup();

} // namespace lua_nodemcu_viper
