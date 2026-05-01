#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Xtensa windowed ABI register numbers.
#define ASM_XTENSA_REG_A0  (0)
#define ASM_XTENSA_REG_A1  (1)
#define ASM_XTENSA_REG_A2  (2)
#define ASM_XTENSA_REG_A3  (3)
#define ASM_XTENSA_REG_A4  (4)
#define ASM_XTENSA_REG_A5  (5)
#define ASM_XTENSA_REG_A6  (6)
#define ASM_XTENSA_REG_A7  (7)
#define ASM_XTENSA_REG_A8  (8)
#define ASM_XTENSA_REG_A9  (9)
#define ASM_XTENSA_REG_A10 (10)
#define ASM_XTENSA_REG_A11 (11)
#define ASM_XTENSA_REG_A12 (12)
#define ASM_XTENSA_REG_A13 (13)
#define ASM_XTENSA_REG_A14 (14)
#define ASM_XTENSA_REG_A15 (15)

// bccz conditions.
#define ASM_XTENSA_CCZ_EQ (0)
#define ASM_XTENSA_CCZ_NE (1)
#define ASM_XTENSA_CCZ_LT (2)
#define ASM_XTENSA_CCZ_GE (3)

// bcc/setcc conditions.
#define ASM_XTENSA_CC_NONE  (0)
#define ASM_XTENSA_CC_EQ    (1)
#define ASM_XTENSA_CC_LT    (2)
#define ASM_XTENSA_CC_LTU   (3)
#define ASM_XTENSA_CC_ALL   (4)
#define ASM_XTENSA_CC_BC    (5)
#define ASM_XTENSA_CC_ANY   (8)
#define ASM_XTENSA_CC_NE    (9)
#define ASM_XTENSA_CC_GE    (10)
#define ASM_XTENSA_CC_GEU   (11)
#define ASM_XTENSA_CC_NALL  (12)
#define ASM_XTENSA_CC_BS    (13)

#define ASM_XTENSA_LABEL_UNSET UINT32_MAX
#define ASM_XTENSA_WINDOWED_ABI_SAVE_WORDS (4U)
#define ASM_XTENSA_WINDOWED_ABI_SAVE_BYTES (ASM_XTENSA_WINDOWED_ABI_SAVE_WORDS * 4U)

#define ASM_XTENSA_ENCODE_RRR(op0, op1, op2, r, s, t) \
    ((((uint32_t)(op2)) << 20) | (((uint32_t)(op1)) << 16) | ((uint32_t)(r) << 12) | \
     ((uint32_t)(s) << 8) | ((uint32_t)(t) << 4) | (uint32_t)(op0))
#define ASM_XTENSA_ENCODE_RRI4(op0, op1, r, s, t, imm4) \
    (((uint32_t)(imm4) << 20) | ((uint32_t)(op1) << 16) | ((uint32_t)(r) << 12) | \
     ((uint32_t)(s) << 8) | ((uint32_t)(t) << 4) | (uint32_t)(op0))
#define ASM_XTENSA_ENCODE_RRI8(op0, r, s, t, imm8) \
    ((((uint32_t)(imm8)) << 16) | ((uint32_t)(r) << 12) | ((uint32_t)(s) << 8) | \
     ((uint32_t)(t) << 4) | (uint32_t)(op0))
#define ASM_XTENSA_ENCODE_RI16(op0, t, imm16) \
    (((uint32_t)(imm16) << 8) | ((uint32_t)(t) << 4) | (uint32_t)(op0))
#define ASM_XTENSA_ENCODE_CALL(op0, n, offset) \
    (((uint32_t)(offset) << 6) | ((uint32_t)(n) << 4) | (uint32_t)(op0))
#define ASM_XTENSA_ENCODE_CALLX(op0, op1, op2, r, s, m, n) \
    ((((uint32_t)(op2)) << 20) | (((uint32_t)(op1)) << 16) | ((uint32_t)(r) << 12) | \
     ((uint32_t)(s) << 8) | ((uint32_t)(m) << 6) | ((uint32_t)(n) << 4) | (uint32_t)(op0))
#define ASM_XTENSA_ENCODE_BRI12(op0, s, m, n, imm12) \
    (((uint32_t)(imm12) << 12) | ((uint32_t)(s) << 8) | ((uint32_t)(m) << 6) | \
     ((uint32_t)(n) << 4) | (uint32_t)(op0))
#define ASM_XTENSA_ENCODE_RRRN(op0, r, s, t) \
    (((uint32_t)(r) << 12) | ((uint32_t)(s) << 8) | ((uint32_t)(t) << 4) | (uint32_t)(op0))
#define ASM_XTENSA_ENCODE_RI7(op0, s, imm7) \
    ((((uint32_t)(imm7) & 0xfU) << 12) | ((uint32_t)(s) << 8) | \
     ((uint32_t)(imm7) & 0x70U) | (uint32_t)(op0))

