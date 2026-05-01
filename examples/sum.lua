local sum = viper.compile_c([[
int32_t sum(uint8_t *p, int32_t n) {
  int32_t s = 0;
  int32_t i;
  for (i = 0; i < n; i = i + 1) {
    s = s + p[i];
  }
  return s;
}
]])

local b = viper.buf(8)
for i = 0, 7 do
  b:set8(i, i + 1)
end

print(sum(b, 8))

