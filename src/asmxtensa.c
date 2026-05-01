#include "asmxtensa.h"

#define WORD_SIZE (4U)

static bool signed_fit_bits(int32_t value, unsigned bits)
{
    const int32_t min_value = -(1 << (bits - 1));
    const int32_t max_value = (1 << (bits - 1)) - 1;
    return value >= min_value && value <= max_value;
}

static bool unsigned_fit_bits(uint32_t value, unsigned bits)
{
    return bits >= 32 || value < (1U << bits);
}

static uint32_t get_label_dest(asm_xtensa_t *as, uint32_t label)
{
    if (!as || !as->label_offsets || label >= as->max_num_labels)
    {
        if (as)
            as->error = "asm_xtensa: invalid label";
        return ASM_XTENSA_LABEL_UNSET;
    }
    return as->label_offsets[label];
}

static void set_error(asm_xtensa_t *as, const char *msg)
{
    if (as && !as->error)
        as->error = msg;
}

void asm_xtensa_init(asm_xtensa_t *as, uint8_t *code, size_t capacity,
                     uint32_t *label_offsets, uint32_t max_num_labels,
                     uint32_t *literal_values, uint32_t max_num_literals,
                     uint32_t literal_count, bool literal_collect,
                     uint8_t *branch_long, uint32_t max_num_branches,
                     bool reset_labels, bool emit)
{
    if (!as)
        return;
    as->code = code;
    as->capacity = capacity;
    as->offset = 0;
    as->entry_offset = 0;
    as->label_offsets = label_offsets;
    as->max_num_labels = max_num_labels;
    as->literal_values = literal_values;
    as->max_num_literals = max_num_literals;
    as->literal_count = literal_count;
    as->literal_table_offset = ASM_XTENSA_LABEL_UNSET;
    as->literal_collect = literal_collect;
    as->branch_long = branch_long;
    as->max_num_branches = max_num_branches;
    as->branch_count = 0;
    as->branch_changed = false;
    as->emit = emit;
    as->error = NULL;
    if (label_offsets && reset_labels)
    {
        for (uint32_t i = 0; i < max_num_labels; ++i)
            label_offsets[i] = ASM_XTENSA_LABEL_UNSET;
    }
}

void asm_xtensa_set_label(asm_xtensa_t *as, uint32_t label)
{
    if (!as || !as->label_offsets || label >= as->max_num_labels)
    {
        set_error(as, "asm_xtensa: invalid label");
        return;
    }
    as->label_offsets[label] = (uint32_t)as->offset;
}

size_t asm_xtensa_get_offset(const asm_xtensa_t *as)
{
    return as ? as->offset : 0;
}

uint32_t asm_xtensa_get_entry_offset(const asm_xtensa_t *as)
{
    return as ? as->entry_offset : 0;
}

void asm_xtensa_op16(asm_xtensa_t *as, uint16_t op)
{
    if (!as)
        return;
    if (as->emit)
    {
        if (!as->code || as->offset + 2 > as->capacity)
        {
            set_error(as, "asm_xtensa: code buffer overflow");
        }
        else
        {
            as->code[as->offset] = (uint8_t)op;
            as->code[as->offset + 1] = (uint8_t)(op >> 8);
        }
    }
    as->offset += 2;
}

void asm_xtensa_op24(asm_xtensa_t *as, uint32_t op)
{
    if (!as)
        return;
    if (as->emit)
    {
        if (!as->code || as->offset + 3 > as->capacity)
        {
            set_error(as, "asm_xtensa: code buffer overflow");
        }
        else
        {
            as->code[as->offset] = (uint8_t)op;
            as->code[as->offset + 1] = (uint8_t)(op >> 8);
            as->code[as->offset + 2] = (uint8_t)(op >> 16);
        }
    }
    as->offset += 3;
}

/**
 * @brief Write a 32-bit data word in little-endian order; the data is only read by l32r, not executed as an instruction.
 */
static void asm_xtensa_op32_data(asm_xtensa_t *as, uint32_t value)
{
    if (!as)
        return;
    if (as->emit)
    {
        if (!as->code || as->offset + 4 > as->capacity)
        {
            set_error(as, "asm_xtensa: code buffer overflow");
        }
        else
        {
            as->code[as->offset] = (uint8_t)value;
            as->code[as->offset + 1] = (uint8_t)(value >> 8);
            as->code[as->offset + 2] = (uint8_t)(value >> 16);
            as->code[as->offset + 3] = (uint8_t)(value >> 24);
        }
    }
    as->offset += 4;
}