typedef struct asm_xtensa_t
{
    uint8_t *code;
    size_t capacity;
    size_t offset;
    uint32_t entry_offset;
    uint32_t *label_offsets;
    uint32_t max_num_labels;
    uint32_t *literal_values;
    uint32_t max_num_literals;
    uint32_t literal_count;
    uint32_t literal_table_offset;
    bool literal_collect;
    uint8_t *branch_long;
    uint32_t max_num_branches;
    uint32_t branch_count;
    bool branch_changed;
    bool emit;
    const char *error;
} asm_xtensa_t;

void asm_xtensa_init(asm_xtensa_t *as, uint8_t *code, size_t capacity,
                     uint32_t *label_offsets, uint32_t max_num_labels,
                     uint32_t *literal_values, uint32_t max_num_literals,
                     uint32_t literal_count, bool literal_collect,
                     uint8_t *branch_long, uint32_t max_num_branches,
                     bool reset_labels, bool emit);
void asm_xtensa_set_label(asm_xtensa_t *as, uint32_t label);
size_t asm_xtensa_get_offset(const asm_xtensa_t *as);
uint32_t asm_xtensa_get_entry_offset(const asm_xtensa_t *as);

void asm_xtensa_op16(asm_xtensa_t *as, uint16_t op);
void asm_xtensa_op24(asm_xtensa_t *as, uint32_t op);
void asm_xtensa_entry_win(asm_xtensa_t *as, int num_locals);
void asm_xtensa_exit_win(asm_xtensa_t *as);
void asm_xtensa_j_label(asm_xtensa_t *as, uint32_t label);
void asm_xtensa_bccz_reg_label(asm_xtensa_t *as, unsigned cond, unsigned reg, uint32_t label);
void asm_xtensa_bcc_reg_reg_label(asm_xtensa_t *as, unsigned cond, unsigned reg1, unsigned reg2, uint32_t label);
void asm_xtensa_setcc_reg_reg_reg(asm_xtensa_t *as, unsigned cond, unsigned reg_dest,
                                  unsigned reg_src1, unsigned reg_src2);
void asm_xtensa_mov_reg_i32_optimised(asm_xtensa_t *as, unsigned reg_dest, uint32_t value);
void asm_xtensa_mov_reg_i32_optimised_scratch(asm_xtensa_t *as, unsigned reg_dest,
                                              uint32_t value, unsigned reg_scratch);
void asm_xtensa_load_reg_reg_offset(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_base,
                                    unsigned offset, unsigned operation_size);
void asm_xtensa_store_reg_reg_offset(asm_xtensa_t *as, unsigned reg_src, unsigned reg_base,
                                     unsigned offset, unsigned operation_size);

static inline void asm_xtensa_op_entry(asm_xtensa_t *as, unsigned reg_src, int32_t num_bytes)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_BRI12(6, reg_src, 0, 3, (num_bytes / 8) & 0xfff));
}

static inline void asm_xtensa_op_extui(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src,
                                       unsigned start_bit, unsigned bit_count)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRR(0, 4 | ((start_bit >> 4) & 1U),
                                              (bit_count - 1U) & 0xfU, reg_dest,
                                              start_bit & 0xfU, reg_src));
}

static inline void asm_xtensa_op_add_n(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src_a, unsigned reg_src_b)
{
    asm_xtensa_op16(as, ASM_XTENSA_ENCODE_RRRN(10, reg_dest, reg_src_a, reg_src_b));
}

static inline void asm_xtensa_op_addi(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src, int imm8)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRI8(2, 12, reg_src, reg_dest, imm8 & 0xff));
}

static inline void asm_xtensa_op_addx2(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src_a, unsigned reg_src_b)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRR(0, 0, 9, reg_dest, reg_src_a, reg_src_b));
}

static inline void asm_xtensa_op_addx4(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src_a, unsigned reg_src_b)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRR(0, 0, 10, reg_dest, reg_src_a, reg_src_b));
}

static inline void asm_xtensa_op_and(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src_a, unsigned reg_src_b)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRR(0, 0, 1, reg_dest, reg_src_a, reg_src_b));
}

static inline void asm_xtensa_op_bcc(asm_xtensa_t *as, unsigned cond, unsigned reg_src1, unsigned reg_src2, int32_t rel8)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRI8(7, cond, reg_src1, reg_src2, rel8 & 0xff));
}

static inline void asm_xtensa_op_bccz(asm_xtensa_t *as, unsigned cond, unsigned reg_src, int32_t rel12)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_BRI12(6, reg_src, cond, 1, rel12 & 0xfff));
}

static inline void asm_xtensa_op_j(asm_xtensa_t *as, int32_t rel18)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_CALL(6, 0, rel18 & 0x3ffff));
}

static inline void asm_xtensa_op_l8ui(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_base, unsigned byte_offset)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRI8(2, 0, reg_base, reg_dest, byte_offset & 0xff));
}

