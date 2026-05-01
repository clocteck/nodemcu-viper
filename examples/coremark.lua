-- CoreMark-style Viper benchmark.
--
-- Run from the NodeMCU prompt after flashing firmware with the viper module:
--   dofile("coremark.lua")
--
-- Optional:
--   ROUNDS = 100
--   dofile("coremark.lua")

local rounds = ROUNDS or 30
if rounds < 1 then
  rounds = 1
end

local VIPER_CRC_SRC = [=[
uint32_t coremark_crc(uint8_t *buf, uint16_t *tab, int32_t iter) {
  uint32_t crc = iter & 65535;
  uint32_t v = 0;
  uint32_t x = 0;
  uint32_t add = (iter + 1) & 255;
  int32_t i = 0;

  for (i = 0; i < 256; i = i + 1) {
    v = (buf[i] + add) & 255;
    add = (add + 1) & 255;

    x = crc ^ v;
    crc = tab[x & 255] ^ tab[256 + (x >> 8)];
  }

  return crc & 65535;
}
]=]

local VIPER_MATRIX_SRC = [=[
uint32_t coremark_matrix(uint16_t *a, uint16_t *b, uint16_t *c, uint16_t *tab, int32_t iter) {
  uint32_t crc = 0;
  uint32_t v = 0;
  uint32_t x = 0;
  uint32_t sum = 0;
  uint32_t folded = 0;
  int32_t i = 0;
  int32_t k = 0;
  int32_t row = 0;
  int32_t col = 0;
  int32_t base = 0;
  int32_t bidx = 0;
  int32_t kconst = (iter % 13) + 1;

  for (i = 0; i < 100; i = i + 1) {
    v = ((a[i] * kconst) + b[i]) & 65535;
    c[i] = v;

    x = crc ^ v;
    crc = tab[x & 255] ^ tab[256 + (x >> 8)];
  }

  for (row = 0; row < 10; row = row + 1) {
    sum = 0;
    base = row * 10;

    for (col = 0; col < 10; col = col + 1) {
      sum = (sum + (a[base + col] * b[col])) & 65535;
    }

    x = crc ^ sum;
    crc = tab[x & 255] ^ tab[256 + (x >> 8)];
  }

  for (row = 0; row < 10; row = row + 1) {
    base = row * 10;

    for (col = 0; col < 10; col = col + 1) {
      sum = 0;
      bidx = col;

      for (k = 0; k < 10; k = k + 1) {
        sum = (sum + (a[base + k] * b[bidx])) & 65535;
        bidx = bidx + 10;
      }

      c[base + col] = sum;
    }
  }

  for (i = 0; i < 100; i = i + 3) {
    v = c[i];
    folded = ((((v >> 4) & 255) * 17) + (v & 15)) & 65535;

    x = crc ^ folded;
    crc = tab[x & 255] ^ tab[256 + (x >> 8)];
  }

  return crc & 65535;
}
]=]

local VIPER_STATE_SRC = [=[
uint32_t coremark_state(uint8_t *buf, uint16_t *tab, int32_t iter) {
  uint32_t crc = iter & 65535;
  uint32_t ch = 0;
  uint32_t v = 0;
  uint32_t x = 0;
  int32_t i = 0;
  int32_t st = 0;
  int32_t valid = 0;
  int32_t invalid = 0;
  int32_t trans = 0;
  int32_t old = 0;
  bool digit = false;
  bool sign = false;
  bool dot = false;
  bool exp = false;
  int32_t salt_mod = (iter % 31) + 17;
  int32_t salt_count = salt_mod;
  uint32_t salt_xor = iter & 7;

  for (i = 0; i < 137; i = i + 1) {
    ch = buf[i];

    salt_count = salt_count - 1;
    if (salt_count == 0) {
      ch = ch ^ salt_xor;
      salt_count = salt_mod;
    }

    if (ch == 44) {
      if (st == 2 || st == 4 || st == 7) {
        valid = valid + 1;
      } else {
        invalid = invalid + 1;
      }

      v = st + valid * 3 + invalid * 7;

      x = crc ^ (v & 65535);
      crc = tab[x & 255] ^ tab[256 + (x >> 8)];

      st = 0;
    } else {
      digit = ch >= 48 && ch <= 57;
      sign = ch == 43 || ch == 45;
      dot = ch == 46;
      exp = ch == 69 || ch == 101;
      old = st;

      if (st == 0) {
        if (sign) {
          st = 1;
        } else if (digit) {
          st = 2;
        } else if (dot) {
          st = 3;
        } else {
          st = 8;
        }
      } else if (st == 1) {
        if (digit) {
          st = 2;
        } else if (dot) {
          st = 3;
        } else {
          st = 8;
        }
      } else if (st == 2) {
        if (digit) {
          st = 2;
        } else if (dot) {
          st = 4;
        } else if (exp) {
          st = 5;
        } else {
          st = 8;
        }
      } else if (st == 3) {
        if (digit) {
          st = 4;
        } else {
          st = 8;
        }
      } else if (st == 4) {
        if (digit) {
          st = 4;
        } else if (exp) {
          st = 5;
        } else {
          st = 8;
        }
      } else if (st == 5) {
        if (sign) {
          st = 6;
        } else if (digit) {
          st = 7;
        } else {
          st = 8;
        }
      } else if (st == 6) {
        if (digit) {
          st = 7;
        } else {
          st = 8;
        }
      } else if (st == 7) {
        if (digit) {
          st = 7;
        } else {
          st = 8;
        }
      } else {
        st = 8;
      }

      if (st != old) {
        trans = trans + 1;
      }
    }
  }

  if (st == 2 || st == 4 || st == 7) {
    valid = valid + 1;
  } else {
    invalid = invalid + 1;
  }

  x = crc ^ (valid & 65535);
  crc = tab[x & 255] ^ tab[256 + (x >> 8)];

  x = crc ^ (invalid & 65535);
  crc = tab[x & 255] ^ tab[256 + (x >> 8)];

  x = crc ^ (trans & 65535);
  crc = tab[x & 255] ^ tab[256 + (x >> 8)];

  return crc & 65535;
}
]=]