void asm_xtensa_entry_win(asm_xtensa_t *as, int num_locals)
{
    if (as && as->literal_count > 0)
    {
        as->literal_table_offset = (uint32_t)as->offset;
        for (uint32_t i = 0; i < as->literal_count; ++i)
        {
            const uint32_t value = as->literal_values ? as->literal_values[i] : 0;
            asm_xtensa_op32_data(as, value);
        }
    }

    if (as)
        as->entry_offset = (uint32_t)as->offset;

    const uint32_t local_words = num_locals > 0 ? (uint32_t)num_locals : 0;
    const uint32_t local_bytes = (local_words * WORD_SIZE + 15U) & ~15U;
    const uint32_t stack_adjust = ASM_XTENSA_WINDOWED_ABI_SAVE_BYTES + local_bytes;
    asm_xtensa_op_entry(as, ASM_XTENSA_REG_A1, (int32_t)stack_adjust);
}

void asm_xtensa_exit_win(asm_xtensa_t *as)
{
    asm_xtensa_op_retw_n(as);
}

void asm_xtensa_j_label(asm_xtensa_t *as, uint32_t label)
{
    const uint32_t dest = get_label_dest(as, label);
    int32_t rel = 0;
    if (dest != ASM_XTENSA_LABEL_UNSET)
        rel = (int32_t)dest - (int32_t)as->offset - 4;
    if (as && as->emit && (dest == ASM_XTENSA_LABEL_UNSET || !signed_fit_bits(rel, 18)))
        set_error(as, "asm_xtensa: jump out of range");
    asm_xtensa_op_j(as, rel);
}

static bool calculate_branch_displacement(asm_xtensa_t *as, uint32_t label, int32_t *displacement)
{
    const uint32_t label_offset = get_label_dest(as, label);
    *displacement = 0;
    if (label_offset == ASM_XTENSA_LABEL_UNSET)
        return false;
    *displacement = (int32_t)label_offset - (int32_t)as->offset - 4;
    return true;
}

/**
 * @brief Return the stable index of the current conditional branch in the relaxation table.
 */
static uint32_t next_branch_index(asm_xtensa_t *as)
{
    if (!as)
        return UINT32_MAX;
    const uint32_t index = as->branch_count;
    if (as->branch_count != UINT32_MAX)
        ++as->branch_count;
    return index;
}

/**
 * @brief Whether the current pass enables the conditional-branch sizing decision table.
 */
static bool has_branch_relaxation(const asm_xtensa_t *as)
{
    return as && as->branch_long && as->max_num_branches > 0;
}

/**
 * @brief Query whether the previous sizing pass determined that this branch must use a long jump.
 */
static bool is_forced_long_branch(asm_xtensa_t *as, uint32_t branch_index)
{
    if (!has_branch_relaxation(as))
        return false;
    if (branch_index >= as->max_num_branches)
    {
        set_error(as, "asm_xtensa: too many branches");
        return true;
    }
    return as->branch_long[branch_index] != 0;
}

/**
 * @brief Upgrade a short branch to a long jump and notify the sizing pass to keep iterating.
 */
static void force_long_branch(asm_xtensa_t *as, uint32_t branch_index)
{
    if (!has_branch_relaxation(as))
        return;
    if (branch_index >= as->max_num_branches)
    {
        set_error(as, "asm_xtensa: too many branches");
        return;
    }
    if (as->branch_long[branch_index] == 0)
    {
        as->branch_long[branch_index] = 1;
        as->branch_changed = true;
    }
}

/**
 * @brief Emit the inverted bccz conditional branch plus fallback j long-jump sequence.
 */
static void emit_long_bccz(asm_xtensa_t *as, unsigned cond, unsigned reg, uint32_t label, int32_t rel)
{
    const uint32_t dest = get_label_dest(as, label);
    if (dest != ASM_XTENSA_LABEL_UNSET)
        rel = (int32_t)dest - (int32_t)as->offset - 4;
    if (as && as->emit && (dest == ASM_XTENSA_LABEL_UNSET || !signed_fit_bits(rel - 3, 18)))
        set_error(as, "asm_xtensa: bccz out of range");

    asm_xtensa_op_bccz(as, cond ^ 1U, reg, 6 - 4);
    asm_xtensa_op_j(as, rel - 3);
}

