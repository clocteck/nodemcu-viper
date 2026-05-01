# C-like Subset

Viper is not a C compiler. It accepts a compact C-like subset designed for
small hot loops.

For performance-focused style rules, see [clike.md](clike.md).

Supported:

- One function per source string.
- `int32_t`, `uint32_t`, `int16_t`, `uint16_t`, `int8_t`, `uint8_t`, `bool`,
  `void`, and pointer forms such as `uint8_t *`.
- Local variable declarations.
- Assignment and compound assignment where implemented.
- `if`, `else`, `for`, and `return`.
- Integer arithmetic, bitwise operations, comparisons, and pointer indexing.

Not supported or not ready:

- `struct`, `union`, `enum`, arrays as locals, macros, includes, function calls,
  recursion, dynamic allocation, and multiple functions.
- Native f32 execution.
- Standard C library calls.
- ESP8266 native backend.

Limits:

- Maximum IR instructions: 4096.
- Maximum variables/temporaries: 1024.
- Maximum native code size: 64 KiB.
- Maximum native literal count: 256.

Performance tips:

- Batch work inside one compiled function.
- Prefer `int32_t`/`uint32_t`.
- Avoid division and modulo in tight loops when possible.
- Keep pointer indexing simple.
- Use `viper.buf` instead of raw addresses unless you are debugging low-level
  memory access.
- Well-written C-like code can be about 10-20x faster than equivalent Lua hot
  loops on the tested ESP32-S3 setup, but the compiler only performs limited
  optimization. Code shape matters.
