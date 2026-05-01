# Viper C-like Performance Guide

Viper C-like is not GCC C. The pipeline is:

```text
C-like source -> Viper IR -> Xtensa native code
```

The goal is to write code that produces fewer IR instructions, fewer temporary
values, simpler loop bodies, fewer branches, and fewer division/modulo
operations.

Well-written Viper C-like code can be about 10-20x faster than equivalent Lua
hot loops on the tested ESP32-S3 setup. This is not guaranteed for every
program. The result depends heavily on how the C-like code is written, because
the compiler only performs limited optimization and will not optimize like GCC.

## Must Remember

- Write one function per source string. Multiple functions are not supported.
- `&&` and `||` are not short-circuit operators; both sides are evaluated.
- Very small native functions called frequently may not be worth it. Process a
  batch of data per call.
- Do not call the same compiled function recursively or concurrently from
  multiple tasks.
- `bounds = true` is not supported by the current native backend.
- Pointer parameters accept `viper.buf` by default. Raw integer addresses require
  `{ unsafe = true }`.

## Supported Statements

```c
variable declaration
assignment / += -= *= /= %=
postfix ++ / --
if / else
for (init; cond; post)
return
{ ... }
```

## Supported Expressions

```c
+ - * / %
<< >>
& | ^
== != ~= < <= > >=
&& ||
unary - ! ~
p[i]
```

## Not Supported

```c
#include / #define / macros
struct / union / enum / typedef
local arrays
function calls
sizeof / casts / ternary ?:
switch / while / do while
break / continue / goto
*p / &x
. / ->
```

Keep the `for` post section simple: use one assignment, `++`, or `--`.

## Types

Recommended for hot paths:

```c
int32_t
uint32_t
uint8_t * / uint16_t * / uint32_t *
```

Parseable types:

```c
int8_t / uint8_t
int16_t / uint16_t
int32_t / uint32_t
bool
char / short / int
signed/unsigned char/short/int
void
pointers to the scalar types above
```

`float` and `float *` can appear in parts of the parser, but the current native
backend does not support f32 execution. Do not use f32 in hot paths.

`uint8_t` and `uint16_t` are good memory element types, but they are usually not
the best loop-local hot variable types. Prefer `int32_t` or `uint32_t` for loop
counters and accumulators.

## Hard Limits

```text
IR instructions <= 4096
variables/temporary values <= 1024
native code <= 65536 bytes
literals <= 256 32-bit values
arguments < 128
```

## Existing Optimizations

The compiler currently has:

- Integer constant deduplication.
- Temp-only constant folding, copy propagation, and dead temp deletion.
- Compare + branch fusion.
- Temporary value forwarding.
- Resident registers for hot variables.
- Literal pool entry without a jump.
- Conditional branch relaxation.
- `x +/- const -> addi`.
- `x * 2^n -> shift`.
- Unsigned `x / 2^n -> logical shift`.
- Fast paths for `x << const` and `x >> const`.
- `x & low_bits_mask -> extui` when the mask is continuous low bits and <= 16
  bits.

Even with these optimizations, write code for Viper's compiler, not for GCC.

## Writing Fast Code

Prefer:

```c
for (i = 0; i < n; i = i + 1) {
  sum = sum + buf[i];
}
```

Avoid:

```c
sum += buf[(i * 3 + 7) % n];
```

Rewrite complex indexing as incremental state:

```c
idx = 0;
for (i = 0; i < n; i = i + 1) {
  sum = sum + buf[idx];
  idx = idx + 3;
}
```

If the divisor is a power of two, prefer `uint32_t` so `/ 2^n` can become a
shift.

