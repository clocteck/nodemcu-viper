local fill = viper.compile_c([[
void fill(uint8_t *p, int32_t n, uint8_t value) {
  int32_t i;
  for (i = 0; i < n; i = i + 1) {
    p[i] = value;
  }
}
]])

local b = viper.buf(16)
fill(b, 16, 0x5a)

print(b:get8(0), b:get8(15))

