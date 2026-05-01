# Examples

Run the examples from `examples/` after building firmware with the `viper`
module enabled.

- `sum.lua`: sums bytes in a `viper.buf`.
- `fill.lua`: fills a `viper.buf` with byte values.
- `coremark.lua`: runs a CoreMark-style benchmark with CRC, matrix, state,
  and list kernels. It prints Viper vs Lua checksums, elapsed time, and
  `speedup_x100` (for example, `1234` means 12.34x).