local VIPER_LIST_SRC = [=[
uint32_t coremark_list(uint16_t *ldata, uint16_t *lnext, uint16_t *tab, int32_t iter) {
  uint32_t crc = 0;
  uint32_t inner = 0;
  uint32_t x = 0;
  uint32_t data = 0;
  int32_t i = 0;
  int32_t j = 0;
  int32_t key = 0;
  int32_t idx = 0;
  int32_t sorted = 65535;
  int32_t prev = 65535;
  int32_t cur = 0;
  int32_t next = 0;

  for (i = 0; i < 8; i = i + 1) {
    key = ((iter + ((i + 1) * 7)) % 72) + 1;
    idx = 0;
    data = key;

    for (j = 0; j < 72; j = j + 1) {
      if (idx != 65535) {
        if ((idx + 1) == key) {
          data = ldata[idx] + idx + 1;
          idx = 65535;
        } else {
          idx = lnext[idx];
        }
      }
    }

    x = crc ^ (data & 65535);
    crc = tab[x & 255] ^ tab[256 + (x >> 8)];
  }

  sorted = 65535;

  for (i = 0; i < 72; i = i + 1) {
    cur = i;
    next = sorted;
    prev = 65535;

    for (j = 0; j < 72; j = j + 1) {
      if (next != 65535) {
        if ((ldata[cur] < ldata[next]) || ((ldata[cur] == ldata[next]) && (cur < next))) {
          j = 72;
        } else {
          prev = next;
          next = lnext[next];
        }
      }
    }

    lnext[cur] = next;

    if (prev == 65535) {
      sorted = cur;
    } else {
      lnext[prev] = cur;
    }
  }

  idx = sorted;
  inner = 0;

  for (i = 0; i < 72; i = i + 1) {
    if (idx != 65535) {
      x = inner ^ ((idx + 1) & 65535);
      inner = tab[x & 255] ^ tab[256 + (x >> 8)];

      x = inner ^ (ldata[idx] & 65535);
      inner = tab[x & 255] ^ tab[256 + (x >> 8)];

      idx = lnext[idx];
    }
  }

  x = crc ^ (inner & 65535);
  crc = tab[x & 255] ^ tab[256 + (x >> 8)];

  for (i = 0; i < 71; i = i + 1) {
    lnext[i] = i + 1;
  }

  lnext[71] = 65535;
  idx = 0;
  inner = 0;

  for (i = 0; i < 16; i = i + 1) {
    x = inner ^ ((idx + 1) & 65535);
    inner = tab[x & 255] ^ tab[256 + (x >> 8)];

    x = inner ^ (ldata[idx] & 65535);
    inner = tab[x & 255] ^ tab[256 + (x >> 8)];

    idx = lnext[idx];
  }

  x = crc ^ (inner & 65535);
  crc = tab[x & 255] ^ tab[256 + (x >> 8)];

  return crc & 65535;
}
]=]

local function now_us()
  if tmr and tmr.now then
    return tmr.now()
  end
  return math.floor(os.clock() * 1000000)
end

local function elapsed_us(t0, t1)
  local d = t1 - t0
  if d < 0 then
    d = d + 4294967296
  end
  return d
end

local function mix(acc, value)
  return (((acc << 5) ~ (acc >> 2) ~ value) & 65535)
end

