# API

## `viper.compile_c(src[, opts]) -> function`

Compiles one C-like function and returns a callable Lua userdata/function.

Parameters:

- `src`: source code string containing exactly one function.
- `opts.unsafe`: allow raw integer pointer arguments. Default is `false`.
- `opts.bounds_check`: emit pointer bounds checks where supported. Default is
  `false`.

The compiled function accepts Lua integers, booleans, and `viper.buf` values.
Pointer parameters require `viper.buf` unless `unsafe` is enabled.

## `viper.buf(size) -> buffer`

Allocates a zero-filled buffer. Small buffers prefer internal RAM; larger
buffers may use PSRAM when available.

Buffer methods:

- `buf:len()`
- `buf:get8(index)`, `buf:set8(index, value)`
- `buf:get16(index)`, `buf:set16(index, value)`
- `buf:get32(index)`, `buf:set32(index, value)`
- `buf:getf32(index)`, `buf:setf32(index, value)`

Indexes are zero-based element indexes. For example, `get32(1)` reads four
bytes starting at byte offset 4.

## Errors

Compilation errors are raised as Lua errors. Common causes are unsupported C
syntax, too many IR instructions, unsupported f32 native operations, or running
out of executable/internal memory.

