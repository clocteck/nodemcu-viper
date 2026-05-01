#include "viper_internal.h"

#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <xtensa/hal.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

#include "asmxtensa.h"

namespace lua_nodemcu_viper::internal
{

/**
 * @brief Allocate a 32-bit word array for the native local-variable frame to avoid consuming the FreeRTOS task stack.
 * @param words Number of 32-bit words required.
 * @return uint32_t* Pointer to the allocated data; returns nullptr when words is 0.
 */
static uint32_t *alloc_viper_native_frame(size_t words)
{
    if (words == 0)
        return nullptr;

    const size_t bytes = words * sizeof(uint32_t);
    uint32_t caps = bytes < kViperBufferInternalLimitBytes ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                                                           : (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t *ptr = (uint32_t *)heap_caps_calloc(words, sizeof(uint32_t), caps);
    if (!ptr && bytes < kViperBufferInternalLimitBytes)
        ptr = (uint32_t *)heap_caps_calloc(words, sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr && bytes >= kViperBufferInternalLimitBytes)
        ptr = (uint32_t *)heap_caps_calloc(words, sizeof(uint32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return ptr;
}

/**
 * @brief Release native code and heap local frame held by a compiled function.
 * @param fn Compiled function object.
 */
void release_native_storage(CompiledFunction &fn)
{
    if (fn.native_code)
    {
        heap_caps_free(fn.native_code);
        fn.native_code = nullptr;
    }
    if (fn.native_frame)
    {
        heap_caps_free(fn.native_frame);
        fn.native_frame = nullptr;
    }
    fn.native_size = 0;
    fn.native_entry = nullptr;
    fn.native_frame_words = 0;
    ViperInternalVector<uint32_t>().swap(fn.native_local_words);
}

static bool native_type_supported(ValueType type)
{
    return type != ValueType::F32 && type != ValueType::PtrF32;
}

/**
 * @brief Determine whether a comparison should use unsigned semantics.
 */
static bool native_unsigned_compare(ValueType a, ValueType b)
{
    return a == ValueType::U32 || b == ValueType::U32 ||
           is_ptr_type(a) || is_ptr_type(b);
}

/**
 * @brief Return why the native backend does not support an IR operation yet, or nullptr when supported.
 */
static const char *native_unsupported_opcode_reason(OpCode op)
{
    switch (op)
    {
    case OpCode::AddF:
    case OpCode::SubF:
    case OpCode::MulF:
    case OpCode::DivF:
    case OpCode::NegF:
    case OpCode::EqF:
    case OpCode::NeF:
    case OpCode::LtF:
    case OpCode::LeF:
    case OpCode::GtF:
    case OpCode::GeF:
    case OpCode::LoadF32:
    case OpCode::StoreF32:
        return "viper native: f32 operations are not supported yet";
    default:
        return nullptr;
    }
}

/**
 * @brief Convert an executable IRAM pointer to its writable DRAM alias.
 */
static uint8_t *native_code_write_alias(uint8_t *exec)
{
    if (esp_ptr_in_diram_iram(exec))
        return (uint8_t *)esp_ptr_diram_iram_to_dram(exec);
    return exec;
}

/**
 * @brief Write generated machine code into executable memory.
 *
 * The ESP32-S3 executable heap may be non-byte-accessible IRAM; in that case
 * bytes cannot be written with memcpy and must be written as 32-bit words.
 * On little-endian Xtensa, word writes preserve the code buffer byte order and
 * do not change final execution performance.
 */
static bool copy_native_code_to_exec(uint8_t *exec, size_t alloc_size,
                                     const ViperInternalVector<uint8_t> &code,
                                     ViperError &err)
{
    uint8_t *write_ptr = native_code_write_alias(exec);
    if (esp_ptr_byte_accessible(write_ptr))
    {
        std::memcpy(write_ptr, code.data(), code.size());
        if (alloc_size > code.size())
            std::memset(write_ptr + code.size(), 0, alloc_size - code.size());
        return true;
    }

    if (((uintptr_t)exec & 3U) != 0)
    {
        err.set("viper native: executable memory is not word aligned");
        return false;
    }

    volatile uint32_t *dst = (volatile uint32_t *)exec;
    const size_t word_count = alloc_size / 4U;
    for (size_t wi = 0; wi < word_count; ++wi)
    {
        uint32_t word = 0;
        const size_t base = wi * 4U;
        for (size_t bi = 0; bi < 4U; ++bi)
        {
            const size_t si = base + bi;
            if (si < code.size())
                word |= ((uint32_t)code[si]) << (bi * 8U);
        }
        dst[wi] = word;
    }
    return true;
}

/**
 * @brief Install generated machine code and flush the instruction cache.
 *
 * native_code stores the releasable executable allocation base; native_entry points to the real entry.
 * This allows the literal pool to live at the function start while entry code begins after the pool.
 */
static bool install_native_code(CompiledFunction &fn, const ViperInternalVector<uint8_t> &code,
                                uint32_t entry_offset, ViperError &err)
{
    if (code.empty() || code.size() > kMaxNativeCodeBytes || entry_offset >= code.size())
    {
        err.set("viper native: invalid code size");
        return false;
    }

    const size_t alloc_size = (code.size() + 3U) & ~3U;
    uint8_t *exec = (uint8_t *)heap_caps_malloc(alloc_size, MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT);
    if (!exec)
    {
        err.set("viper native: executable memory allocation failed; disable ESP_SYSTEM_MEMPROT_FEATURE");
        return false;
    }
    if (!esp_ptr_executable(exec))
    {
        heap_caps_free(exec);
        err.set("viper native: allocated memory is not executable");
        return false;
    }

    if (!copy_native_code_to_exec(exec, alloc_size, code, err))
    {
        heap_caps_free(exec);
        return false;
    }

    /*
     * ESP32-S3 MALLOC_CAP_EXEC comes from internal executable RAM. After
     * machine-code writes complete, only write completion and subsequent
     * instruction fetch synchronization are required; doing a DCache region
     * writeback on internal RAM can instead trigger a cache error in
     * cache/MMU-sensitive windows.
     */
    __asm__ __volatile__("memw" ::: "memory");
    xthal_icache_sync();

    uint32_t *frame = alloc_viper_native_frame(fn.native_frame_words);
    if (fn.native_frame_words > 0 && !frame)
    {
        heap_caps_free(exec);
        err.set("viper native: local frame allocation failed");
        return false;
    }

    if (fn.native_code)
        heap_caps_free(fn.native_code);
    if (fn.native_frame)
        heap_caps_free(fn.native_frame);
    fn.native_code = exec;
    fn.native_size = code.size();
    fn.native_entry = (ViperNativeEntry)(exec + entry_offset);
    fn.native_frame = frame;
    ViperInternalVector<uint32_t>().swap(fn.native_local_words);
    return true;
}

/**
 * @brief Compile Viper IR to ESP32-S3 Xtensa windowed ABI machine code.
 */
class NativeCompiler
{
  public:
    explicit NativeCompiler(CompiledFunction &fn, asm_xtensa_t &as)
        : m_fn(fn), m_as(as)
    {
    }

    bool emit_all(ViperError &err)
    {
        m_error = nullptr;
        if (m_fn.param_count > 0 && (size_t)m_fn.param_count >= kMaxNativeParams)
            return fail(err, "viper native: too many parameters");
        if (m_fn.bounds_check)
            return fail(err, "viper native: bounds check is not supported by native backend");
        if (m_fn.vars.size() >= UINT16_MAX || m_fn.code.size() >= UINT16_MAX)
            return fail(err, "viper native: function is too large");
        for (const Variable &v : m_fn.vars)
        {
            if (!native_type_supported(v.type))
                return fail(err, "viper native: f32 is not supported yet");
        }
        for (size_t i = 0; i < m_fn.code.size(); ++i)
        {
            const char *reason = native_unsupported_opcode_reason(m_fn.code[i].op);
            if (reason)
            {
                err.set_at_ir(reason, i);
                return false;
            }
        }

        prepare_temp_forwarding();
        prepare_resident_regs();
        prepare_local_slots();
        asm_xtensa_entry_win(&m_as, 0);
        emit_param_loads();
        for (size_t pc = 0; pc < m_fn.code.size();)
        {
            m_pc = pc;
            asm_xtensa_set_label(&m_as, label_for_pc((int)pc));
            if (emit_fused_compare_jump(pc))
            {
                consume_fused_compare_jump_reads(m_fn.code[pc], m_fn.code[pc + 1]);
                ++pc;
                asm_xtensa_set_label(&m_as, label_for_pc((int)pc));
                ++pc;
                continue;
            }
            size_t fused_count = 0;
            if (emit_fused_load_binary_store(pc, fused_count))
            {
                for (size_t i = 0; i < fused_count; ++i)
                    consume_instruction_reads(m_fn.code[pc + i]);
                pc += fused_count;
                continue;
            }
            if (!emit_instruction(m_fn.code[pc]))
                return fail(err, m_error);
            consume_instruction_reads(m_fn.code[pc]);
            ++pc;
        }
        asm_xtensa_set_label(&m_as, label_for_pc((int)m_fn.code.size()));
        if (needs_final_return())
        {
            emit_movi32(ASM_XTENSA_REG_A2, 0);
            asm_xtensa_exit_win(&m_as);
        }

        if (m_as.error)
            return fail(err, m_as.error);
        return true;
    }

  private:
    static constexpr unsigned kValue0 = ASM_XTENSA_REG_A10;
    static constexpr unsigned kValue1 = ASM_XTENSA_REG_A11;
    static constexpr unsigned kValue2 = ASM_XTENSA_REG_A12;
    static constexpr unsigned kValue3 = ASM_XTENSA_REG_A13;
    static constexpr unsigned kScratch = ASM_XTENSA_REG_A14;
    static constexpr unsigned kLocalBaseReg = ASM_XTENSA_REG_A15;
    static constexpr unsigned kArgBaseReg = kScratch;
    static constexpr unsigned kForwardTempReg = kValue2;
    static constexpr int kNoResidentReg = -1;
    static constexpr unsigned kNoScratchReg = 0xffU;

    CompiledFunction &m_fn;
    asm_xtensa_t &m_as;
    const char *m_error = nullptr;
    ViperInternalVector<int> m_resident_regs;
    ViperInternalVector<uint16_t> m_remaining_uses;
    ViperInternalVector<uint8_t> m_branch_targets;
    int m_forward_var = -1;
    size_t m_pc = 0;
    bool m_temp_forwarding = false;

    bool fail(ViperError &err, const char *msg)
    {
        err.set(msg ? msg : "viper native: compile error");
        return false;
    }

    bool error(const char *msg)
    {
        m_error = msg ? msg : "viper native: compile error";
        return false;
    }

    uint32_t label_for_pc(int pc) const
    {
        return (uint32_t)pc;
    }

    uint32_t local_word(int var) const
    {
        if (var < 0 || var >= (int)m_fn.native_local_words.size())
            return 0;
        return m_fn.native_local_words[(size_t)var];
    }

    bool needs_final_return() const
    {
        if (m_fn.code.empty())
            return true;
        if (m_branch_targets.size() > m_fn.code.size() && m_branch_targets[m_fn.code.size()] != 0)
            return true;
        return m_fn.code.back().op != OpCode::Return;
    }

    static bool unsigned_fit_bits_local(uint32_t value, unsigned bits)
    {
        return bits >= 32 || value < (1U << bits);
    }

    static bool signed_fit_bits_local(int32_t value, unsigned bits)
    {
        const int32_t min_value = -(1 << (bits - 1));
        const int32_t max_value = (1 << (bits - 1)) - 1;
        return value >= min_value && value <= max_value;
    }

    static bool is_avoided_reg(unsigned reg, unsigned avoid0, unsigned avoid1,
                               unsigned avoid2, unsigned avoid3, unsigned avoid4 = kNoScratchReg)
    {
        return reg == avoid0 || reg == avoid1 || reg == avoid2 || reg == avoid3 || reg == avoid4;
    }

    /**
     * @brief Select internal scratch registers for the native compiler, avoiding the A8/A9 resident register pool.
     */
    unsigned scratch_reg(unsigned avoid0 = kNoScratchReg, unsigned avoid1 = kNoScratchReg,
                         unsigned avoid2 = kNoScratchReg, unsigned avoid3 = kNoScratchReg,
                         unsigned avoid4 = kNoScratchReg)
    {
        for (size_t i = 0; i < scratch_reg_count(); ++i)
        {
            const unsigned reg = scratch_reg_at(i);
            if (!is_avoided_reg(reg, avoid0, avoid1, avoid2, avoid3, avoid4))
                return reg;
        }
        if (!m_as.error)
            m_as.error = "viper native: scratch register allocation failed";
        return kScratch;
    }

    /**
     * @brief Return a scratch candidate register without a long-lived static array.
     */
    static unsigned scratch_reg_at(size_t index)
    {
        switch (index)
        {
        case 0:
            return kScratch;
        case 1:
            return kValue3;
        case 2:
            return kValue2;
        case 3:
            return kValue1;
        default:
            return kValue0;
        }
    }

    /**
     * @brief Return the number of scratch candidate registers.
     */
    static constexpr size_t scratch_reg_count()
    {
        return 5;
    }

    /**
     * @brief Load a 32-bit immediate with explicit scratch, preventing asm helpers from clobbering A8/A9.
     */
    void emit_movi32(unsigned dst, uint32_t value, unsigned protect0 = kNoScratchReg,
                     unsigned protect1 = kNoScratchReg, unsigned protect2 = kNoScratchReg,
                     unsigned protect3 = kNoScratchReg)
    {
        const unsigned scratch = scratch_reg(dst, protect0, protect1, protect2, protect3);
        asm_xtensa_mov_reg_i32_optimised_scratch(&m_as, dst, value, scratch);
    }

    /**
     * @brief Generate an offset load; the long-offset path uses explicit scratch.
     */
    void emit_load_reg_offset(unsigned dst, unsigned base, uint32_t offset, unsigned operation_size)
    {
        if (operation_size > 2)
        {
            if (!m_as.error)
                m_as.error = "viper native: invalid load size";
            return;
        }
        if (operation_size == 2 && unsigned_fit_bits_local(offset, 4))
        {
            asm_xtensa_op_l32i_n(&m_as, dst, base, offset);
            return;
        }
        if (unsigned_fit_bits_local(offset, 8))
        {
            asm_xtensa_op24(&m_as, ASM_XTENSA_ENCODE_RRI8(2, operation_size, base, dst, offset));
            return;
        }

        const unsigned addr = dst == base ? scratch_reg(dst, base) : dst;
        if (addr == dst)
            emit_movi32(dst, offset << operation_size, base);
        else
        {
            const unsigned imm_scratch = scratch_reg(addr, dst, base);
            asm_xtensa_mov_reg_i32_optimised_scratch(&m_as, addr, offset << operation_size, imm_scratch);
        }
        asm_xtensa_op_add_n(&m_as, addr, base, addr);
        if (operation_size == 2)
            asm_xtensa_op_l32i_n(&m_as, dst, addr, 0);
        else
            asm_xtensa_op24(&m_as, ASM_XTENSA_ENCODE_RRI8(2, operation_size, addr, dst, 0));
    }

    /**
     * @brief Generate an offset store; the long-offset path uses explicit address/scratch registers.
     */
    void emit_store_reg_offset(unsigned src, unsigned base, uint32_t offset, unsigned operation_size)
    {
        if (operation_size > 2)
        {
            if (!m_as.error)
                m_as.error = "viper native: invalid store size";
            return;
        }
        if (operation_size == 2 && unsigned_fit_bits_local(offset, 4))
        {
            asm_xtensa_op_s32i_n(&m_as, src, base, offset);
            return;
        }
        if (unsigned_fit_bits_local(offset, 8))
        {
            asm_xtensa_op24(&m_as, ASM_XTENSA_ENCODE_RRI8(2, 0x04 | operation_size, base, src, offset));
            return;
        }

        const unsigned addr = scratch_reg(src, base);
        const unsigned imm_scratch = scratch_reg(addr, src, base);
        asm_xtensa_mov_reg_i32_optimised_scratch(&m_as, addr, offset << operation_size, imm_scratch);
        asm_xtensa_op_add_n(&m_as, addr, base, addr);
        if (operation_size == 2)
            asm_xtensa_op_s32i_n(&m_as, src, addr, 0);
        else
            asm_xtensa_op24(&m_as, ASM_XTENSA_ENCODE_RRI8(2, 0x04 | operation_size, addr, src, 0));
    }

    static unsigned resident_reg_at(size_t index)
    {
        switch (index)
        {
        case 0:
            return ASM_XTENSA_REG_A3;
        case 1:
            return ASM_XTENSA_REG_A4;
        case 2:
            return ASM_XTENSA_REG_A5;
        case 3:
            return ASM_XTENSA_REG_A6;
        case 4:
            return ASM_XTENSA_REG_A7;
        case 5:
            return ASM_XTENSA_REG_A8;
        case 6:
            return ASM_XTENSA_REG_A9;
        default:
            return ASM_XTENSA_REG_A2;
        }
    }

    static constexpr size_t resident_reg_count()
    {
        return 8;
    }

    static uint32_t resident_loop_weight(uint8_t depth)
    {
        const unsigned shift = depth >= 4 ? 8U : (unsigned)depth * 2U;
        return 1U << shift;
    }

    static void add_resident_score(ViperInternalVector<uint32_t> &scores, int var, uint32_t amount)
    {
        if (var < 0 || var >= (int)scores.size() || amount == 0)
            return;
        const uint32_t old = scores[(size_t)var];
        scores[(size_t)var] = UINT32_MAX - old < amount ? UINT32_MAX : old + amount;
    }

    void score_instruction_resident_vars(ViperInternalVector<uint32_t> &scores,
                                         const Instruction &ins, uint32_t weight)
    {
        const uint32_t read_weight = weight * 2U;
        switch (ins.op)
        {
        case OpCode::Const:
            add_resident_score(scores, ins.a, weight);
            break;
        case OpCode::Mov:
            add_resident_score(scores, ins.b, read_weight);
            add_resident_score(scores, ins.a, weight);
            break;
        case OpCode::JumpIfFalse:
        case OpCode::Return:
            add_resident_score(scores, ins.a, read_weight);
            break;
        case OpCode::Add:
        case OpCode::AddI:
        case OpCode::AddI32:
        case OpCode::Sub:
        case OpCode::SubI:
        case OpCode::SubI32:
        case OpCode::Mul:
        case OpCode::MulI:
        case OpCode::MulI32:
        case OpCode::Div:
        case OpCode::DivI:
        case OpCode::Mod:
        case OpCode::BitAnd:
        case OpCode::BitOr:
        case OpCode::BitXor:
        case OpCode::Shl:
        case OpCode::Shr:
        case OpCode::Eq:
        case OpCode::EqI:
        case OpCode::Ne:
        case OpCode::NeI:
        case OpCode::Lt:
        case OpCode::LtI:
        case OpCode::Le:
        case OpCode::LeI:
        case OpCode::Gt:
        case OpCode::GtI:
        case OpCode::Ge:
        case OpCode::GeI:
        case OpCode::Load:
        case OpCode::Load8:
        case OpCode::Load8I32:
        case OpCode::Load16:
        case OpCode::Load16I32:
        case OpCode::Load32:
        case OpCode::Load32I32:
            add_resident_score(scores, ins.b, read_weight);
            add_resident_score(scores, ins.c, read_weight);
            add_resident_score(scores, ins.a, weight);
            break;
        case OpCode::Neg:
        case OpCode::NegI:
            add_resident_score(scores, ins.b, read_weight);
            add_resident_score(scores, ins.a, weight);
            break;
        case OpCode::Store:
        case OpCode::Store8:
        case OpCode::Store16:
        case OpCode::Store32:
            add_resident_score(scores, ins.a, read_weight);
            add_resident_score(scores, ins.b, read_weight);
            add_resident_score(scores, ins.c, read_weight);
            break;
        default:
            break;
        }
    }

    /**
     * @brief Allocate resident registers to named variables by loop hotness; temporaries still use short-lifetime forwarding.
     */
    void prepare_resident_regs()
    {
        m_resident_regs.assign(m_fn.vars.size(), kNoResidentReg);
        if (m_fn.vars.empty())
            return;

        ViperInternalVector<uint8_t> loop_depth(m_fn.code.size(), 0);
        for (size_t pc = 0; pc < m_fn.code.size(); ++pc)
        {
            const Instruction &ins = m_fn.code[pc];
            if (ins.op != OpCode::Jump || ins.a < 0 || ins.a > (int)pc)
                continue;

            const size_t begin = (size_t)ins.a;
            for (size_t i = begin; i <= pc && i < loop_depth.size(); ++i)
            {
                if (loop_depth[i] < 4)
                    ++loop_depth[i];
            }
        }

        ViperInternalVector<uint32_t> scores(m_fn.vars.size(), 0);
        for (size_t pc = 0; pc < m_fn.code.size(); ++pc)
        {
            const uint32_t weight = resident_loop_weight(loop_depth[pc]);
            score_instruction_resident_vars(scores, m_fn.code[pc], weight);
        }

        struct Candidate
        {
            size_t var = 0;
            uint32_t score = 0;
        };

        ViperInternalVector<Candidate> candidates;
        candidates.reserve(m_fn.vars.size());
        for (size_t i = 0; i < m_fn.vars.size(); ++i)
        {
            const Variable &v = m_fn.vars[i];
            if (v.is_const || v.is_temp || !native_type_supported(v.type) || v.type == ValueType::Void)
                continue;

            const uint32_t score = scores[i];
            if (score == 0)
                continue;
            Candidate candidate;
            candidate.var = i;
            candidate.score = score;
            candidates.push_back(candidate);
        }

        std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
            if (a.score != b.score)
                return a.score > b.score;
            return a.var < b.var;
        });

        bool used[resident_reg_count()] = {};
        for (const Candidate &candidate : candidates)
        {
            const Variable &v = m_fn.vars[candidate.var];
            for (size_t ri = 0; ri < resident_reg_count(); ++ri)
            {
                if (used[ri])
                    continue;

                const unsigned reg = resident_reg_at(ri);
                if (v.is_param && reg == ASM_XTENSA_REG_A2)
                    continue;

                m_resident_regs[candidate.var] = (int)reg;
                used[ri] = true;
                break;
            }
        }
    }

    /**
     * @brief Allocate dense heap local-frame slots for variables that must spill to memory.
     */
    void prepare_local_slots()
    {
        m_fn.native_local_words.assign(m_fn.vars.size(), UINT32_MAX);
        size_t next = 0;
        for (size_t i = 0; i < m_fn.vars.size(); ++i)
        {
            const Variable &v = m_fn.vars[i];
            if (v.is_const || v.type == ValueType::Void || resident_reg((int)i) >= 0)
                continue;
            m_fn.native_local_words[i] = (uint32_t)next++;
        }
        m_fn.native_frame_words = next;
    }

    /**
     * @brief Pre-count variable uses and branch targets for short-lifetime temporary forwarding.
     */
    void prepare_temp_forwarding()
    {
        m_temp_forwarding = m_fn.code.size() <= kMaxTempForwardingInstructions;
        m_remaining_uses.assign(m_fn.vars.size(), 0);
        m_branch_targets.assign(m_fn.code.size() + 1, 0);
        m_forward_var = -1;

        for (const Instruction &ins : m_fn.code)
        {
            mark_instruction_reads(ins);
            if (ins.op == OpCode::Jump && ins.a >= 0 && ins.a < (int)m_branch_targets.size())
                m_branch_targets[(size_t)ins.a] = 1;
            else if (ins.op == OpCode::JumpIfFalse && ins.b >= 0 && ins.b < (int)m_branch_targets.size())
                m_branch_targets[(size_t)ins.b] = 1;
        }
    }

    void mark_use(int var)
    {
        if (var >= 0 && var < (int)m_remaining_uses.size() && m_remaining_uses[(size_t)var] < UINT16_MAX)
            ++m_remaining_uses[(size_t)var];
    }

    /**
     * @brief Count the input variables of one IR instruction.
     */
    void mark_instruction_reads(const Instruction &ins)
    {
        switch (ins.op)
        {
        case OpCode::Mov:
        case OpCode::JumpIfFalse:
        case OpCode::Return:
            mark_use(ins.a >= 0 && ins.op == OpCode::Mov ? ins.b : ins.a);
            break;
        case OpCode::Add:
        case OpCode::AddI:
        case OpCode::AddI32:
        case OpCode::Sub:
        case OpCode::SubI:
        case OpCode::SubI32:
        case OpCode::Mul:
        case OpCode::MulI:
        case OpCode::MulI32:
        case OpCode::Div:
        case OpCode::DivI:
        case OpCode::Mod:
        case OpCode::BitAnd:
        case OpCode::BitOr:
        case OpCode::BitXor:
        case OpCode::Shl:
        case OpCode::Shr:
        case OpCode::Eq:
        case OpCode::EqI:
        case OpCode::Ne:
        case OpCode::NeI:
        case OpCode::Lt:
        case OpCode::LtI:
        case OpCode::Le:
        case OpCode::LeI:
        case OpCode::Gt:
        case OpCode::GtI:
        case OpCode::Ge:
        case OpCode::GeI:
        case OpCode::Load:
        case OpCode::Load8:
        case OpCode::Load8I32:
        case OpCode::Load16:
        case OpCode::Load16I32:
        case OpCode::Load32:
        case OpCode::Load32I32:
            mark_use(ins.b);
            mark_use(ins.c);
            break;
        case OpCode::Neg:
        case OpCode::NegI:
            mark_use(ins.b);
            break;
        case OpCode::Store:
        case OpCode::Store8:
        case OpCode::Store16:
        case OpCode::Store32:
            mark_use(ins.a);
            mark_use(ins.b);
            mark_use(ins.c);
            break;
        default:
            break;
        }
    }

    /**
     * @brief Determine whether an IR instruction reads the specified variable.
     */
    bool instruction_reads_var(const Instruction &ins, int var) const
    {
        if (var < 0)
            return false;
        switch (ins.op)
        {
        case OpCode::Mov:
            return ins.b == var;
        case OpCode::JumpIfFalse:
        case OpCode::Return:
            return ins.a == var;
        case OpCode::Neg:
        case OpCode::NegI:
            return ins.b == var;
        case OpCode::Store:
        case OpCode::Store8:
        case OpCode::Store16:
        case OpCode::Store32:
            return ins.a == var || ins.b == var || ins.c == var;
        case OpCode::Jump:
        case OpCode::Const:
            return false;
        default:
            return ins.b == var || ins.c == var;
        }
    }

    void consume_use(int var)
    {
        if (var < 0 || var >= (int)m_remaining_uses.size())
            return;
        uint16_t &uses = m_remaining_uses[(size_t)var];
        if (uses > 0)
            --uses;
        if (m_forward_var == var && uses == 0)
            m_forward_var = -1;
    }

    /**
     * @brief Decrement input variable use counts after emitting one IR instruction.
     */
    void consume_instruction_reads(const Instruction &ins)
    {
        switch (ins.op)
        {
        case OpCode::Mov:
            consume_use(ins.b);
            break;
        case OpCode::JumpIfFalse:
        case OpCode::Return:
            consume_use(ins.a);
            break;
        case OpCode::Add:
        case OpCode::AddI:
        case OpCode::AddI32:
        case OpCode::Sub:
        case OpCode::SubI:
        case OpCode::SubI32:
        case OpCode::Mul:
        case OpCode::MulI:
        case OpCode::MulI32:
        case OpCode::Div:
        case OpCode::DivI:
        case OpCode::Mod:
        case OpCode::BitAnd:
        case OpCode::BitOr:
        case OpCode::BitXor:
        case OpCode::Shl:
        case OpCode::Shr:
        case OpCode::Eq:
        case OpCode::EqI:
        case OpCode::Ne:
        case OpCode::NeI:
        case OpCode::Lt:
        case OpCode::LtI:
        case OpCode::Le:
        case OpCode::LeI:
        case OpCode::Gt:
        case OpCode::GtI:
        case OpCode::Ge:
        case OpCode::GeI:
        case OpCode::Load:
        case OpCode::Load8:
        case OpCode::Load8I32:
        case OpCode::Load16:
        case OpCode::Load16I32:
        case OpCode::Load32:
        case OpCode::Load32I32:
            consume_use(ins.b);
            consume_use(ins.c);
            break;
        case OpCode::Neg:
        case OpCode::NegI:
            consume_use(ins.b);
            break;
        case OpCode::Store:
        case OpCode::Store8:
        case OpCode::Store16:
        case OpCode::Store32:
            consume_use(ins.a);
            consume_use(ins.b);
            consume_use(ins.c);
            break;
        default:
            break;
        }
    }

    /**
     * @brief Decrement compare inputs and the branch condition read after fusing compare+branch.
     */
    void consume_fused_compare_jump_reads(const Instruction &cmp, const Instruction &jmp)
    {
        consume_use(cmp.b);
        consume_use(cmp.c);
        consume_use(jmp.a);
    }

    bool can_forward_temp(int var) const
    {
        if (!m_temp_forwarding)
            return false;
        if (var < 0 || var >= (int)m_fn.vars.size() || var >= (int)m_remaining_uses.size())
            return false;
        const Variable &v = m_fn.vars[(size_t)var];
        const size_t next_pc = m_pc + 1;
        return v.is_temp && !v.is_const && native_type_supported(v.type) &&
               m_remaining_uses[(size_t)var] == 1 && next_pc < m_fn.code.size() &&
               (next_pc >= m_branch_targets.size() || !m_branch_targets[next_pc]) &&
               instruction_reads_var(m_fn.code[next_pc], var);
    }

    int resident_reg(int var) const
    {
        if (var < 0 || var >= (int)m_resident_regs.size())
            return kNoResidentReg;
        return m_resident_regs[(size_t)var];
    }

    void mov_reg(unsigned dst, unsigned src)
    {
        if (dst != src)
            asm_xtensa_op_mov_n(&m_as, dst, src);
    }

    void emit_param_loads()
    {
        emit_load_reg_offset(kLocalBaseReg, ASM_XTENSA_REG_A2, 1, 2);
        emit_load_reg_offset(kArgBaseReg, ASM_XTENSA_REG_A2, 0, 2);
        for (int i = 0; i < m_fn.param_count; ++i)
        {
            const int reg = resident_reg(i);
            if (reg >= 0)
            {
                emit_load_reg_offset((unsigned)reg, kArgBaseReg, (uint32_t)i * 2U, 2);
            }
            else
            {
                emit_load_reg_offset(kValue0, kArgBaseReg, (uint32_t)i * 2U, 2);
                store_local(i, kValue0);
            }
        }
    }

    void load_value(unsigned dst, int var)
    {
        if (var >= 0 && var < (int)m_fn.vars.size() && m_fn.vars[var].is_const)
        {
            emit_movi32(dst, (uint32_t)m_fn.vars[var].const_i);
            return;
        }
        if (m_temp_forwarding && var == m_forward_var)
        {
            mov_reg(dst, kForwardTempReg);
            return;
        }
        const int reg = resident_reg(var);
        if (reg >= 0)
        {
            mov_reg(dst, (unsigned)reg);
            return;
        }
        emit_load_reg_offset(dst, kLocalBaseReg, local_word(var), 2);
    }

    /**
     * @brief Reuse resident/forwarded registers for read-only operands when possible to reduce resident-to-scratch moves.
     */
    unsigned read_value_reg(int var, unsigned preferred, unsigned avoid0 = kNoScratchReg,
                            unsigned avoid1 = kNoScratchReg, unsigned avoid2 = kNoScratchReg,
                            unsigned avoid3 = kNoScratchReg)
    {
        unsigned dst = preferred;
        if (is_avoided_reg(dst, avoid0, avoid1, avoid2, avoid3))
            dst = scratch_reg(avoid0, avoid1, avoid2, avoid3);

        if (var >= 0 && var < (int)m_fn.vars.size() && m_fn.vars[(size_t)var].is_const)
        {
            emit_movi32(dst, (uint32_t)m_fn.vars[(size_t)var].const_i, avoid0, avoid1, avoid2, avoid3);
            return dst;
        }

        if (m_temp_forwarding && var == m_forward_var)
        {
            if (!is_avoided_reg(kForwardTempReg, avoid0, avoid1, avoid2, avoid3))
                return kForwardTempReg;
            mov_reg(dst, kForwardTempReg);
            return dst;
        }

        const int reg = resident_reg(var);
        if (reg >= 0)
        {
            if (!is_avoided_reg((unsigned)reg, avoid0, avoid1, avoid2, avoid3))
                return (unsigned)reg;
            mov_reg(dst, (unsigned)reg);
            return dst;
        }

        emit_load_reg_offset(dst, kLocalBaseReg, local_word(var), 2);
        return dst;
    }

    void store_local(int var, unsigned src)
    {
        emit_store_reg_offset(src, kLocalBaseReg, local_word(var), 2);
    }

    void store_value(int var, unsigned src)
    {
        emit_cast_in_reg(src, m_fn.vars[var].type);
        const int reg = resident_reg(var);
        if (reg >= 0)
        {
            mov_reg((unsigned)reg, src);
            return;
        }

        if (can_forward_temp(var))
        {
            mov_reg(kForwardTempReg, src);
            m_forward_var = var;
            return;
        }

        if (m_temp_forwarding && m_forward_var == var)
            m_forward_var = -1;
        store_local(var, src);
    }

    void emit_mask(unsigned reg, uint32_t mask)
    {
        if (mask == 0xffU)
        {
            asm_xtensa_op_extui(&m_as, reg, reg, 0, 8);
            return;
        }
        if (mask == 0xffffU)
        {
            asm_xtensa_op_extui(&m_as, reg, reg, 0, 16);
            return;
        }
        emit_movi32(kScratch, mask, reg);
        asm_xtensa_op_and(&m_as, reg, reg, kScratch);
    }

    void emit_sign_extend(unsigned reg, int bits)
    {
        const int shift = 32 - bits;
        asm_xtensa_op_movi_n(&m_as, kScratch, shift);
        asm_xtensa_op_ssl(&m_as, kScratch);
        asm_xtensa_op_sll(&m_as, reg, reg);
        asm_xtensa_op_ssr(&m_as, kScratch);
        asm_xtensa_op_sra(&m_as, reg, reg);
    }

    void emit_cast_in_reg(unsigned reg, ValueType type)
    {
        switch (type)
        {
        case ValueType::I8:
            emit_sign_extend(reg, 8);
            break;
        case ValueType::U8:
            emit_mask(reg, 0xffU);
            break;
        case ValueType::I16:
            emit_sign_extend(reg, 16);
            break;
        case ValueType::U16:
            emit_mask(reg, 0xffffU);
            break;
        case ValueType::Bool:
            asm_xtensa_op_movi_n(&m_as, kScratch, 0);
            {
                const unsigned bool_tmp = reg == kValue2 ? kValue1 : kValue2;
                asm_xtensa_setcc_reg_reg_reg(&m_as, ASM_XTENSA_CC_NE, bool_tmp, reg, kScratch);
                asm_xtensa_op_mov_n(&m_as, reg, bool_tmp);
            }
            break;
        default:
            break;
        }
    }

    static bool is_compare_opcode(OpCode op)
    {
        switch (op)
        {
        case OpCode::Eq:
        case OpCode::EqI:
        case OpCode::Ne:
        case OpCode::NeI:
        case OpCode::Lt:
        case OpCode::LtI:
        case OpCode::Le:
        case OpCode::LeI:
        case OpCode::Gt:
        case OpCode::GtI:
        case OpCode::Ge:
        case OpCode::GeI:
            return true;
        default:
            return false;
        }
    }

    /**
     * @brief Fuse compare + JumpIfFalse into a direct conditional branch.
     */
    bool emit_fused_compare_jump(size_t pc)
    {
        if (pc + 1 >= m_fn.code.size())
            return false;

        const Instruction &cmp = m_fn.code[pc];
        const Instruction &jmp = m_fn.code[pc + 1];
        if (!is_compare_opcode(cmp.op) || jmp.op != OpCode::JumpIfFalse || jmp.a != cmp.a)
            return false;
        if (pc + 1 < m_branch_targets.size() && m_branch_targets[pc + 1])
            return false;
        if (cmp.a < 0 || cmp.a >= (int)m_remaining_uses.size() || m_remaining_uses[(size_t)cmp.a] != 1)
            return false;
        if (jmp.b == (int)pc + 2)
            return true;

        emit_compare_branch_false(cmp, jmp.b);
        return true;
    }

    void emit_compare_branch_false(const Instruction &cmp, int target_pc)
    {
        const bool use_unsigned = native_unsigned_compare(m_fn.vars[cmp.b].type, m_fn.vars[cmp.c].type);
        unsigned cond = ASM_XTENSA_CC_NE;
        int lhs = cmp.b;
        int rhs = cmp.c;

        switch (cmp.op)
        {
        case OpCode::Eq:
        case OpCode::EqI:
            cond = ASM_XTENSA_CC_NE;
            break;
        case OpCode::Ne:
        case OpCode::NeI:
            cond = ASM_XTENSA_CC_EQ;
            break;
        case OpCode::Lt:
        case OpCode::LtI:
            cond = use_unsigned ? ASM_XTENSA_CC_GEU : ASM_XTENSA_CC_GE;
            break;
        case OpCode::Le:
        case OpCode::LeI:
            cond = use_unsigned ? ASM_XTENSA_CC_LTU : ASM_XTENSA_CC_LT;
            lhs = cmp.c;
            rhs = cmp.b;
            break;
        case OpCode::Gt:
        case OpCode::GtI:
            cond = use_unsigned ? ASM_XTENSA_CC_GEU : ASM_XTENSA_CC_GE;
            lhs = cmp.c;
            rhs = cmp.b;
            break;
        case OpCode::Ge:
        case OpCode::GeI:
            cond = use_unsigned ? ASM_XTENSA_CC_LTU : ASM_XTENSA_CC_LT;
            break;
        default:
            return;
        }

        if (emit_compare_zero_branch_false(cmp, target_pc, use_unsigned))
            return;

        const unsigned lhs_reg = read_value_reg(lhs, kValue0);
        const unsigned rhs_reg = read_value_reg(rhs, kValue1, lhs_reg);
        asm_xtensa_bcc_reg_reg_label(&m_as, cond, lhs_reg, rhs_reg, label_for_pc(target_pc));
    }

    bool emit_compare_zero_branch_false(const Instruction &cmp, int target_pc, bool use_unsigned)
    {
        int32_t value = 0;
        if (const_i32(cmp.c, value) && value == 0)
        {
            switch (cmp.op)
            {
            case OpCode::Eq:
            case OpCode::EqI:
                emit_bccz_var(cmp.b, ASM_XTENSA_CCZ_NE, target_pc);
                return true;
            case OpCode::Ne:
            case OpCode::NeI:
                emit_bccz_var(cmp.b, ASM_XTENSA_CCZ_EQ, target_pc);
                return true;
            case OpCode::Lt:
            case OpCode::LtI:
                if (use_unsigned)
                    asm_xtensa_j_label(&m_as, label_for_pc(target_pc));
                else
                    emit_bccz_var(cmp.b, ASM_XTENSA_CCZ_GE, target_pc);
                return true;
            case OpCode::Le:
            case OpCode::LeI:
                if (use_unsigned)
                {
                    emit_bccz_var(cmp.b, ASM_XTENSA_CCZ_NE, target_pc);
                    return true;
                }
                return false;
            case OpCode::Gt:
            case OpCode::GtI:
                if (use_unsigned)
                {
                    emit_bccz_var(cmp.b, ASM_XTENSA_CCZ_EQ, target_pc);
                    return true;
                }
                return false;
            case OpCode::Ge:
            case OpCode::GeI:
                if (use_unsigned)
                    return true;
                emit_bccz_var(cmp.b, ASM_XTENSA_CCZ_LT, target_pc);
                return true;
            default:
                return false;
            }
        }

        if (const_i32(cmp.b, value) && value == 0)
        {
            switch (cmp.op)
            {
            case OpCode::Eq:
            case OpCode::EqI:
                emit_bccz_var(cmp.c, ASM_XTENSA_CCZ_NE, target_pc);
                return true;
            case OpCode::Ne:
            case OpCode::NeI:
                emit_bccz_var(cmp.c, ASM_XTENSA_CCZ_EQ, target_pc);
                return true;
            case OpCode::Lt:
            case OpCode::LtI:
                if (use_unsigned)
                {
                    emit_bccz_var(cmp.c, ASM_XTENSA_CCZ_EQ, target_pc);
                    return true;
                }
                return false;
            case OpCode::Le:
            case OpCode::LeI:
                if (use_unsigned)
                    return true;
                emit_bccz_var(cmp.c, ASM_XTENSA_CCZ_LT, target_pc);
                return true;
            case OpCode::Gt:
            case OpCode::GtI:
                if (use_unsigned)
                    asm_xtensa_j_label(&m_as, label_for_pc(target_pc));
                else
                    emit_bccz_var(cmp.c, ASM_XTENSA_CCZ_GE, target_pc);
                return true;
            case OpCode::Ge:
            case OpCode::GeI:
                if (use_unsigned)
                {
                    emit_bccz_var(cmp.c, ASM_XTENSA_CCZ_NE, target_pc);
                    return true;
                }
                return false;
            default:
                return false;
            }
        }

        return false;
    }

    void emit_bccz_var(int var, unsigned cond, int target_pc)
    {
        const unsigned reg = read_value_reg(var, kValue0);
        asm_xtensa_bccz_reg_label(&m_as, cond, reg, label_for_pc(target_pc));
    }

    bool emit_fused_load_binary_store(size_t pc, size_t &count)
    {
        count = 0;
        if (pc + 2 >= m_fn.code.size())
            return false;

        const Instruction &load = m_fn.code[pc];
        const Instruction &bin = m_fn.code[pc + 1];
        const Instruction &store = m_fn.code[pc + 2];
        int other = -1;
        bool loaded_lhs = false;
        if (!can_fuse_load_binary_store(pc, load, bin, store, other, loaded_lhs))
            return false;

        const size_t elem_size = ptr_elem_size(load.type);
        const unsigned op_size = ptr_operation_size(elem_size);
        int32_t const_index = 0;
        if (const_i32(load.c, const_index))
        {
            const unsigned base = read_value_reg(load.b, kValue0);
            const unsigned value = scratch_reg(base);
            emit_load_reg_offset(value, base, (uint32_t)const_index, op_size);
            emit_fused_binary_inplace(bin, other, loaded_lhs, value, base);
            emit_store_reg_offset(value, base, (uint32_t)const_index, op_size);
        }
        else
        {
            const unsigned base = read_value_reg(load.b, kValue0);
            const unsigned index = read_value_reg(load.c, kValue1, base);
            const unsigned addr = scratch_reg(base, index);
            emit_index_addr(addr, base, index, elem_size);
            const unsigned value = scratch_reg(base, index, addr);

            if (elem_size == 1)
                asm_xtensa_op_l8ui(&m_as, value, addr, 0);
            else if (elem_size == 2)
                asm_xtensa_op_l16ui(&m_as, value, addr, 0);
            else
                asm_xtensa_op_l32i_n(&m_as, value, addr, 0);

            emit_fused_binary_inplace(bin, other, loaded_lhs, value, base, index, addr);

            if (elem_size == 1)
                asm_xtensa_op_s8i(&m_as, value, addr, 0);
            else if (elem_size == 2)
                asm_xtensa_op_s16i(&m_as, value, addr, 0);
            else
                asm_xtensa_op_s32i_n(&m_as, value, addr, 0);
        }

        count = 3;
        return true;
    }

    bool can_fuse_load_binary_store(size_t pc, const Instruction &load, const Instruction &bin,
                                    const Instruction &store, int &other, bool &loaded_lhs) const
    {
        if (pc + 1 < m_branch_targets.size() && m_branch_targets[pc + 1])
            return false;
        if (pc + 2 < m_branch_targets.size() && m_branch_targets[pc + 2])
            return false;
        if (load.type == ValueType::PtrF32 || load.op != load_opcode_for_ptr(load.type) ||
            store.op != store_opcode_for_ptr(load.type))
            return false;
        if (store.a != load.b || store.b != load.c || store.c != bin.a)
            return false;
        if (!is_native_temp(load.a) || !is_native_temp(bin.a))
            return false;
        if (load.a < 0 || load.a >= (int)m_remaining_uses.size() || m_remaining_uses[(size_t)load.a] != 1)
            return false;
        if (bin.a < 0 || bin.a >= (int)m_remaining_uses.size() || m_remaining_uses[(size_t)bin.a] != 1)
            return false;

        loaded_lhs = bin.b == load.a;
        const bool loaded_rhs = bin.c == load.a;
        if (!loaded_lhs && !loaded_rhs)
            return false;
        if (!loaded_lhs && !is_commutative_binary_opcode(bin.op))
            return false;
        if (!is_fusible_binary_opcode(bin.op))
            return false;

        other = loaded_lhs ? bin.c : bin.b;
        return other >= 0 && other < (int)m_fn.vars.size();
    }

    bool is_native_temp(int var) const
    {
        return var >= 0 && var < (int)m_fn.vars.size() && m_fn.vars[(size_t)var].is_temp;
    }

    static bool is_commutative_binary_opcode(OpCode op)
    {
        switch (op)
        {
        case OpCode::Add:
        case OpCode::AddI:
        case OpCode::AddI32:
        case OpCode::Mul:
        case OpCode::MulI:
        case OpCode::MulI32:
        case OpCode::BitAnd:
        case OpCode::BitOr:
        case OpCode::BitXor:
            return true;
        default:
            return false;
        }
    }

    static bool is_fusible_binary_opcode(OpCode op)
    {
        switch (op)
        {
        case OpCode::Add:
        case OpCode::AddI:
        case OpCode::AddI32:
        case OpCode::Sub:
        case OpCode::SubI:
        case OpCode::SubI32:
        case OpCode::Mul:
        case OpCode::MulI:
        case OpCode::MulI32:
        case OpCode::Div:
        case OpCode::DivI:
        case OpCode::Mod:
        case OpCode::Shl:
        case OpCode::Shr:
        case OpCode::BitAnd:
        case OpCode::BitOr:
        case OpCode::BitXor:
            return true;
        default:
            return false;
        }
    }

    void emit_fused_binary_inplace(const Instruction &bin, int other, bool loaded_lhs, unsigned value,
                                   unsigned avoid0 = kNoScratchReg, unsigned avoid1 = kNoScratchReg,
                                   unsigned avoid2 = kNoScratchReg)
    {
        int32_t imm = 0;
        unsigned shift = 0;
        switch (bin.op)
        {
        case OpCode::Add:
        case OpCode::AddI:
        case OpCode::AddI32:
            if (const_i32(other, imm) && signed_fit_bits_local(imm, 8))
                asm_xtensa_op_addi(&m_as, value, value, imm);
            else
                asm_xtensa_op_add_n(&m_as, value, value, read_fused_rhs(other, value, avoid0, avoid1, avoid2));
            return;
        case OpCode::Sub:
        case OpCode::SubI:
        case OpCode::SubI32:
            if (loaded_lhs && const_i32(other, imm) && imm != std::numeric_limits<int32_t>::min() &&
                signed_fit_bits_local(-imm, 8))
                asm_xtensa_op_addi(&m_as, value, value, -imm);
            else
                asm_xtensa_op_sub(&m_as, value, value, read_fused_rhs(other, value, avoid0, avoid1, avoid2));
            return;
        case OpCode::Mul:
        case OpCode::MulI:
        case OpCode::MulI32:
            if (const_i32(other, imm) && power2_shift((uint32_t)imm, shift))
                emit_shift_reg_imm(value, shift, true, avoid0, avoid1, avoid2);
            else
                asm_xtensa_op_mull(&m_as, value, value, read_fused_rhs(other, value, avoid0, avoid1, avoid2));
            return;
        case OpCode::Div:
        case OpCode::DivI:
            if (const_i32(other, imm) &&
                (native_unsigned_compare(m_fn.vars[bin.b].type, m_fn.vars[bin.c].type) ||
                 m_fn.vars[bin.a].type == ValueType::U32) &&
                power2_shift((uint32_t)imm, shift))
            {
                emit_shift_reg_imm(value, shift, false, avoid0, avoid1, avoid2);
                return;
            }
            emit_fused_div_mod(bin, other, value, false, avoid0, avoid1, avoid2);
            return;
        case OpCode::Mod:
            emit_fused_div_mod(bin, other, value, true, avoid0, avoid1, avoid2);
            return;
        case OpCode::Shl:
            if (const_i32(other, imm))
                emit_shift_reg_imm(value, (uint32_t)imm, true, avoid0, avoid1, avoid2);
            else
            {
                asm_xtensa_op_ssl(&m_as, read_fused_rhs(other, value, avoid0, avoid1, avoid2));
                asm_xtensa_op_sll(&m_as, value, value);
            }
            return;
        case OpCode::Shr:
            if (const_i32(other, imm))
                emit_shift_reg_imm(value, (uint32_t)imm, false, avoid0, avoid1, avoid2);
            else
            {
                asm_xtensa_op_ssr(&m_as, read_fused_rhs(other, value, avoid0, avoid1, avoid2));
                asm_xtensa_op_srl(&m_as, value, value);
            }
            return;
        case OpCode::BitAnd:
            if (const_i32(other, imm))
            {
                const uint32_t mask = (uint32_t)imm;
                if (mask == 0)
                    asm_xtensa_op_movi_n(&m_as, value, 0);
                else if (mask == UINT32_MAX)
                    return;
                else if (low_mask_bit_count(mask, shift))
                    asm_xtensa_op_extui(&m_as, value, value, 0, shift);
                else
                    asm_xtensa_op_and(&m_as, value, value, read_fused_rhs(other, value, avoid0, avoid1, avoid2));
            }
            else
                asm_xtensa_op_and(&m_as, value, value, read_fused_rhs(other, value, avoid0, avoid1, avoid2));
            return;
        case OpCode::BitOr:
            asm_xtensa_op_or(&m_as, value, value, read_fused_rhs(other, value, avoid0, avoid1, avoid2));
            return;
        case OpCode::BitXor:
            asm_xtensa_op_xor(&m_as, value, value, read_fused_rhs(other, value, avoid0, avoid1, avoid2));
            return;
        default:
            return;
        }
    }

    unsigned read_fused_rhs(int var, unsigned value, unsigned avoid0, unsigned avoid1, unsigned avoid2)
    {
        return read_value_reg(var, scratch_reg(value, avoid0, avoid1, avoid2),
                              value, avoid0, avoid1, avoid2);
    }

    void emit_shift_reg_imm(unsigned reg, uint32_t shift, bool left,
                            unsigned avoid0 = kNoScratchReg, unsigned avoid1 = kNoScratchReg,
                            unsigned avoid2 = kNoScratchReg)
    {
        shift &= 31U;
        if (shift == 0)
            return;
        const unsigned shift_reg = scratch_reg(reg, avoid0, avoid1, avoid2);
        asm_xtensa_op_movi_n(&m_as, shift_reg, (int)shift);
        if (left)
            asm_xtensa_op_ssl(&m_as, shift_reg);
        else
            asm_xtensa_op_ssr(&m_as, shift_reg);
        if (left)
            asm_xtensa_op_sll(&m_as, reg, reg);
        else
            asm_xtensa_op_srl(&m_as, reg, reg);
    }

    void emit_fused_div_mod(const Instruction &bin, int other, unsigned value, bool is_mod,
                            unsigned avoid0, unsigned avoid1, unsigned avoid2)
    {
        const unsigned rhs = read_fused_rhs(other, value, avoid0, avoid1, avoid2);
        const bool use_unsigned = native_unsigned_compare(m_fn.vars[bin.b].type, m_fn.vars[bin.c].type) ||
                                  m_fn.vars[bin.a].type == ValueType::U32;
        if (is_mod)
        {
            if (use_unsigned)
                asm_xtensa_op_remu(&m_as, value, value, rhs);
            else
                asm_xtensa_op_rems(&m_as, value, value, rhs);
        }
        else
        {
            if (use_unsigned)
                asm_xtensa_op_quou(&m_as, value, value, rhs);
            else
                asm_xtensa_op_quos(&m_as, value, value, rhs);
        }
    }

    bool emit_instruction(const Instruction &ins)
    {
        switch (ins.op)
        {
        case OpCode::Const:
            emit_movi32(kValue0, (uint32_t)ins.imm);
            store_value(ins.a, kValue0);
            return true;
        case OpCode::Mov:
            if (can_update_resident_without_cast(ins.a))
            {
                store_value(ins.a, read_value_reg(ins.b, kValue0));
                return true;
            }
            load_value(kValue0, ins.b);
            store_value(ins.a, kValue0);
            return true;
        case OpCode::Add:
        case OpCode::AddI:
        case OpCode::AddI32:
            if (emit_add_const(ins))
                return true;
            if (emit_binary_inplace(ins, true, [](asm_xtensa_t *as, unsigned dst, unsigned lhs, unsigned rhs) {
                    asm_xtensa_op_add_n(as, dst, lhs, rhs);
                }))
                return true;
            return emit_binary_reg(ins, [](asm_xtensa_t *as, unsigned rhs) {
                asm_xtensa_op_add_n(as, kValue0, kValue0, rhs);
            });
        case OpCode::Sub:
        case OpCode::SubI:
        case OpCode::SubI32:
            if (emit_sub_const(ins))
                return true;
            if (emit_binary_inplace(ins, false, [](asm_xtensa_t *as, unsigned dst, unsigned lhs, unsigned rhs) {
                    asm_xtensa_op_sub(as, dst, lhs, rhs);
                }))
                return true;
            return emit_binary_reg(ins, [](asm_xtensa_t *as, unsigned rhs) {
                asm_xtensa_op_sub(as, kValue0, kValue0, rhs);
            });
        case OpCode::Mul:
        case OpCode::MulI:
        case OpCode::MulI32:
            if (emit_mul_pow2(ins))
                return true;
            if (emit_binary_inplace(ins, true, [](asm_xtensa_t *as, unsigned dst, unsigned lhs, unsigned rhs) {
                    asm_xtensa_op_mull(as, dst, lhs, rhs);
                }))
                return true;
            return emit_binary_reg(ins, [](asm_xtensa_t *as, unsigned rhs) {
                asm_xtensa_op_mull(as, kValue0, kValue0, rhs);
            });
        case OpCode::Div:
        case OpCode::DivI:
            if (emit_unsigned_div_pow2(ins))
                return true;
            return emit_div_mod(ins, false);
        case OpCode::Mod:
            return emit_div_mod(ins, true);
        case OpCode::BitAnd:
            if (emit_bitand_mask(ins))
                return true;
            if (emit_binary_inplace(ins, true, [](asm_xtensa_t *as, unsigned dst, unsigned lhs, unsigned rhs) {
                    asm_xtensa_op_and(as, dst, lhs, rhs);
                }))
                return true;
            return emit_binary_reg(ins, [](asm_xtensa_t *as, unsigned rhs) {
                asm_xtensa_op_and(as, kValue0, kValue0, rhs);
            });
        case OpCode::BitOr:
            if (emit_binary_inplace(ins, true, [](asm_xtensa_t *as, unsigned dst, unsigned lhs, unsigned rhs) {
                    asm_xtensa_op_or(as, dst, lhs, rhs);
                }))
                return true;
            return emit_binary_reg(ins, [](asm_xtensa_t *as, unsigned rhs) {
                asm_xtensa_op_or(as, kValue0, kValue0, rhs);
            });
        case OpCode::BitXor:
            if (emit_binary_inplace(ins, true, [](asm_xtensa_t *as, unsigned dst, unsigned lhs, unsigned rhs) {
                    asm_xtensa_op_xor(as, dst, lhs, rhs);
                }))
                return true;
            return emit_binary_reg(ins, [](asm_xtensa_t *as, unsigned rhs) {
                asm_xtensa_op_xor(as, kValue0, kValue0, rhs);
            });
        case OpCode::Shl:
            if (emit_shift_const(ins, true))
                return true;
            load_value(kValue0, ins.b);
            asm_xtensa_op_ssl(&m_as, read_value_reg(ins.c, kValue1, kValue0));
            asm_xtensa_op_sll(&m_as, kValue0, kValue0);
            store_value(ins.a, kValue0);
            return true;
        case OpCode::Shr:
            if (emit_shift_const(ins, false))
                return true;
            load_value(kValue0, ins.b);
            asm_xtensa_op_ssr(&m_as, read_value_reg(ins.c, kValue1, kValue0));
            asm_xtensa_op_srl(&m_as, kValue0, kValue0);
            store_value(ins.a, kValue0);
            return true;
        case OpCode::Neg:
        case OpCode::NegI:
            load_value(kValue0, ins.b);
            asm_xtensa_op_neg(&m_as, kValue0, kValue0);
            store_value(ins.a, kValue0);
            return true;
        case OpCode::Eq:
        case OpCode::EqI:
        case OpCode::Ne:
        case OpCode::NeI:
        case OpCode::Lt:
        case OpCode::LtI:
        case OpCode::Le:
        case OpCode::LeI:
        case OpCode::Gt:
        case OpCode::GtI:
        case OpCode::Ge:
        case OpCode::GeI:
            return emit_compare(ins);
        case OpCode::Load:
        case OpCode::Load8:
        case OpCode::Load8I32:
        case OpCode::Load16:
        case OpCode::Load16I32:
        case OpCode::Load32:
        case OpCode::Load32I32:
            return emit_load(ins);
        case OpCode::Store:
        case OpCode::Store8:
        case OpCode::Store16:
        case OpCode::Store32:
            return emit_store(ins);
        case OpCode::Jump:
            if (ins.a == (int)m_pc + 1)
                return true;
            asm_xtensa_j_label(&m_as, label_for_pc(ins.a));
            return true;
        case OpCode::JumpIfFalse:
        {
            if (ins.b == (int)m_pc + 1)
                return true;
            int32_t const_value = 0;
            if (const_i32(ins.a, const_value))
            {
                if (const_value == 0)
                    asm_xtensa_j_label(&m_as, label_for_pc(ins.b));
                return true;
            }
            const unsigned cond_reg = read_value_reg(ins.a, kValue0);
            asm_xtensa_bccz_reg_label(&m_as, ASM_XTENSA_CCZ_EQ, cond_reg, label_for_pc(ins.b));
            return true;
        }
        case OpCode::Return:
            if (ins.a >= 0)
            {
                load_value(ASM_XTENSA_REG_A2, ins.a);
                emit_cast_in_reg(ASM_XTENSA_REG_A2, m_fn.return_type);
            }
            else
            {
                emit_movi32(ASM_XTENSA_REG_A2, 0);
            }
            asm_xtensa_exit_win(&m_as);
            return true;
        default:
            return error("viper native: unsupported opcode");
        }
    }

    template <typename EmitOp>
    bool emit_binary_reg(const Instruction &ins, EmitOp emit_op)
    {
        load_value(kValue0, ins.b);
        const unsigned rhs = read_value_reg(ins.c, kValue1, kValue0);
        emit_op(&m_as, rhs);
        store_value(ins.a, kValue0);
        return true;
    }

    template <typename EmitOp>
    bool emit_binary_inplace(const Instruction &ins, bool commutative, EmitOp emit_op)
    {
        const int dst_reg = resident_reg(ins.a);
        if (dst_reg < 0 || !can_update_resident_without_cast(ins.a))
            return false;

        if (ins.a == ins.b)
        {
            const unsigned rhs = read_value_reg(ins.c, scratch_reg((unsigned)dst_reg), (unsigned)dst_reg);
            emit_op(&m_as, (unsigned)dst_reg, (unsigned)dst_reg, rhs);
            return true;
        }

        if (commutative && ins.a == ins.c)
        {
            const unsigned lhs = read_value_reg(ins.b, scratch_reg((unsigned)dst_reg), (unsigned)dst_reg);
            emit_op(&m_as, (unsigned)dst_reg, (unsigned)dst_reg, lhs);
            return true;
        }

        return false;
    }

    bool const_i32(int var, int32_t &value) const
    {
        if (var < 0 || var >= (int)m_fn.vars.size() || !m_fn.vars[(size_t)var].is_const)
            return false;
        value = (int32_t)m_fn.vars[(size_t)var].const_i;
        return true;
    }

    static bool power2_shift(uint32_t value, unsigned &shift)
    {
        if (value == 0 || (value & (value - 1U)) != 0)
            return false;
        shift = 0;
        while ((value & 1U) == 0)
        {
            value >>= 1;
            ++shift;
        }
        return shift < 32;
    }

    static bool low_mask_bit_count(uint32_t mask, unsigned &bits)
    {
        if (mask == 0 || (mask & (mask + 1U)) != 0)
            return false;
        bits = 0;
        while (mask != 0)
        {
            mask >>= 1;
            ++bits;
        }
        return bits >= 1 && bits <= 16;
    }

    bool can_update_resident_without_cast(int var) const
    {
        if (var < 0 || var >= (int)m_fn.vars.size())
            return false;
        const ValueType type = m_fn.vars[(size_t)var].type;
        return type == ValueType::I32 || type == ValueType::U32 || is_ptr_type(type);
    }

    bool emit_addi_from_src(const Instruction &ins, int src_var, int32_t imm)
    {
        if (!signed_fit_bits_local(imm, 8))
            return false;

        const int dst_reg = resident_reg(ins.a);
        const int src_reg = resident_reg(src_var);
        if (ins.a == src_var && dst_reg >= 0 && dst_reg == src_reg && can_update_resident_without_cast(ins.a))
        {
            asm_xtensa_op_addi(&m_as, (unsigned)dst_reg, (unsigned)dst_reg, imm);
            return true;
        }

        if (dst_reg >= 0 && can_update_resident_without_cast(ins.a))
        {
            const unsigned src = read_value_reg(src_var, scratch_reg((unsigned)dst_reg), (unsigned)dst_reg);
            asm_xtensa_op_addi(&m_as, (unsigned)dst_reg, src, imm);
            return true;
        }

        const unsigned src = read_value_reg(src_var, kValue0);
        asm_xtensa_op_addi(&m_as, kValue0, src, imm);
        store_value(ins.a, kValue0);
        return true;
    }

    bool emit_add_const(const Instruction &ins)
    {
        int32_t imm = 0;
        if (const_i32(ins.c, imm))
            return emit_addi_from_src(ins, ins.b, imm);
        if (const_i32(ins.b, imm))
            return emit_addi_from_src(ins, ins.c, imm);
        return false;
    }

    bool emit_sub_const(const Instruction &ins)
    {
        int32_t imm = 0;
        if (const_i32(ins.c, imm) && imm != std::numeric_limits<int32_t>::min())
            return emit_addi_from_src(ins, ins.b, -imm);
        return false;
    }

    void emit_copy_to_target(int dst_var, int src_var)
    {
        load_value(kValue0, src_var);
        store_value(dst_var, kValue0);
    }

    void emit_shift_imm_from_src(int dst_var, int src_var, unsigned shift, bool left)
    {
        shift &= 31U;
        if (shift == 0)
        {
            emit_copy_to_target(dst_var, src_var);
            return;
        }

        const int dst_reg = resident_reg(dst_var);
        if (dst_reg >= 0 && can_update_resident_without_cast(dst_var))
        {
            const unsigned src = read_value_reg(src_var, scratch_reg((unsigned)dst_reg), (unsigned)dst_reg);
            mov_reg((unsigned)dst_reg, src);
            asm_xtensa_op_movi_n(&m_as, kScratch, (int)shift);
            if (left)
                asm_xtensa_op_ssl(&m_as, kScratch);
            else
                asm_xtensa_op_ssr(&m_as, kScratch);
            if (left)
                asm_xtensa_op_sll(&m_as, (unsigned)dst_reg, (unsigned)dst_reg);
            else
                asm_xtensa_op_srl(&m_as, (unsigned)dst_reg, (unsigned)dst_reg);
            return;
        }

        load_value(kValue0, src_var);
        asm_xtensa_op_movi_n(&m_as, kValue1, (int)shift);
        if (left)
            asm_xtensa_op_ssl(&m_as, kValue1);
        else
            asm_xtensa_op_ssr(&m_as, kValue1);
        if (left)
            asm_xtensa_op_sll(&m_as, kValue0, kValue0);
        else
            asm_xtensa_op_srl(&m_as, kValue0, kValue0);
        store_value(dst_var, kValue0);
    }

    bool emit_shift_const(const Instruction &ins, bool left)
    {
        int32_t imm = 0;
        if (!const_i32(ins.c, imm))
            return false;
        emit_shift_imm_from_src(ins.a, ins.b, (uint32_t)imm, left);
        return true;
    }

    bool emit_mul_pow2(const Instruction &ins)
    {
        int32_t imm = 0;
        int src_var = -1;
        if (const_i32(ins.c, imm))
            src_var = ins.b;
        else if (const_i32(ins.b, imm))
            src_var = ins.c;
        else
            return false;

        unsigned shift = 0;
        if (!power2_shift((uint32_t)imm, shift))
            return false;
        emit_shift_imm_from_src(ins.a, src_var, shift, true);
        return true;
    }

    bool emit_unsigned_div_pow2(const Instruction &ins)
    {
        int32_t imm = 0;
        if (!const_i32(ins.c, imm))
            return false;

        const bool use_unsigned = native_unsigned_compare(m_fn.vars[ins.b].type, m_fn.vars[ins.c].type) ||
                                  m_fn.vars[ins.a].type == ValueType::U32;
        if (!use_unsigned)
            return false;

        unsigned shift = 0;
        if (!power2_shift((uint32_t)imm, shift))
            return false;
        emit_shift_imm_from_src(ins.a, ins.b, shift, false);
        return true;
    }

    bool emit_bitand_mask(const Instruction &ins)
    {
        int32_t mask = 0;
        int src_var = -1;
        if (const_i32(ins.c, mask))
            src_var = ins.b;
        else if (const_i32(ins.b, mask))
            src_var = ins.c;
        else
            return false;

        const uint32_t umask = (uint32_t)mask;
        if (umask == 0)
        {
            emit_movi32(kValue0, 0);
            store_value(ins.a, kValue0);
            return true;
        }
        if (umask == UINT32_MAX)
        {
            emit_copy_to_target(ins.a, src_var);
            return true;
        }

        unsigned bits = 0;
        if (low_mask_bit_count(umask, bits))
        {
            emit_extui_to_target(ins.a, src_var, bits);
            return true;
        }
        return false;
    }

    void emit_extui_to_target(int dst_var, int src_var, unsigned bits)
    {
        const int dst_reg = resident_reg(dst_var);
        if (dst_reg >= 0 && can_update_resident_without_cast(dst_var))
        {
            const unsigned src = read_value_reg(src_var, scratch_reg((unsigned)dst_reg), (unsigned)dst_reg);
            asm_xtensa_op_extui(&m_as, (unsigned)dst_reg, src, 0, bits);
            return;
        }

        const unsigned src = read_value_reg(src_var, kValue0);
        asm_xtensa_op_extui(&m_as, kValue0, src, 0, bits);
        store_value(dst_var, kValue0);
    }

    /**
     * @brief Emit ESP32-S3 DIV32 integer division or modulo instructions.
     */
    bool emit_div_mod(const Instruction &ins, bool is_mod)
    {
        load_value(kValue0, ins.b);
        const unsigned rhs = read_value_reg(ins.c, kValue1, kValue0);
        const bool use_unsigned = native_unsigned_compare(m_fn.vars[ins.b].type, m_fn.vars[ins.c].type) ||
                                  m_fn.vars[ins.a].type == ValueType::U32;
        if (is_mod)
        {
            if (use_unsigned)
                asm_xtensa_op_remu(&m_as, kValue0, kValue0, rhs);
            else
                asm_xtensa_op_rems(&m_as, kValue0, kValue0, rhs);
        }
        else
        {
            if (use_unsigned)
                asm_xtensa_op_quou(&m_as, kValue0, kValue0, rhs);
            else
                asm_xtensa_op_quos(&m_as, kValue0, kValue0, rhs);
        }
        store_value(ins.a, kValue0);
        return true;
    }

    bool emit_compare(const Instruction &ins)
    {
        const bool use_unsigned = native_unsigned_compare(m_fn.vars[ins.b].type, m_fn.vars[ins.c].type);
        unsigned cond = ASM_XTENSA_CC_EQ;
        int lhs = ins.b;
        int rhs = ins.c;

        switch (ins.op)
        {
        case OpCode::Eq:
        case OpCode::EqI:
            cond = ASM_XTENSA_CC_EQ;
            break;
        case OpCode::Ne:
        case OpCode::NeI:
            cond = ASM_XTENSA_CC_NE;
            break;
        case OpCode::Lt:
        case OpCode::LtI:
            cond = use_unsigned ? ASM_XTENSA_CC_LTU : ASM_XTENSA_CC_LT;
            break;
        case OpCode::Le:
        case OpCode::LeI:
            cond = use_unsigned ? ASM_XTENSA_CC_GEU : ASM_XTENSA_CC_GE;
            lhs = ins.c;
            rhs = ins.b;
            break;
        case OpCode::Gt:
        case OpCode::GtI:
            cond = use_unsigned ? ASM_XTENSA_CC_LTU : ASM_XTENSA_CC_LT;
            lhs = ins.c;
            rhs = ins.b;
            break;
        case OpCode::Ge:
        case OpCode::GeI:
            cond = use_unsigned ? ASM_XTENSA_CC_GEU : ASM_XTENSA_CC_GE;
            break;
        default:
            return error("viper native: invalid compare opcode");
        }

        const unsigned lhs_reg = read_value_reg(lhs, kValue0);
        const unsigned rhs_reg = read_value_reg(rhs, kValue1, lhs_reg);
        const unsigned dst_reg = scratch_reg(lhs_reg, rhs_reg, kScratch);
        asm_xtensa_setcc_reg_reg_reg(&m_as, cond, dst_reg, lhs_reg, rhs_reg);
        store_value(ins.a, dst_reg);
        return true;
    }

    bool emit_load(const Instruction &ins)
    {
        if (ins.type == ValueType::PtrF32)
            return error("viper native: f32 pointer load is not supported yet");
        const size_t elem_size = ptr_elem_size(ins.type);
        if (ins.c >= 0 && ins.c < (int)m_fn.vars.size() && m_fn.vars[(size_t)ins.c].is_const)
        {
            const unsigned base = read_value_reg(ins.b, kValue0);
            const unsigned dst = load_result_reg(ins.a, base);
            emit_load_reg_offset(dst, base, (uint32_t)m_fn.vars[(size_t)ins.c].const_i,
                                 ptr_operation_size(elem_size));
            if ((int)dst != resident_reg(ins.a))
                store_value(ins.a, dst);
            return true;
        }

        const unsigned base = read_value_reg(ins.b, kValue0);
        const unsigned index = read_value_reg(ins.c, kValue1, base);
        const unsigned addr = scratch_reg(base, index);
        emit_index_addr(addr, base, index, elem_size);
        const unsigned dst = load_result_reg(ins.a, addr);

        if (elem_size == 1)
            asm_xtensa_op_l8ui(&m_as, dst, addr, 0);
        else if (elem_size == 2)
            asm_xtensa_op_l16ui(&m_as, dst, addr, 0);
        else
            asm_xtensa_op_l32i_n(&m_as, dst, addr, 0);
        if ((int)dst != resident_reg(ins.a))
            store_value(ins.a, dst);
        return true;
    }

    unsigned load_result_reg(int var, unsigned avoid0)
    {
        const int reg = resident_reg(var);
        if (reg >= 0 && can_update_resident_without_cast(var) && (unsigned)reg != avoid0)
            return (unsigned)reg;
        return scratch_reg(avoid0);
    }

    bool emit_store(const Instruction &ins)
    {
        if (ins.type == ValueType::PtrF32)
            return error("viper native: f32 pointer store is not supported yet");
        const size_t elem_size = ptr_elem_size(ins.type);
        if (ins.b >= 0 && ins.b < (int)m_fn.vars.size() && m_fn.vars[(size_t)ins.b].is_const)
        {
            const unsigned base = read_value_reg(ins.a, kValue0);
            const unsigned value = read_value_reg(ins.c, kValue3, base);
            emit_store_reg_offset(value, base, (uint32_t)m_fn.vars[(size_t)ins.b].const_i,
                                  ptr_operation_size(elem_size));
            return true;
        }

        const unsigned base = read_value_reg(ins.a, kValue0);
        const unsigned index = read_value_reg(ins.b, kValue1, base);
        const unsigned value = read_value_reg(ins.c, kValue3, base, index);
        const unsigned addr = scratch_reg(base, index, value);
        emit_index_addr(addr, base, index, elem_size);

        if (elem_size == 1)
            asm_xtensa_op_s8i(&m_as, value, addr, 0);
        else if (elem_size == 2)
            asm_xtensa_op_s16i(&m_as, value, addr, 0);
        else
            asm_xtensa_op_s32i_n(&m_as, value, addr, 0);
        return true;
    }

    void emit_index_addr(unsigned dst, unsigned base, unsigned index, size_t elem_size)
    {
        if (elem_size == 1)
            asm_xtensa_op_add_n(&m_as, dst, base, index);
        else if (elem_size == 2)
            asm_xtensa_op_addx2(&m_as, dst, index, base);
        else
            asm_xtensa_op_addx4(&m_as, dst, index, base);
    }

    /**
     * @brief Convert pointer element width to Xtensa load/store offset units.
     */
    static unsigned ptr_operation_size(size_t elem_size)
    {
        if (elem_size == 1)
            return 0;
        if (elem_size == 2)
            return 1;
        return 2;
    }
};

/**
 * @brief Run literal collection, multi-pass branch sizing, final native compilation, and machine-code installation.
 */
bool compile_native(CompiledFunction &fn, ViperError &err)
{
    err.clear();
    ViperAllocScope alloc_scope;
    enter_viper_alloc_scope(alloc_scope);
    if (setjmp(alloc_scope.env) != 0)
    {
        err.set(alloc_scope.error ? alloc_scope.error : "viper native: out of memory");
        leave_viper_alloc_scope(alloc_scope);
        return false;
    }

    const size_t label_count = fn.code.size() + 1;
    ViperInternalVector<uint32_t> labels(label_count, ASM_XTENSA_LABEL_UNSET);
    ViperInternalVector<uint32_t> literals(kMaxNativeLiterals, 0);
    ViperInternalVector<uint8_t> branch_long(label_count, 0);

    asm_xtensa_t collector;
    asm_xtensa_init(&collector, nullptr, 0, labels.data(), (uint32_t)labels.size(),
                    literals.data(), (uint32_t)literals.size(), 0, true,
                    nullptr, 0, true, false);
    ViperScopedObject<NativeCompiler> collector_compiler(make_viper_work_object<NativeCompiler>(fn, collector));
    if (!collector_compiler.get())
    {
        err.set("viper native: compiler state allocation failed");
        leave_viper_alloc_scope(alloc_scope);
        return false;
    }
    if (!collector_compiler->emit_all(err))
    {
        leave_viper_alloc_scope(alloc_scope);
        return false;
    }
    collector_compiler.reset();
    if (collector.error)
    {
        err.set(collector.error);
        leave_viper_alloc_scope(alloc_scope);
        return false;
    }
    const uint32_t literal_count = collector.literal_count;

    auto run_sizing_pass = [&](asm_xtensa_t &sizing, bool reset_labels) -> bool
    {
        asm_xtensa_init(&sizing, nullptr, 0, labels.data(), (uint32_t)labels.size(),
                        literals.data(), (uint32_t)literals.size(), literal_count, false,
                        branch_long.data(), (uint32_t)branch_long.size(), reset_labels, false);
        ViperScopedObject<NativeCompiler> sizing_compiler(make_viper_work_object<NativeCompiler>(fn, sizing));
        if (!sizing_compiler.get())
        {
            err.set("viper native: compiler state allocation failed");
            return false;
        }
        if (!sizing_compiler->emit_all(err))
            return false;
        sizing_compiler.reset();
        if (sizing.error)
        {
            err.set(sizing.error);
            return false;
        }
        if (sizing.branch_count > branch_long.size())
        {
            err.set("viper native: too many branches");
            return false;
        }
        return true;
    };

    constexpr size_t kMaxBranchRelaxPasses = 32;
    size_t code_size = 0;
    size_t previous_code_size = 0;
    uint32_t expected_branch_count = UINT32_MAX;
    bool branch_relaxed = false;

    for (size_t pass = 0; pass < kMaxBranchRelaxPasses; ++pass)
    {
        asm_xtensa_t sizing;
        if (!run_sizing_pass(sizing, pass == 0))
        {
            leave_viper_alloc_scope(alloc_scope);
            return false;
        }

        if (expected_branch_count == UINT32_MAX)
        {
            expected_branch_count = sizing.branch_count;
        }
        else if (expected_branch_count != sizing.branch_count)
        {
            err.set("viper native: unstable branch count");
            leave_viper_alloc_scope(alloc_scope);
            return false;
        }

        code_size = asm_xtensa_get_offset(&sizing);
        if (code_size == 0 || code_size > kMaxNativeCodeBytes)
        {
            err.set("viper native: code size exceeds limit");
            leave_viper_alloc_scope(alloc_scope);
            return false;
        }

        if (pass > 0 && !sizing.branch_changed && code_size == previous_code_size)
        {
            branch_relaxed = true;
            break;
        }

        previous_code_size = code_size;
    }

    if (!branch_relaxed)
    {
        for (size_t i = 0; i < branch_long.size(); ++i)
            branch_long[i] = 1;

        asm_xtensa_t sizing;
        if (!run_sizing_pass(sizing, true))
        {
            leave_viper_alloc_scope(alloc_scope);
            return false;
        }
        if (expected_branch_count != UINT32_MAX && expected_branch_count != sizing.branch_count)
        {
            err.set("viper native: unstable branch count");
            leave_viper_alloc_scope(alloc_scope);
            return false;
        }

        code_size = asm_xtensa_get_offset(&sizing);
        if (code_size == 0 || code_size > kMaxNativeCodeBytes)
        {
            err.set("viper native: code size exceeds limit");
            leave_viper_alloc_scope(alloc_scope);
            return false;
        }
    }

    ViperInternalVector<uint8_t> code(code_size);
    asm_xtensa_t emitter;
    asm_xtensa_init(&emitter, code.data(), code.size(), labels.data(), (uint32_t)labels.size(),
                    literals.data(), (uint32_t)literals.size(), literal_count, false,
                    branch_long.data(), (uint32_t)branch_long.size(), false, true);
    ViperScopedObject<NativeCompiler> emitter_compiler(make_viper_work_object<NativeCompiler>(fn, emitter));
    if (!emitter_compiler.get())
    {
        err.set("viper native: compiler state allocation failed");
        leave_viper_alloc_scope(alloc_scope);
        return false;
    }
    if (!emitter_compiler->emit_all(err))
    {
        leave_viper_alloc_scope(alloc_scope);
        return false;
    }
    emitter_compiler.reset();
    if (emitter.error)
    {
        err.set(emitter.error);
        leave_viper_alloc_scope(alloc_scope);
        return false;
    }
    if (expected_branch_count != UINT32_MAX && expected_branch_count != emitter.branch_count)
    {
        err.set("viper native: unstable branch count");
        leave_viper_alloc_scope(alloc_scope);
        return false;
    }
    if (asm_xtensa_get_offset(&emitter) != code_size)
    {
        err.set("viper native: unstable code size");
        leave_viper_alloc_scope(alloc_scope);
        return false;
    }
    const uint32_t entry_offset = asm_xtensa_get_entry_offset(&emitter);
    if (entry_offset >= code_size)
    {
        err.set("viper native: invalid entry offset");
        leave_viper_alloc_scope(alloc_scope);
        return false;
    }

    const bool ok = install_native_code(fn, code, entry_offset, err);
    leave_viper_alloc_scope(alloc_scope);
    return ok;
}

} // namespace lua_nodemcu_viper::internal