local function crc_step(v)
  local crc = v & 255
  for _ = 1, 8 do
    if (crc & 1) ~= 0 then
      crc = ((crc >> 1) ~ 40961) & 65535
    else
      crc = (crc >> 1) & 65535
    end
  end
  return crc
end

local function crc_tab(tab, x)
  return (tab[x & 255] ~ tab[256 + ((x >> 8) & 255)]) & 65535
end

print("coremark-viper rounds", rounds)

local tab_buf = viper.buf(512 * 2)
local tab = {}
for i = 0, 511 do
  local v = crc_step(i)
  tab_buf:set16(i, v)
  tab[i] = v
end

local crc_buf = viper.buf(256)
local crc_data = {}
for i = 0, 255 do
  local v = ((i * 17) + 23) & 255
  crc_buf:set8(i, v)
  crc_data[i] = v
end

local state_seed = "-3.14,42,1e5,+7.0,abc,9.9e-2,-,12e+3,.5,77,0,xyz,"
local state_buf = viper.buf(137)
local state_data = {}
for i = 0, 136 do
  local v = state_seed:byte((i % #state_seed) + 1)
  state_buf:set8(i, v)
  state_data[i] = v
end

local a_buf = viper.buf(100 * 2)
local b_buf = viper.buf(100 * 2)
local c_buf = viper.buf(100 * 2)
local a = {}
local b = {}
local c = {}
for i = 0, 99 do
  local av = ((i * 37) + 11) & 65535
  local bv = ((i * 19) + 7) & 65535
  a_buf:set16(i, av)
  b_buf:set16(i, bv)
  c_buf:set16(i, 0)
  a[i] = av
  b[i] = bv
  c[i] = 0
end

local ldata_buf = viper.buf(72 * 2)
local lnext_buf = viper.buf(72 * 2)
local ldata = {}
local lnext = {}
for i = 0, 71 do
  local v = (((i * 53) + 91) ~ (i * 7)) & 65535
  ldata_buf:set16(i, v)
  lnext_buf:set16(i, i < 71 and (i + 1) or 65535)
  ldata[i] = v
  lnext[i] = i < 71 and (i + 1) or 65535
end

local t0 = now_us()
local v_crc = viper.compile_c(VIPER_CRC_SRC)
local v_matrix = viper.compile_c(VIPER_MATRIX_SRC)
local v_state = viper.compile_c(VIPER_STATE_SRC)
local v_list = viper.compile_c(VIPER_LIST_SRC)
local compile_us = elapsed_us(t0, now_us())
print("compile_us", compile_us)

local function lua_crc(buf, tab, iter)
  local crc = iter & 65535
  local add = (iter + 1) & 255
  for i = 0, 255 do
    local v = (buf[i] + add) & 255
    add = (add + 1) & 255
    crc = crc_tab(tab, crc ~ v)
  end
  return crc & 65535
end

local function lua_matrix(a, b, c, tab, iter)
  local crc = 0
  local kconst = (iter % 13) + 1
  for i = 0, 99 do
    local v = ((a[i] * kconst) + b[i]) & 65535
    c[i] = v
    crc = crc_tab(tab, crc ~ v)
  end
  for row = 0, 9 do
    local sum = 0
    local base = row * 10
    for col = 0, 9 do
      sum = (sum + (a[base + col] * b[col])) & 65535
    end
    crc = crc_tab(tab, crc ~ sum)
  end
  for row = 0, 9 do
    local base = row * 10
    for col = 0, 9 do
      local sum = 0
      local bidx = col
      for k = 0, 9 do
        sum = (sum + (a[base + k] * b[bidx])) & 65535
        bidx = bidx + 10
      end
      c[base + col] = sum
    end
  end
  for i = 0, 99, 3 do
    local v = c[i]
    local folded = ((((v >> 4) & 255) * 17) + (v & 15)) & 65535
    crc = crc_tab(tab, crc ~ folded)
  end
  return crc & 65535
end

local function lua_state(buf, tab, iter)
  local crc = iter & 65535
  local st, valid, invalid, trans = 0, 0, 0, 0
  local salt_mod = (iter % 31) + 17
  local salt_count = salt_mod
  local salt_xor = iter & 7
  for i = 0, 136 do
    local ch = buf[i]
    salt_count = salt_count - 1
    if salt_count == 0 then
      ch = ch ~ salt_xor
      salt_count = salt_mod
    end
    if ch == 44 then
      if st == 2 or st == 4 or st == 7 then
        valid = valid + 1
      else
        invalid = invalid + 1
      end
      local v = st + valid * 3 + invalid * 7
      crc = crc_tab(tab, crc ~ (v & 65535))
      st = 0
    else
      local digit = ch >= 48 and ch <= 57
      local sign = ch == 43 or ch == 45
      local dot = ch == 46
      local exp = ch == 69 or ch == 101
      local old = st
      if st == 0 then
        if sign then st = 1 elseif digit then st = 2 elseif dot then st = 3 else st = 8 end
      elseif st == 1 then
        if digit then st = 2 elseif dot then st = 3 else st = 8 end
      elseif st == 2 then
        if digit then st = 2 elseif dot then st = 4 elseif exp then st = 5 else st = 8 end
      elseif st == 3 then
        if digit then st = 4 else st = 8 end
      elseif st == 4 then
        if digit then st = 4 elseif exp then st = 5 else st = 8 end
      elseif st == 5 then
        if sign then st = 6 elseif digit then st = 7 else st = 8 end
      elseif st == 6 then
        if digit then st = 7 else st = 8 end
      elseif st == 7 then
        if digit then st = 7 else st = 8 end
      else
        st = 8
      end
      if st ~= old then
        trans = trans + 1
      end
    end
  end
  if st == 2 or st == 4 or st == 7 then
    valid = valid + 1
  else
    invalid = invalid + 1
  end
  crc = crc_tab(tab, crc ~ (valid & 65535))
  crc = crc_tab(tab, crc ~ (invalid & 65535))
  crc = crc_tab(tab, crc ~ (trans & 65535))
  return crc & 65535
end

local function lua_list(ldata, lnext, tab, iter)
  local crc = 0
  for i = 0, 7 do
    local key = ((iter + ((i + 1) * 7)) % 72) + 1
    local idx = 0
    local data = key
    for _ = 0, 71 do
      if idx ~= 65535 then
        if (idx + 1) == key then
          data = ldata[idx] + idx + 1
          idx = 65535
        else
          idx = lnext[idx]
        end
      end
    end
    crc = crc_tab(tab, crc ~ (data & 65535))
  end

  local sorted = 65535
  for i = 0, 71 do
    local cur = i
    local next_idx = sorted
    local prev = 65535
    for _ = 0, 71 do
      if next_idx ~= 65535 then
        if (ldata[cur] < ldata[next_idx]) or ((ldata[cur] == ldata[next_idx]) and (cur < next_idx)) then
          break
        else
          prev = next_idx
          next_idx = lnext[next_idx]
        end
      end
    end
    lnext[cur] = next_idx
    if prev == 65535 then
      sorted = cur
    else
      lnext[prev] = cur
    end
  end

  local idx = sorted
  local inner = 0
  for _ = 0, 71 do
    if idx ~= 65535 then
      inner = crc_tab(tab, inner ~ ((idx + 1) & 65535))
      inner = crc_tab(tab, inner ~ (ldata[idx] & 65535))
      idx = lnext[idx]
    end
  end
  crc = crc_tab(tab, crc ~ (inner & 65535))

  for i = 0, 70 do
    lnext[i] = i + 1
  end
  lnext[71] = 65535

  idx = 0
  inner = 0
  for _ = 0, 15 do
    inner = crc_tab(tab, inner ~ ((idx + 1) & 65535))
    inner = crc_tab(tab, inner ~ (ldata[idx] & 65535))
    idx = lnext[idx]
  end
  crc = crc_tab(tab, crc ~ (inner & 65535))
  return crc & 65535
end

local function run_viper(n)
  local acc = 0
  for iter = 0, n - 1 do
    acc = mix(acc, v_crc(crc_buf, tab_buf, iter))
    acc = mix(acc, v_matrix(a_buf, b_buf, c_buf, tab_buf, iter))
    acc = mix(acc, v_state(state_buf, tab_buf, iter))
    acc = mix(acc, v_list(ldata_buf, lnext_buf, tab_buf, iter))
  end
  return acc
end

local function run_lua(n)
  local acc = 0
  for iter = 0, n - 1 do
    acc = mix(acc, lua_crc(crc_data, tab, iter))
    acc = mix(acc, lua_matrix(a, b, c, tab, iter))
    acc = mix(acc, lua_state(state_data, tab, iter))
    acc = mix(acc, lua_list(ldata, lnext, tab, iter))
  end
  return acc
end

local function bench(name, fn)
  collectgarbage()
  local start = now_us()
  local checksum = fn(rounds)
  local usec = elapsed_us(start, now_us())
  local per_round = math.floor(usec / rounds)
  print(name, "checksum", checksum, "us", usec, "us_per_round", per_round)
  return checksum, usec
end

run_viper(1)
run_lua(1)

local viper_checksum, viper_us = bench("viper", run_viper)
local lua_checksum, lua_us = bench("lua", run_lua)

if viper_checksum ~= lua_checksum then
  print("checksum_mismatch", viper_checksum, lua_checksum)
else
  print("checksum_ok", viper_checksum)
end

if viper_us > 0 then
  local scaled = math.floor((lua_us * 100) / viper_us)
  print("speedup", string.format("%d.%02dx", math.floor(scaled / 100), scaled % 100))
end