/**
 * @brief Emit the inverted bcc conditional branch plus fallback j long-jump sequence.
 */
static void emit_long_bcc(asm_xtensa_t *as, unsigned cond, unsigned reg1, unsigned reg2,
                          uint32_t label, int32_t rel)
{
    const uint32_t dest = get_label_dest(as, label);
    if (dest != ASM_XTENSA_LABEL_UNSET)
        rel = (int32_t)dest - (int32_t)as->offset - 4;
    if (as && as->emit && (dest == ASM_XTENSA_LABEL_UNSET || !signed_fit_bits(rel - 3, 18)))
        set_error(as, "asm_xtensa: bcc out of range");

    asm_xtensa_op_bcc(as, cond ^ 8U, reg1, reg2, 6 - 4);
    asm_xtensa_op_j(as, rel - 3);
}

void asm_xtensa_bccz_reg_label(asm_xtensa_t *as, unsigned cond, unsigned reg, uint32_t label)
{
    int32_t rel = 0;
    const uint32_t branch_index = next_branch_index(as);
    const bool has_rel = calculate_branch_displacement(as, label, &rel);
    const bool forced_long = is_forced_long_branch(as, branch_index);

    if (!forced_long)
    {
        if (has_rel && signed_fit_bits(rel, 12))
        {
            asm_xtensa_op_bccz(as, cond, reg, rel);
            return;
        }
        if (!has_rel && has_branch_relaxation(as) && !as->emit)
        {
            asm_xtensa_op_bccz(as, cond, reg, 0);
            return;
        }
        if (has_rel && as && as->emit && has_branch_relaxation(as))
            set_error(as, "asm_xtensa: branch relaxation unstable");
        if (has_branch_relaxation(as))
            force_long_branch(as, branch_index);
    }

    emit_long_bccz(as, cond, reg, label, rel);
}

void asm_xtensa_bcc_reg_reg_label(asm_xtensa_t *as, unsigned cond, unsigned reg1, unsigned reg2, uint32_t label)
{
    int32_t rel = 0;
    const uint32_t branch_index = next_branch_index(as);
    const bool has_rel = calculate_branch_displacement(as, label, &rel);
    const bool forced_long = is_forced_long_branch(as, branch_index);

    if (!forced_long)
    {
        if (has_rel && signed_fit_bits(rel, 8))
        {
            asm_xtensa_op_bcc(as, cond, reg1, reg2, rel);
            return;
        }
        if (!has_rel && has_branch_relaxation(as) && !as->emit)
        {
            asm_xtensa_op_bcc(as, cond, reg1, reg2, 0);
            return;
        }
        if (has_rel && as && as->emit && has_branch_relaxation(as))
            set_error(as, "asm_xtensa: branch relaxation unstable");
        if (has_branch_relaxation(as))
            force_long_branch(as, branch_index);
    }

    emit_long_bcc(as, cond, reg1, reg2, label, rel);
}

void asm_xtensa_setcc_reg_reg_reg(asm_xtensa_t *as, unsigned cond, unsigned reg_dest,
                                  unsigned reg_src1, unsigned reg_src2)
{
    asm_xtensa_op_movi_n(as, reg_dest, 1);
    asm_xtensa_op_bcc(as, cond, reg_src1, reg_src2, 1);
    asm_xtensa_op_movi_n(as, reg_dest, 0);
}

/**
 * @brief Return the literal index in the constant pool; the collection pass deduplicates and appends automatically.
 */
static uint32_t asm_xtensa_literal_index(asm_xtensa_t *as, uint32_t value)
{
    if (!as || !as->literal_values || as->max_num_literals == 0)
    {
        set_error(as, "asm_xtensa: literal pool is not configured");
        return ASM_XTENSA_LABEL_UNSET;
    }

    for (uint32_t i = 0; i < as->literal_count; ++i)
    {
        if (as->literal_values[i] == value)
            return i;
    }

    if (!as->literal_collect)
    {
        set_error(as, "asm_xtensa: literal missing from pool");
        return ASM_XTENSA_LABEL_UNSET;
    }

    if (as->literal_count >= as->max_num_literals)
    {
        set_error(as, "asm_xtensa: too many literals");
        return ASM_XTENSA_LABEL_UNSET;
    }

    const uint32_t index = as->literal_count++;
    as->literal_values[index] = value;
    return index;
}