static inline void asm_xtensa_op_l16ui(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_base, unsigned half_word_offset)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRI8(2, 1, reg_base, reg_dest, half_word_offset & 0xff));
}

static inline void asm_xtensa_op_l32i(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_base, unsigned word_offset)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRI8(2, 2, reg_base, reg_dest, word_offset & 0xff));
}

static inline void asm_xtensa_op_l32i_n(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_base, unsigned word_offset)
{
    asm_xtensa_op16(as, ASM_XTENSA_ENCODE_RRRN(8, word_offset & 0xf, reg_base, reg_dest));
}

static inline void asm_xtensa_op_l32r(asm_xtensa_t *as, unsigned reg_dest, uint32_t op_off, uint32_t dest_off)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RI16(1, reg_dest, ((dest_off - ((op_off + 3) & ~3U)) >> 2) & 0xffff));
}

static inline void asm_xtensa_op_mov_n(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src)
{
    asm_xtensa_op16(as, ASM_XTENSA_ENCODE_RRRN(13, 0, reg_src, reg_dest));
}

static inline void asm_xtensa_op_movi(asm_xtensa_t *as, unsigned reg_dest, int32_t imm12)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRI8(2, 10, (imm12 >> 8) & 0xf, reg_dest, imm12 & 0xff));
}

static inline void asm_xtensa_op_movi_n(asm_xtensa_t *as, unsigned reg_dest, int imm7)
{
    asm_xtensa_op16(as, ASM_XTENSA_ENCODE_RI7(12, reg_dest, imm7));
}

static inline void asm_xtensa_op_mull(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src_a, unsigned reg_src_b)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRR(0, 2, 8, reg_dest, reg_src_a, reg_src_b));
}

static inline void asm_xtensa_op_neg(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRR(0, 0, 6, reg_dest, 0, reg_src));
}

static inline void asm_xtensa_op_or(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src_a, unsigned reg_src_b)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRR(0, 0, 2, reg_dest, reg_src_a, reg_src_b));
}

static inline void asm_xtensa_op_quos(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src_a, unsigned reg_src_b)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRR(0, 2, 13, reg_dest, reg_src_a, reg_src_b));
}

static inline void asm_xtensa_op_quou(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src_a, unsigned reg_src_b)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRR(0, 2, 12, reg_dest, reg_src_a, reg_src_b));
}

static inline void asm_xtensa_op_rems(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src_a, unsigned reg_src_b)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRR(0, 2, 15, reg_dest, reg_src_a, reg_src_b));
}

static inline void asm_xtensa_op_remu(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src_a, unsigned reg_src_b)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRR(0, 2, 14, reg_dest, reg_src_a, reg_src_b));
}

static inline void asm_xtensa_op_retw_n(asm_xtensa_t *as)
{
    asm_xtensa_op16(as, ASM_XTENSA_ENCODE_RRRN(13, 15, 0, 1));
}

static inline void asm_xtensa_op_s8i(asm_xtensa_t *as, unsigned reg_src, unsigned reg_base, unsigned byte_offset)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRI8(2, 4, reg_base, reg_src, byte_offset & 0xff));
}

static inline void asm_xtensa_op_s16i(asm_xtensa_t *as, unsigned reg_src, unsigned reg_base, unsigned half_word_offset)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRI8(2, 5, reg_base, reg_src, half_word_offset & 0xff));
}

static inline void asm_xtensa_op_s32i(asm_xtensa_t *as, unsigned reg_src, unsigned reg_base, unsigned word_offset)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRI8(2, 6, reg_base, reg_src, word_offset & 0xff));
}

static inline void asm_xtensa_op_s32i_n(asm_xtensa_t *as, unsigned reg_src, unsigned reg_base, unsigned word_offset)
{
    asm_xtensa_op16(as, ASM_XTENSA_ENCODE_RRRN(9, word_offset & 0xf, reg_base, reg_src));
}

static inline void asm_xtensa_op_sll(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRR(0, 1, 10, reg_dest, reg_src, 0));
}

static inline void asm_xtensa_op_srl(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRR(0, 1, 9, reg_dest, 0, reg_src));
}

static inline void asm_xtensa_op_sra(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRR(0, 1, 11, reg_dest, 0, reg_src));
}

static inline void asm_xtensa_op_ssl(asm_xtensa_t *as, unsigned reg_src)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRR(0, 0, 4, 1, reg_src, 0));
}

static inline void asm_xtensa_op_ssr(asm_xtensa_t *as, unsigned reg_src)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRR(0, 0, 4, 0, reg_src, 0));
}

static inline void asm_xtensa_op_sub(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src_a, unsigned reg_src_b)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRR(0, 0, 12, reg_dest, reg_src_a, reg_src_b));
}

static inline void asm_xtensa_op_xor(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_src_a, unsigned reg_src_b)
{
    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRR(0, 0, 3, reg_dest, reg_src_a, reg_src_b));
}

#ifdef __cplusplus
}
#endif
