#include <Arduino.h>
#include <LuaWrapper.h>
#include <esp_heap_caps.h>

#include <cstdint>
#include <cstring>
#include <new>
#include <string>

#include "lua_viper.h"
#include "viper_internal.h"

namespace lua_nodemcu_viper::internal
{

static constexpr const char *kViperFunctionMeta = "nodemcu.viper.function";
static constexpr const char *kViperBufferMeta = "nodemcu.viper.buffer";
static ViperAllocScope *s_viper_alloc_scope = nullptr;

/**
 * @brief Enter the Viper compile-time allocation failure guard scope.
 */
void enter_viper_alloc_scope(ViperAllocScope &scope)
{
    scope.previous = s_viper_alloc_scope;
    scope.error = nullptr;
    s_viper_alloc_scope = &scope;
}

/**
 * @brief Leave the Viper compile-time allocation failure guard scope.
 */
void leave_viper_alloc_scope(ViperAllocScope &scope)
{
    if (s_viper_alloc_scope == &scope)
        s_viper_alloc_scope = scope.previous;
}

/**
 * @brief Convert Viper compile-time working-set allocation failures to compile errors instead of aborting.
 */
[[noreturn]] void fail_viper_alloc(const char *msg)
{
    if (s_viper_alloc_scope)
    {
        s_viper_alloc_scope->error = msg ? msg : "viper.compile_c: out of memory";
        std::longjmp(s_viper_alloc_scope->env, 1);
    }
    std::abort();
}

/**
 * @brief Allocate memory for viper compile/call working sets, preferring internal RAM.
 * @param bytes Number of bytes required.
 * @return void* Pointer to the allocated memory, or nullptr on failure.
 */
void *alloc_viper_work_data(size_t bytes)
{
    if (bytes == 0)
        return nullptr;

    const bool prefer_internal = bytes <= kViperWorkInternalLimitBytes;
    const uint32_t primary_caps = prefer_internal ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                                                  : (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const uint32_t fallback_caps = prefer_internal ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                                   : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    void *ptr = heap_caps_malloc(bytes, primary_caps);
    if (!ptr)
        ptr = heap_caps_malloc(bytes, fallback_caps);
    return ptr;
}

/**
 * @brief Allocate the data area for viper.buf; buffers below 4 KB prefer internal RAM, others use PSRAM.
 * @param bytes Number of bytes required.
 * @param out_caps Caps actually used for allocation; may be null.
 * @return uint8_t* Pointer to the allocated data.
 */
static uint8_t *alloc_viper_buffer_data(size_t bytes, uint32_t *out_caps = nullptr)
{
    uint32_t caps = bytes < kViperBufferInternalLimitBytes ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                                                           : (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *ptr = (uint8_t *)heap_caps_calloc(bytes, 1, caps);
    if (!ptr && bytes < kViperBufferInternalLimitBytes)
    {
        caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        ptr = (uint8_t *)heap_caps_calloc(bytes, 1, caps);
    }
    if (out_caps)
        *out_caps = ptr ? caps : 0;
    return ptr;
}

/**
 * @brief Create the compiled function object in internal RAM to avoid PSRAM during cache-disabled windows.
 */
static CompiledFunction *create_compiled_function()
{
    void *mem = heap_caps_calloc(1, sizeof(CompiledFunction), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!mem)
        return nullptr;
    return new (mem) CompiledFunction();
}

/**
 * @brief Destroy an object created by create_compiled_function.
 */
static void destroy_compiled_function(CompiledFunction *fn)
{
    if (!fn)
        return;
    release_native_storage(*fn);
    fn->~CompiledFunction();
    heap_caps_free(fn);
}

/**
 * @brief Read viper buffer userdata from the Lua stack.
 */
static ViperBuffer *check_buffer(lua_State *L, int idx)
{
    return (ViperBuffer *)luaL_checkudata(L, idx, kViperBufferMeta);
}

/**
 * @brief Read compiled function userdata from the Lua stack.
 */
static CompiledFunction *check_function(lua_State *L, int idx)
{
    void *ud = luaL_checkudata(L, idx, kViperFunctionMeta);
    return ud ? *((CompiledFunction **)ud) : nullptr;
}

/**
 * @brief Convert Lua arguments to 32-bit arguments used by the native entry.
 */
static bool read_native_arg(lua_State *L, int idx, ValueType type, bool unsafe_ptr, ViperNativeArg &out)
{
    out.value = 0;
    out.bytes = 0;
    if (is_ptr_type(type))
    {
        if (luaL_testudata(L, idx, kViperBufferMeta))
        {
            ViperBuffer *buf = check_buffer(L, idx);
            out.value = (uint32_t)(uintptr_t)(buf ? buf->data : nullptr);
            out.bytes = (uint32_t)(buf ? buf->bytes : 0);
            return out.value != 0;
        }
        if (unsafe_ptr && lua_isinteger(L, idx))
        {
            out.value = (uint32_t)(uintptr_t)lua_tointeger(L, idx);
            out.bytes = UINT32_MAX;
            return out.value != 0;
        }
        luaL_error(L, "viper: pointer argument expects viper.buf userdata");
        return false;
    }

    if (type == ValueType::F32)
    {
        luaL_error(L, "viper: f32 native argument is not supported yet");
        return false;
    }
    if (type == ValueType::Bool)
    {
        out.value = lua_toboolean(L, idx) ? 1U : 0U;
        return true;
    }
    out.value = (uint32_t)cast_scalar((int64_t)luaL_checkinteger(L, idx), type);
    return true;
}

/**
 * @brief Lua API: compiled_function(...).
 */
static int l_viper_function_call(lua_State *L)
{
    CompiledFunction *fn = check_function(L, 1);
    lua_remove(L, 1);
    if (!fn || !fn->native_entry)
        return luaL_error(L, "viper: invalid native function");

    const int argc = lua_gettop(L);
    if (argc != fn->param_count)
        return luaL_error(L, "viper: expected %d args, got %d", fn->param_count, argc);

    if (fn->native_args.size() < (size_t)fn->param_count)
        return luaL_error(L, "viper: invalid native argument frame");
    for (int i = 0; i < fn->param_count; ++i)
    {
        if (!read_native_arg(L, i + 1, fn->vars[i].type, fn->unsafe_ptr, fn->native_args[(size_t)i]))
            return luaL_error(L, "viper: failed to read argument");
    }

    if (fn->native_frame_words > 0 && !fn->native_frame)
        return luaL_error(L, "viper: invalid native local frame");

    ViperNativeCall call;
    call.args = fn->native_args.empty() ? nullptr : fn->native_args.data();
    call.locals = fn->native_frame;
    const uint32_t ret = fn->native_entry(&call);
    if (fn->return_type == ValueType::Void)
        return 0;
    if (fn->return_type == ValueType::F32)
        return luaL_error(L, "viper: f32 native return is not supported yet");
    lua_pushinteger(L, (lua_Integer)cast_scalar((int64_t)ret, fn->return_type));
    return 1;
}

/**
 * @brief Lua GC: release a compiled function.
 */
static int l_viper_function_gc(lua_State *L)
{
    void *ud = luaL_checkudata(L, 1, kViperFunctionMeta);
    CompiledFunction **fn = (CompiledFunction **)ud;
    if (fn && *fn)
    {
        destroy_compiled_function(*fn);
        *fn = nullptr;
    }
    return 0;
}

/**
 * @brief Lua API: viper.compile_c(src, opts?) -> function.
 */
static int l_viper_compile_c(lua_State *L)
{
    size_t len = 0;
    const char *src = luaL_checklstring(L, 1, &len);
    (void)len;

    bool unsafe_ptr = false;
    bool bounds_check = false;
    if (lua_istable(L, 2))
    {
        lua_getfield(L, 2, "unsafe");
        unsafe_ptr = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        lua_getfield(L, 2, "bounds");
        if (!lua_isnil(L, -1))
            bounds_check = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }

    CompiledFunction *fn = create_compiled_function();
    if (!fn)
        return luaL_error(L, "viper.compile_c: out of memory");

    std::string err;
    if (!parse_viper_c(src, unsafe_ptr, bounds_check, *fn, err))
    {
        destroy_compiled_function(fn);
        return luaL_error(L, "%s", err.c_str());
    }

    ViperAllocScope alloc_scope;
    enter_viper_alloc_scope(alloc_scope);
    if (setjmp(alloc_scope.env) != 0)
    {
        err = alloc_scope.error ? alloc_scope.error : "viper.compile_c: out of memory";
        leave_viper_alloc_scope(alloc_scope);
        destroy_compiled_function(fn);
        return luaL_error(L, "%s", err.c_str());
    }
    fn->native_args.resize((size_t)fn->param_count);
    leave_viper_alloc_scope(alloc_scope);

    if (!compile_native(*fn, err))
    {
        destroy_compiled_function(fn);
        return luaL_error(L, "%s", err.c_str());
    }
    fn->code.clear();
    fn->code.shrink_to_fit();
    if (fn->vars.size() > (size_t)fn->param_count)
    {
        fn->vars.resize((size_t)fn->param_count);
        fn->vars.shrink_to_fit();
    }

    void *ud = lua_newuserdatauv(L, sizeof(CompiledFunction *), 0);
    *((CompiledFunction **)ud) = fn;
    luaL_getmetatable(L, kViperFunctionMeta);
    lua_setmetatable(L, -2);
    return 1;
}

/**
 * @brief Lua API: viper.buf(size) -> buffer.
 */
static int l_viper_buf(lua_State *L)
{
    const lua_Integer size_arg = luaL_checkinteger(L, 1);
    if (size_arg <= 0)
        return luaL_error(L, "viper.buf: size must be > 0");

    ViperBuffer *buf = (ViperBuffer *)lua_newuserdatauv(L, sizeof(ViperBuffer), 0);
    buf->bytes = (size_t)size_arg;
    buf->data = alloc_viper_buffer_data(buf->bytes);
    if (!buf->data)
        return luaL_error(L, "viper.buf: out of memory");

    luaL_getmetatable(L, kViperBufferMeta);
    lua_setmetatable(L, -2);
    return 1;
}

/**
 * @brief Lua GC: release a buffer.
 */
static int l_viper_buffer_gc(lua_State *L)
{
    ViperBuffer *buf = check_buffer(L, 1);
    if (buf && buf->data)
    {
        heap_caps_free(buf->data);
        buf->data = nullptr;
        buf->bytes = 0;
    }
    return 0;
}

/**
 * @brief Lua API: buffer:len() -> bytes.
 */
static int l_viper_buffer_len(lua_State *L)
{
    ViperBuffer *buf = check_buffer(L, 1);
    lua_pushinteger(L, (lua_Integer)(buf ? buf->bytes : 0));
    return 1;
}

/**
 * @brief Lua API: buffer:get8(index) -> value, for script-side verification.
 */
static int l_viper_buffer_get8(lua_State *L)
{
    ViperBuffer *buf = check_buffer(L, 1);
    const lua_Integer index = luaL_checkinteger(L, 2);
    if (!buf || index < 0 || (size_t)index >= buf->bytes)
        return luaL_error(L, "viper.buf:get8: index out of range");
    lua_pushinteger(L, buf->data[index]);
    return 1;
}

/**
 * @brief Lua API: buffer:set8(index, value).
 */
static int l_viper_buffer_set8(lua_State *L)
{
    ViperBuffer *buf = check_buffer(L, 1);
    const lua_Integer index = luaL_checkinteger(L, 2);
    const lua_Integer value = luaL_checkinteger(L, 3);
    if (!buf || index < 0 || (size_t)index >= buf->bytes)
        return luaL_error(L, "viper.buf:set8: index out of range");
    buf->data[index] = (uint8_t)value;
    return 0;
}

/**
 * @brief Lua API: buffer:get16(index) -> value, where index is the element index.
 */
static int l_viper_buffer_get16(lua_State *L)
{
    ViperBuffer *buf = check_buffer(L, 1);
    const lua_Integer index = luaL_checkinteger(L, 2);
    const size_t off = (size_t)index * 2;
    if (!buf || index < 0 || off > buf->bytes || 2 > buf->bytes - off)
        return luaL_error(L, "viper.buf:get16: index out of range");
    uint16_t value = 0;
    std::memcpy(&value, buf->data + off, sizeof(value));
    lua_pushinteger(L, value);
    return 1;
}

/**
 * @brief Lua API: buffer:set16(index, value), where index is the element index.
 */
static int l_viper_buffer_set16(lua_State *L)
{
    ViperBuffer *buf = check_buffer(L, 1);
    const lua_Integer index = luaL_checkinteger(L, 2);
    const lua_Integer value = luaL_checkinteger(L, 3);
    const size_t off = (size_t)index * 2;
    if (!buf || index < 0 || off > buf->bytes || 2 > buf->bytes - off)
        return luaL_error(L, "viper.buf:set16: index out of range");
    const uint16_t v = (uint16_t)value;
    std::memcpy(buf->data + off, &v, sizeof(v));
    return 0;
}

/**
 * @brief Lua API: buffer:get32(index) -> value, where index is the element index.
 */
static int l_viper_buffer_get32(lua_State *L)
{
    ViperBuffer *buf = check_buffer(L, 1);
    const lua_Integer index = luaL_checkinteger(L, 2);
    const size_t off = (size_t)index * 4;
    if (!buf || index < 0 || off > buf->bytes || 4 > buf->bytes - off)
        return luaL_error(L, "viper.buf:get32: index out of range");
    uint32_t value = 0;
    std::memcpy(&value, buf->data + off, sizeof(value));
    lua_pushinteger(L, value);
    return 1;
}

/**
 * @brief Lua API: buffer:set32(index, value), where index is the element index.
 */
static int l_viper_buffer_set32(lua_State *L)
{
    ViperBuffer *buf = check_buffer(L, 1);
    const lua_Integer index = luaL_checkinteger(L, 2);
    const lua_Integer value = luaL_checkinteger(L, 3);
    const size_t off = (size_t)index * 4;
    if (!buf || index < 0 || off > buf->bytes || 4 > buf->bytes - off)
        return luaL_error(L, "viper.buf:set32: index out of range");
    const uint32_t v = (uint32_t)value;
    std::memcpy(buf->data + off, &v, sizeof(v));
    return 0;
}

/**
 * @brief Lua API: buffer:getf32(index) -> value, where index is the element index.
 */
static int l_viper_buffer_getf32(lua_State *L)
{
    ViperBuffer *buf = check_buffer(L, 1);
    const lua_Integer index = luaL_checkinteger(L, 2);
    const size_t off = (size_t)index * 4;
    if (!buf || index < 0 || off > buf->bytes || 4 > buf->bytes - off)
        return luaL_error(L, "viper.buf:getf32: index out of range");
    float value = 0.0f;
    std::memcpy(&value, buf->data + off, sizeof(value));
    lua_pushnumber(L, (lua_Number)value);
    return 1;
}

/**
 * @brief Lua API: buffer:setf32(index, value), where index is the element index.
 */
static int l_viper_buffer_setf32(lua_State *L)
{
    ViperBuffer *buf = check_buffer(L, 1);
    const lua_Integer index = luaL_checkinteger(L, 2);
    const float value = (float)luaL_checknumber(L, 3);
    const size_t off = (size_t)index * 4;
    if (!buf || index < 0 || off > buf->bytes || 4 > buf->bytes - off)
        return luaL_error(L, "viper.buf:setf32: index out of range");
    std::memcpy(buf->data + off, &value, sizeof(value));
    return 0;
}

/**
 * @brief Create the function userdata metatable.
 */
static void create_function_meta(lua_State *L)
{
    if (luaL_newmetatable(L, kViperFunctionMeta))
    {
        lua_pushcfunction(L, l_viper_function_call);
        lua_setfield(L, -2, "__call");
        lua_pushcfunction(L, l_viper_function_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
}

/**
 * @brief Create the buffer userdata metatable.
 */
static void create_buffer_meta(lua_State *L)
{
    if (luaL_newmetatable(L, kViperBufferMeta))
    {
        lua_pushcfunction(L, l_viper_buffer_gc);
        lua_setfield(L, -2, "__gc");

        lua_newtable(L);
        lua_pushcfunction(L, l_viper_buffer_len);
        lua_setfield(L, -2, "len");
        lua_pushcfunction(L, l_viper_buffer_get8);
        lua_setfield(L, -2, "get8");
        lua_pushcfunction(L, l_viper_buffer_set8);
        lua_setfield(L, -2, "set8");
        lua_pushcfunction(L, l_viper_buffer_get16);
        lua_setfield(L, -2, "get16");
        lua_pushcfunction(L, l_viper_buffer_set16);
        lua_setfield(L, -2, "set16");
        lua_pushcfunction(L, l_viper_buffer_get32);
        lua_setfield(L, -2, "get32");
        lua_pushcfunction(L, l_viper_buffer_set32);
        lua_setfield(L, -2, "set32");
        lua_pushcfunction(L, l_viper_buffer_getf32);
        lua_setfield(L, -2, "getf32");
        lua_pushcfunction(L, l_viper_buffer_setf32);
        lua_setfield(L, -2, "setf32");
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
}

} // namespace lua_nodemcu_viper::internal

namespace lua_nodemcu_viper
{

void register_api(LuaWrapper &lua)
{
    if (!lua._state)
    {
        return;
    }

    lua_State *L = lua._state;
    internal::create_function_meta(L);
    internal::create_buffer_meta(L);

    lua_newtable(L);
    lua_pushcfunction(L, internal::l_viper_compile_c);
    lua_setfield(L, -2, "compile_c");
    lua_pushcfunction(L, internal::l_viper_buf);
    lua_setfield(L, -2, "buf");
    lua_setglobal(L, "viper");
}

void cleanup()
{
}

} // namespace lua_nodemcu_viper