/**
 * @brief Load a 32-bit constant from the function-entry literal pool using l32r.
 */
static void asm_xtensa_mov_reg_i32_literal(asm_xtensa_t *as, unsigned reg_dest, uint32_t value)
{
    const uint32_t index = asm_xtensa_literal_index(as, value);
    if (index == ASM_XTENSA_LABEL_UNSET)
        return;

    if (as->literal_table_offset != ASM_XTENSA_LABEL_UNSET)
    {
        const uint32_t dest = as->literal_table_offset + index * WORD_SIZE;
        const uint32_t pc = ((uint32_t)as->offset + 3U) & ~3U;
        const int32_t rel = (int32_t)dest - (int32_t)pc;
        if ((dest & 3U) != 0 || rel >= 0 || !signed_fit_bits(rel, 18))
            set_error(as, "asm_xtensa: l32r literal out of range");
        asm_xtensa_op_l32r(as, reg_dest, (uint32_t)as->offset, dest);
        return;
    }

    asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RI16(1, reg_dest, 0));
}

void asm_xtensa_mov_reg_i32_optimised(asm_xtensa_t *as, unsigned reg_dest, uint32_t value)
{
    const unsigned temp = reg_dest == ASM_XTENSA_REG_A8 ? ASM_XTENSA_REG_A9 : ASM_XTENSA_REG_A8;
    asm_xtensa_mov_reg_i32_optimised_scratch(as, reg_dest, value, temp);
}

void asm_xtensa_mov_reg_i32_optimised_scratch(asm_xtensa_t *as, unsigned reg_dest,
                                              uint32_t value, unsigned reg_scratch)
{
    const int32_t signed_value = (int32_t)value;
    if (-32 <= signed_value && signed_value <= 95)
    {
        asm_xtensa_op_movi_n(as, reg_dest, signed_value);
        return;
    }
    if (signed_fit_bits(signed_value, 12))
    {
        asm_xtensa_op_movi(as, reg_dest, signed_value);
        return;
    }

    (void)reg_scratch;
    asm_xtensa_mov_reg_i32_literal(as, reg_dest, value);
}

void asm_xtensa_load_reg_reg_offset(asm_xtensa_t *as, unsigned reg_dest, unsigned reg_base,
                                    unsigned offset, unsigned operation_size)
{
    if (operation_size > 2)
    {
        set_error(as, "asm_xtensa: invalid load size");
        return;
    }
    if (operation_size == 2 && unsigned_fit_bits(offset, 4))
    {
        asm_xtensa_op_l32i_n(as, reg_dest, reg_base, offset);
        return;
    }
    if (unsigned_fit_bits(offset, 8))
    {
        asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRI8(2, operation_size, reg_base, reg_dest, offset));
        return;
    }

    asm_xtensa_mov_reg_i32_optimised(as, reg_dest, offset << operation_size);
    asm_xtensa_op_add_n(as, reg_dest, reg_base, reg_dest);
    if (operation_size == 2)
        asm_xtensa_op_l32i_n(as, reg_dest, reg_dest, 0);
    else
        asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRI8(2, operation_size, reg_dest, reg_dest, 0));
}

void asm_xtensa_store_reg_reg_offset(asm_xtensa_t *as, unsigned reg_src, unsigned reg_base,
                                     unsigned offset, unsigned operation_size)
{
    if (operation_size > 2)
    {
        set_error(as, "asm_xtensa: invalid store size");
        return;
    }
    if (operation_size == 2 && unsigned_fit_bits(offset, 4))
    {
        asm_xtensa_op_s32i_n(as, reg_src, reg_base, offset);
        return;
    }
    if (unsigned_fit_bits(offset, 8))
    {
        asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRI8(2, 0x04 | operation_size, reg_base, reg_src, offset));
        return;
    }

    const unsigned temp = reg_src == ASM_XTENSA_REG_A8 ? ASM_XTENSA_REG_A9 : ASM_XTENSA_REG_A8;
    asm_xtensa_mov_reg_i32_optimised(as, temp, offset << operation_size);
    asm_xtensa_op_add_n(as, temp, reg_base, temp);
    if (operation_size == 2)
        asm_xtensa_op_s32i_n(as, reg_src, temp, 0);
    else
        asm_xtensa_op24(as, ASM_XTENSA_ENCODE_RRI8(2, 0x04 | operation_size, temp, reg_src, 0));
}
