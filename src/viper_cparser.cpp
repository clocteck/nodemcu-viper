#include "viper_internal.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace lua_nodemcu_viper::internal
{

enum class TokenKind : uint8_t
{
    End,
    Name,
    Number,
    Symbol,
    KwReturn,
    KwFor,
    KwIf,
    KwElse,
};

struct TextRef
{
    const char *data = nullptr;
    size_t len = 0;

    /**
     * @brief Determine whether the source slice is empty.
     */
    bool empty() const
    {
        return len == 0;
    }

    /**
     * @brief Compare a source slice with constant text without allocating a string.
     */
    bool equals(const char *literal) const
    {
        return viper_text_equals(data, len, literal);
    }
};

struct Token
{
    TokenKind kind = TokenKind::End;
    TextRef text;
    int64_t number = 0;
    float fnumber = 0.0f;
    bool is_float = false;
    size_t pos = 0;
};

/**
 * @brief Small lexer that only parses the Viper C subset.
 */
class Lexer
{
  public:
    explicit Lexer(const char *src) : m_src(src ? src : ""), m_len(std::strlen(m_src)) {}

    Token next()
    {
        skip_space_and_comments();
        Token t;
        t.pos = m_pos;
        if (m_pos >= m_len)
        {
            t.kind = TokenKind::End;
            return t;
        }

        const char ch = m_src[m_pos];
        if (std::isalpha((unsigned char)ch) || ch == '_')
        {
            const size_t start = m_pos++;
            while (m_pos < m_len)
            {
                const char c = m_src[m_pos];
                if (!std::isalnum((unsigned char)c) && c != '_')
                    break;
                ++m_pos;
            }
            t.text = {m_src + start, m_pos - start};
            t.kind = keyword_kind(t.text);
            return t;
        }

        if (std::isdigit((unsigned char)ch))
        {
            const size_t start = m_pos;
            bool is_float = false;
            if (ch == '0' && m_pos + 1 < m_len && (m_src[m_pos + 1] == 'x' || m_src[m_pos + 1] == 'X'))
            {
                m_pos += 2;
                while (m_pos < m_len && std::isxdigit((unsigned char)m_src[m_pos]))
                    ++m_pos;
            }
            else
            {
                while (m_pos < m_len && std::isdigit((unsigned char)m_src[m_pos]))
                    ++m_pos;
                if (m_pos < m_len && m_src[m_pos] == '.')
                {
                    is_float = true;
                    ++m_pos;
                    while (m_pos < m_len && std::isdigit((unsigned char)m_src[m_pos]))
                        ++m_pos;
                }
                if (m_pos < m_len && (m_src[m_pos] == 'e' || m_src[m_pos] == 'E'))
                {
                    is_float = true;
                    ++m_pos;
                    if (m_pos < m_len && (m_src[m_pos] == '+' || m_src[m_pos] == '-'))
                        ++m_pos;
                    while (m_pos < m_len && std::isdigit((unsigned char)m_src[m_pos]))
                        ++m_pos;
                }
            }
            t.kind = TokenKind::Number;
            t.text = {m_src + start, m_pos - start};
            t.is_float = is_float;
            if (is_float)
            {
                t.fnumber = std::strtof(m_src + start, nullptr);
                t.number = (int64_t)t.fnumber;
            }
            else
            {
                t.number = std::strtoll(m_src + start, nullptr, 0);
                t.fnumber = (float)t.number;
            }
            return t;
        }

        if (m_pos + 1 < m_len)
        {
            if (is_two_char_symbol(m_src[m_pos], m_src[m_pos + 1]))
            {
                t.text = {m_src + m_pos, 2};
                m_pos += 2;
                t.kind = TokenKind::Symbol;
                return t;
            }
        }

        t.kind = TokenKind::Symbol;
        t.text = {m_src + m_pos, 1};
        ++m_pos;
        return t;
    }

  private:
    const char *m_src;
    size_t m_len = 0;
    size_t m_pos = 0;

    /**
     * @brief Skip whitespace and C-style comments.
     */
    void skip_space_and_comments()
    {
        while (m_pos < m_len)
        {
            while (m_pos < m_len && std::isspace((unsigned char)m_src[m_pos]))
                ++m_pos;

            if (m_pos + 1 < m_len && m_src[m_pos] == '/' && m_src[m_pos + 1] == '/')
            {
                while (m_pos < m_len && m_src[m_pos] != '\n')
                    ++m_pos;
                continue;
            }
            if (m_pos + 1 < m_len && m_src[m_pos] == '/' && m_src[m_pos + 1] == '*')
            {
                m_pos += 2;
                while (m_pos + 1 < m_len && !(m_src[m_pos] == '*' && m_src[m_pos + 1] == '/'))
                    ++m_pos;
                if (m_pos + 1 < m_len)
                    m_pos += 2;
                continue;
            }
            break;
        }
    }

    /**
     * @brief Convert keyword text to a token type.
     */
    static TokenKind keyword_kind(TextRef text)
    {
        if (text.equals("return"))
            return TokenKind::KwReturn;
        if (text.equals("for"))
            return TokenKind::KwFor;
        if (text.equals("if"))
            return TokenKind::KwIf;
        if (text.equals("else"))
            return TokenKind::KwElse;
        return TokenKind::Name;
    }

    /**
     * @brief Determine whether two consecutive characters form a C-like two-character symbol.
     */
    static bool is_two_char_symbol(char a, char b)
    {
        return (a == '=' && b == '=') || (a == '!' && b == '=') ||
               (a == '~' && b == '=') || (a == '<' && (b == '=' || b == '<')) ||
               (a == '>' && (b == '=' || b == '>')) || (a == '+' && (b == '+' || b == '=')) ||
               (a == '-' && (b == '-' || b == '=')) || (a == '*' && b == '=') ||
               (a == '/' && b == '=') || (a == '%' && b == '=') ||
               (a == '&' && b == '&') || (a == '|' && b == '|');
    }
};


/**
 * @brief C-like subset parser that compiles C function source directly to Viper IR bytecode.
 */
class CParser
{
  public:
    CParser(const char *src, bool unsafe_ptr, bool bounds_check)
        : m_unsafe_ptr(unsafe_ptr), m_bounds_check(bounds_check)
    {
        Lexer lex(src);
        while (true)
        {
            Token t = lex.next();
            m_tokens.push_back(t);
            if (t.kind == TokenKind::End)
                break;
        }
        m_end = m_tokens.back();
        m_limit = m_tokens.size() - 1;
    }

    /**
     * @brief Parse a complete C-like function and write it into a compiled function object.
     */
    bool parse(CompiledFunction &fn, ViperError &err)
    {
        m_fn = &fn;
        m_error.clear();
        fn.unsafe_ptr = m_unsafe_ptr;
        fn.bounds_check = m_bounds_check;

        ValueType return_type = ValueType::Void;
        if (!parse_c_type(return_type, true))
            return fail(err);
        fn.return_type = return_type;

        if (cur().kind != TokenKind::Name)
            return fail_with(err, "viper.compile_c: expected function name");
        advance();

        if (!parse_function_params())
            return fail(err);
        if (!parse_compound_block())
            return fail(err);
        if (!at_end())
            return fail_with(err, "viper.compile_c: unexpected token after function body");

        if (emit(OpCode::Return, -1, 0, 0, 0, ValueType::Void) < 0)
            return fail(err);
        return true;
    }

  private:
    ViperInternalVector<Token> m_tokens;
    ViperInternalVector<TextRef> m_var_names;
    Token m_end;
    size_t m_pos = 0;
    size_t m_limit = 0;
    CompiledFunction *m_fn = nullptr;
    bool m_unsafe_ptr = false;
    bool m_bounds_check = true;
    ViperError m_error;

    struct BranchList
    {
        ViperInternalVector<int> jumps;
    };

    /**
     * @brief Get the current token, returning the End sentinel when the subrange ends.
     */
    const Token &cur() const
    {
        return m_pos < m_limit ? m_tokens[m_pos] : m_end;
    }

    /**
     * @brief Determine whether the current parse range has ended.
     */
    bool at_end() const
    {
        return m_pos >= m_limit || cur().kind == TokenKind::End;
    }

    /**
     * @brief Return the most recent C-like parse error.
     */
    bool fail(ViperError &err)
    {
        err.set(m_error.c_str("viper.compile_c: parser error"));
        return false;
    }

    /**
     * @brief Set a parse error and return failure.
     */
    bool fail_with(ViperError &err, const char *msg)
    {
        error(msg);
        return fail(err);
    }

    /**
     * @brief Consume the current token.
     */
    void advance()
    {
        if (m_pos < m_limit)
            ++m_pos;
    }

    /**
     * @brief Test and consume the specified symbol.
     */
    bool match_symbol(const char *text)
    {
        if (cur().kind == TokenKind::Symbol && cur().text.equals(text))
        {
            advance();
            return true;
        }
        return false;
    }

    /**
     * @brief Test and consume the specified keyword.
     */
    bool match_kind(TokenKind kind)
    {
        if (cur().kind == kind)
        {
            advance();
            return true;
        }
        return false;
    }

    /**
     * @brief Test and consume the specified name.
     */
    bool match_name(const char *text)
    {
        if (cur().kind == TokenKind::Name && cur().text.equals(text))
        {
            advance();
            return true;
        }
        return false;
    }

    /**
     * @brief Require the current token to be the specified symbol.
     */
    bool expect_symbol(const char *text, const char *msg)
    {
        if (!match_symbol(text))
            return error(msg);
        return true;
    }

    /**
     * @brief Record a C-like parse error with a source offset.
     */
    bool error(const char *msg)
    {
        m_error.set_near(msg, cur().pos);
        return false;
    }

    /**
     * @brief Determine whether the specified position may be the start of a C type.
     */
    bool token_starts_c_type(size_t pos) const
    {
        while (pos < m_limit && m_tokens[pos].kind == TokenKind::Name &&
               (m_tokens[pos].text.equals("const") || m_tokens[pos].text.equals("volatile")))
        {
            ++pos;
        }
        if (pos >= m_limit || m_tokens[pos].kind != TokenKind::Name)
            return false;
        ValueType ignored = ValueType::Void;
        const TextRef name = m_tokens[pos].text;
        return parse_c_scalar_type_name(name.data, name.len, ignored) || name.equals("signed") ||
               name.equals("unsigned") || name.equals("char") || name.equals("short") || name.equals("int");
    }

    /**
     * @brief Determine whether the current token may start a C type.
     */
    bool starts_c_type() const
    {
        return token_starts_c_type(m_pos);
    }

    /**
     * @brief Skip C type qualifiers.
     */
    void skip_c_qualifiers()
    {
        while (cur().kind == TokenKind::Name && (cur().text.equals("const") || cur().text.equals("volatile")))
            advance();
    }

    /**
     * @brief Parse a C scalar or pointer type.
     */
    bool parse_c_type(ValueType &type, bool allow_void)
    {
        skip_c_qualifiers();
        if (cur().kind != TokenKind::Name)
            return error("viper.compile_c: expected type");

        bool is_unsigned = false;
        bool has_sign_prefix = false;
        if (cur().text.equals("unsigned") || cur().text.equals("signed"))
        {
            is_unsigned = cur().text.equals("unsigned");
            has_sign_prefix = true;
            advance();
            skip_c_qualifiers();
        }

        if (cur().kind != TokenKind::Name)
            return error("viper.compile_c: expected type");

        const TextRef name = cur().text;
        if (has_sign_prefix)
        {
            if (name.equals("char"))
                type = is_unsigned ? ValueType::U8 : ValueType::I8;
            else if (name.equals("short"))
                type = is_unsigned ? ValueType::U16 : ValueType::I16;
            else if (name.equals("int"))
                type = is_unsigned ? ValueType::U32 : ValueType::I32;
            else
                return error("viper.compile_c: unsupported signed type");
            advance();
        }
        else if (name.equals("char"))
        {
            type = ValueType::I8;
            advance();
        }
        else if (name.equals("short"))
        {
            type = ValueType::I16;
            advance();
        }
        else if (name.equals("int"))
        {
            type = ValueType::I32;
            advance();
        }
        else
        {
            if (!parse_c_scalar_type_name(name.data, name.len, type))
                return error("viper.compile_c: unknown type");
            advance();
        }

        skip_c_qualifiers();
        while (match_symbol("*"))
        {
            ValueType ptr_type = ValueType::Ptr8;
            if (!pointer_type_for_scalar(type, ptr_type))
                return error("viper.compile_c: unsupported pointer type");
            type = ptr_type;
            skip_c_qualifiers();
        }

        if (type == ValueType::Void && !allow_void)
            return error("viper.compile_c: void value is not allowed here");
        return true;
    }

    /**
     * @brief Parse a C function parameter list.
     */
    bool parse_function_params()
    {
        if (!expect_symbol("(", "viper.compile_c: expected '(' after function name"))
            return false;

        if (match_symbol(")"))
        {
            m_fn->param_count = 0;
            return true;
        }

        if (cur().kind == TokenKind::Name && cur().text.equals("void") &&
            m_pos + 1 < m_limit && m_tokens[m_pos + 1].kind == TokenKind::Symbol &&
            m_tokens[m_pos + 1].text.equals(")"))
        {
            advance();
            advance();
            m_fn->param_count = 0;
            return true;
        }

        while (true)
        {
            ValueType type = ValueType::I32;
            if (!parse_c_type(type, false))
                return false;
            if (cur().kind != TokenKind::Name)
                return error("viper.compile_c: expected parameter name");

            Variable var;
            const TextRef name = cur().text;
            var.type = type;
            var.is_param = true;
            advance();
            if (add_var(var, name) < 0)
                return false;

            if (match_symbol(")"))
                break;
            if (!expect_symbol(",", "viper.compile_c: expected ',' between parameters"))
                return false;
        }
        m_fn->param_count = (int)m_fn->vars.size();
        return true;
    }

    /**
     * @brief Parse a `{ ... }` compound statement block.
     */
    bool parse_compound_block()
    {
        if (!expect_symbol("{", "viper.compile_c: expected '{'"))
            return false;
        while (!at_end() && !(cur().kind == TokenKind::Symbol && cur().text.equals("}")))
        {
            if (!parse_statement())
                return false;
        }
        return expect_symbol("}", "viper.compile_c: expected '}'");
    }

    /**
     * @brief Parse a single C-like statement.
     */
    bool parse_statement()
    {
        if (match_symbol(";"))
            return true;
        if (cur().kind == TokenKind::Symbol && cur().text.equals("{"))
            return parse_compound_block();
        if (match_kind(TokenKind::KwReturn))
            return parse_return();
        if (match_kind(TokenKind::KwIf))
            return parse_if();
        if (match_kind(TokenKind::KwFor))
            return parse_for();
        if (starts_c_type())
            return parse_declaration(true);
        if (cur().kind == TokenKind::Name)
            return parse_assignment(true);
        return error("viper.compile_c: unsupported statement");
    }

    /**
     * @brief Parse a local variable declaration.
     */
    bool parse_declaration(bool need_semicolon)
    {
        ValueType type = ValueType::I32;
        if (!parse_c_type(type, false))
            return false;
        if (cur().kind != TokenKind::Name)
            return error("viper.compile_c: expected variable name");

        Variable var;
        const TextRef name = cur().text;
        var.type = type;
        advance();
        const int reg = add_var(var, name);
        if (reg < 0)
            return false;

        int rhs = temp_const(0, type);
        if (rhs < 0)
            return false;
        if (match_symbol("="))
        {
            rhs = parse_expr();
            if (rhs < 0)
                return false;
        }
        if (emit_mov(reg, rhs, type) < 0)
            return false;

        if (need_semicolon && !expect_symbol(";", "viper.compile_c: expected ';' after declaration"))
            return false;
        return true;
    }

    /**
     * @brief Parse assignment, compound assignment, and increment/decrement statements.
     */
    bool parse_assignment(bool need_semicolon)
    {
        if (cur().kind != TokenKind::Name)
            return error("viper.compile_c: expected assignment target");

        const int var = find_var(cur().text);
        if (var < 0)
            return error("viper.compile_c: assignment to unknown variable");
        advance();

        bool indexed = false;
        int index = -1;
        ValueType target_type = m_fn->vars[var].type;
        if (match_symbol("["))
        {
            if (!is_ptr_type(m_fn->vars[var].type))
                return error("viper.compile_c: indexed assignment target is not a pointer");
            indexed = true;
            target_type = ptr_value_type(m_fn->vars[var].type);
            index = parse_expr();
            if (index < 0)
                return false;
            if (!expect_symbol("]", "viper.compile_c: expected ']' after index"))
                return false;
        }

        if (cur().kind == TokenKind::Symbol && (cur().text.equals("++") || cur().text.equals("--")))
        {
            const bool inc = cur().text.equals("++");
            advance();
            const int one = temp_const(1, target_type);
            if (one < 0)
                return false;
            const int cur_value = indexed ? emit_index_load(var, index) : var;
            if (cur_value < 0)
                return false;
            const int value = binary(inc ? OpCode::Add : OpCode::Sub, cur_value, one);
            if (value < 0)
                return false;
            if (!emit_target_store(var, index, indexed, value, target_type))
                return false;
            if (need_semicolon && !expect_symbol(";", "viper.compile_c: expected ';' after assignment"))
                return false;
            return true;
        }

        if (cur().kind != TokenKind::Symbol)
            return error("viper.compile_c: expected assignment operator");
        const TextRef op = cur().text;
        if (!op.equals("=") && !op.equals("+=") && !op.equals("-=") &&
            !op.equals("*=") && !op.equals("/=") && !op.equals("%="))
            return error("viper.compile_c: unsupported assignment operator");
        advance();

        int rhs = parse_expr();
        if (rhs < 0)
            return false;
        if (!op.equals("="))
        {
            const int cur_value = indexed ? emit_index_load(var, index) : var;
            if (cur_value < 0)
                return false;
            rhs = binary(assignment_binary_opcode(op), cur_value, rhs);
            if (rhs < 0)
                return false;
        }
        if (!emit_target_store(var, index, indexed, rhs, target_type))
            return false;

        if (need_semicolon && !expect_symbol(";", "viper.compile_c: expected ';' after assignment"))
            return false;
        return true;
    }

    /**
     * @brief Parse a C-like if/else statement.
     */
    bool parse_if()
    {
        if (!expect_symbol("(", "viper.compile_c: expected '(' after if"))
            return false;
        BranchList false_jumps;
        if (!parse_condition(false_jumps))
            return false;
        if (!expect_symbol(")", "viper.compile_c: expected ')' after if condition"))
            return false;

        if (!parse_statement())
            return false;

        if (match_kind(TokenKind::KwElse))
        {
            const int jmp_end = emit(OpCode::Jump, 0, 0, 0, 0, ValueType::Void);
            if (jmp_end < 0)
                return false;
            patch_jumps(false_jumps, (int)m_fn->code.size());
            if (!parse_statement())
                return false;
            m_fn->code[jmp_end].a = (int)m_fn->code.size();
            return true;
        }

        patch_jumps(false_jumps, (int)m_fn->code.size());
        return true;
    }

    /**
     * @brief Parse a C-like for(init; cond; post) loop.
     */
    bool parse_for()
    {
        if (!expect_symbol("(", "viper.compile_c: expected '(' after for"))
            return false;

        if (!match_symbol(";"))
        {
            if (starts_c_type())
            {
                if (!parse_declaration(false))
                    return false;
            }
            else if (!parse_assignment(false))
            {
                return false;
            }
            if (!expect_symbol(";", "viper.compile_c: expected ';' after for init"))
                return false;
        }

        const int loop_start = (int)m_fn->code.size();
        BranchList false_jumps;
        bool has_cond = false;
        if (!match_symbol(";"))
        {
            has_cond = true;
            if (!parse_condition(false_jumps))
                return false;
            if (!expect_symbol(";", "viper.compile_c: expected ';' after for condition"))
                return false;
        }

        const size_t post_begin = m_pos;
        size_t post_end = post_begin;
        if (!skip_for_post(post_end))
            return false;

        if (!parse_statement())
            return false;
        if (!parse_post_range(post_begin, post_end))
            return false;

        if (emit(OpCode::Jump, loop_start, 0, 0, 0, ValueType::Void) < 0)
            return false;
        if (has_cond)
            patch_jumps(false_jumps, (int)m_fn->code.size());
        return true;
    }

    /**
     * @brief Skip the for post expression and return the token position before the closing parenthesis.
     */
    bool skip_for_post(size_t &post_end)
    {
        int depth = 0;
        while (!at_end())
        {
            if (cur().kind == TokenKind::Symbol && cur().text.equals("("))
            {
                ++depth;
                advance();
                continue;
            }
            if (cur().kind == TokenKind::Symbol && cur().text.equals(")"))
            {
                if (depth == 0)
                {
                    post_end = m_pos;
                    advance();
                    return true;
                }
                --depth;
                advance();
                continue;
            }
            advance();
        }
        return error("viper.compile_c: expected ')' after for post");
    }

    /**
     * @brief Generate for post bytecode inside a saved token subrange.
     */
    bool parse_post_range(size_t start, size_t end)
    {
        if (start >= end)
            return true;

        const size_t saved_pos = m_pos;
        const size_t saved_limit = m_limit;
        m_pos = start;
        m_limit = end;
        const bool ok = parse_assignment(false) &&
                        (at_end() || error("viper.compile_c: unsupported for post expression"));
        m_pos = saved_pos;
        m_limit = saved_limit;
        return ok;
    }

    /**
     * @brief Parse a C-like return statement.
     */
    bool parse_return()
    {
        if (match_symbol(";"))
        {
            if (m_fn->return_type != ValueType::Void)
                return error("viper.compile_c: non-void function must return a value");
            return emit(OpCode::Return, -1, 0, 0, 0, ValueType::Void) >= 0;
        }

        if (m_fn->return_type == ValueType::Void)
            return error("viper.compile_c: void function should not return a value");
        const int reg = parse_expr();
        if (reg < 0)
            return false;
        if (!expect_symbol(";", "viper.compile_c: expected ';' after return"))
            return false;
        int out = reg;
        if (m_fn->vars[reg].type != m_fn->return_type)
        {
            out = temp_var(m_fn->return_type);
            if (out < 0 || emit_mov(out, reg, m_fn->return_type) < 0)
                return false;
        }
        m_fn->return_reg = out;
        return emit(OpCode::Return, out, 0, 0, 0, m_fn->return_type) >= 0;
    }

    /**
     * @brief Parse an expression entry point.
     */
    int parse_expr()
    {
        return parse_logical_or();
    }

    bool add_jump(BranchList &list, int jump)
    {
        if (jump < 0)
            return false;
        list.jumps.push_back(jump);
        return true;
    }

    void patch_jumps(BranchList &list, int target)
    {
        for (int jump : list.jumps)
        {
            if (jump >= 0 && jump < (int)m_fn->code.size())
            {
                if (m_fn->code[(size_t)jump].op == OpCode::Jump)
                    m_fn->code[(size_t)jump].a = target;
                else if (m_fn->code[(size_t)jump].op == OpCode::JumpIfFalse)
                    m_fn->code[(size_t)jump].b = target;
            }
        }
    }

    /**
     * @brief Parse an if/for condition, generate short-circuit control flow directly, and return patch points that jump to the false branch.
     */
    bool parse_condition(BranchList &false_jumps)
    {
        BranchList true_jumps;
        if (!parse_condition_or(false_jumps, true_jumps))
            return false;
        patch_jumps(true_jumps, (int)m_fn->code.size());
        return true;
    }

    bool parse_condition_or(BranchList &false_jumps, BranchList &true_jumps)
    {
        if (!parse_condition_and(false_jumps))
            return false;

        while (match_symbol("||"))
        {
            const int jmp_true = emit(OpCode::Jump, 0, 0, 0, 0, ValueType::Void);
            if (!add_jump(true_jumps, jmp_true))
                return false;

            patch_jumps(false_jumps, (int)m_fn->code.size());
            false_jumps.jumps.clear();
            if (!parse_condition_and(false_jumps))
                return false;
        }
        return true;
    }

    bool parse_condition_and(BranchList &false_jumps)
    {
        int cond = parse_compare();
        if (cond < 0)
            return false;
        if (!add_jump(false_jumps, emit(OpCode::JumpIfFalse, cond, 0, 0, 0, ValueType::Void)))
            return false;

        while (match_symbol("&&"))
        {
            cond = parse_compare();
            if (cond < 0)
                return false;
            if (!add_jump(false_jumps, emit(OpCode::JumpIfFalse, cond, 0, 0, 0, ValueType::Void)))
                return false;
        }
        return true;
    }

    /**
     * @brief Parse a logical-or expression with C short-circuit semantics and normalize the result to bool.
     */
    int parse_logical_or()
    {
        int left = parse_logical_and();
        while (left >= 0 && match_symbol("||"))
        {
            const int dst = temp_var(ValueType::Bool);
            const int true_value = temp_const(1, ValueType::Bool);
            const int false_value = temp_const(0, ValueType::Bool);
            if (dst < 0 || true_value < 0 || false_value < 0)
                return -1;

            const int jmp_right = emit(OpCode::JumpIfFalse, left, 0, 0, 0, ValueType::Void);
            if (jmp_right < 0 || emit_mov(dst, true_value, ValueType::Bool) < 0)
                return -1;
            const int jmp_end_true = emit(OpCode::Jump, 0, 0, 0, 0, ValueType::Void);
            if (jmp_end_true < 0)
                return -1;
            m_fn->code[jmp_right].b = (int)m_fn->code.size();

            const int right = parse_logical_and();
            if (right < 0)
                return -1;
            const int jmp_false = emit(OpCode::JumpIfFalse, right, 0, 0, 0, ValueType::Void);
            if (jmp_false < 0 || emit_mov(dst, true_value, ValueType::Bool) < 0)
                return -1;
            const int jmp_end_right = emit(OpCode::Jump, 0, 0, 0, 0, ValueType::Void);
            if (jmp_end_right < 0)
                return -1;

            m_fn->code[jmp_false].b = (int)m_fn->code.size();
            if (emit_mov(dst, false_value, ValueType::Bool) < 0)
                return -1;
            const int end = (int)m_fn->code.size();
            m_fn->code[jmp_end_true].a = end;
            m_fn->code[jmp_end_right].a = end;
            left = dst;
        }
        return left;
    }

    /**
     * @brief Parse a logical-and expression with C short-circuit semantics and normalize the result to bool.
     */
    int parse_logical_and()
    {
        int left = parse_compare();
        while (left >= 0 && match_symbol("&&"))
        {
            const int dst = temp_var(ValueType::Bool);
            const int true_value = temp_const(1, ValueType::Bool);
            const int false_value = temp_const(0, ValueType::Bool);
            if (dst < 0 || true_value < 0 || false_value < 0)
                return -1;

            const int jmp_false_left = emit(OpCode::JumpIfFalse, left, 0, 0, 0, ValueType::Void);
            if (jmp_false_left < 0)
                return -1;
            const int right = parse_compare();
            if (right < 0)
                return -1;
            const int jmp_false_right = emit(OpCode::JumpIfFalse, right, 0, 0, 0, ValueType::Void);
            if (jmp_false_right < 0 || emit_mov(dst, true_value, ValueType::Bool) < 0)
                return -1;
            const int jmp_end = emit(OpCode::Jump, 0, 0, 0, 0, ValueType::Void);
            if (jmp_end < 0)
                return -1;

            const int false_label = (int)m_fn->code.size();
            m_fn->code[jmp_false_left].b = false_label;
            m_fn->code[jmp_false_right].b = false_label;
            if (emit_mov(dst, false_value, ValueType::Bool) < 0)
                return -1;
            m_fn->code[jmp_end].a = (int)m_fn->code.size();
            left = dst;
        }
        return left;
    }

    /**
     * @brief Parse a comparison expression.
     */
    int parse_compare()
    {
        int left = parse_bit_or();
        while (left >= 0 && cur().kind == TokenKind::Symbol &&
               (cur().text.equals("==") || cur().text.equals("!=") || cur().text.equals("~=") ||
                cur().text.equals("<") || cur().text.equals("<=") || cur().text.equals(">") ||
                cur().text.equals(">=")))
        {
            const TextRef op = cur().text;
            advance();
            const int right = parse_bit_or();
            if (right < 0)
                return -1;
            const int dst = temp_var(ValueType::Bool);
            OpCode cmp = OpCode::Eq;
            if (op.equals("=="))
                cmp = OpCode::Eq;
            else if (op.equals("!=") || op.equals("~="))
                cmp = OpCode::Ne;
            else if (op.equals("<"))
                cmp = OpCode::Lt;
            else if (op.equals("<="))
                cmp = OpCode::Le;
            else if (op.equals(">"))
                cmp = OpCode::Gt;
            else
                cmp = OpCode::Ge;
            const bool use_f32 = m_fn->vars[left].type == ValueType::F32 || m_fn->vars[right].type == ValueType::F32;
            if (dst < 0 || emit(typed_compare_opcode(cmp, use_f32), dst, left, right, 0, ValueType::Bool) < 0)
                return -1;
            left = dst;
        }
        return left;
    }

    /**
     * @brief Parse a bitwise-or expression.
     */
    int parse_bit_or()
    {
        int left = parse_bit_xor();
        while (left >= 0 && match_symbol("|"))
            left = binary(OpCode::BitOr, left, parse_bit_xor());
        return left;
    }

    /**
     * @brief Parse a bitwise-xor expression.
     */
    int parse_bit_xor()
    {
        int left = parse_bit_and();
        while (left >= 0 && match_symbol("^"))
            left = binary(OpCode::BitXor, left, parse_bit_and());
        return left;
    }

    /**
     * @brief Parse a bitwise-and expression.
     */
    int parse_bit_and()
    {
        int left = parse_shift();
        while (left >= 0 && match_symbol("&"))
            left = binary(OpCode::BitAnd, left, parse_shift());
        return left;
    }

    /**
     * @brief Parse a shift expression.
     */
    int parse_shift()
    {
        int left = parse_add();
        while (left >= 0 && cur().kind == TokenKind::Symbol &&
               (cur().text.equals("<<") || cur().text.equals(">>")))
        {
            const bool is_shl = cur().text.equals("<<");
            advance();
            left = binary(is_shl ? OpCode::Shl : OpCode::Shr, left, parse_add());
        }
        return left;
    }

    /**
     * @brief Parse an addition/subtraction expression.
     */
    int parse_add()
    {
        int left = parse_mul();
        while (left >= 0 && cur().kind == TokenKind::Symbol &&
               (cur().text.equals("+") || cur().text.equals("-")))
        {
            const bool is_add = cur().text.equals("+");
            advance();
            left = binary(is_add ? OpCode::Add : OpCode::Sub, left, parse_mul());
        }
        return left;
    }

    /**
     * @brief Parse a multiply/divide/modulo expression.
     */
    int parse_mul()
    {
        int left = parse_unary();
        while (left >= 0 && cur().kind == TokenKind::Symbol &&
               (cur().text.equals("*") || cur().text.equals("/") || cur().text.equals("%")))
        {
            const TextRef op = cur().text;
            advance();
            if (op.equals("*"))
                left = binary(OpCode::Mul, left, parse_unary());
            else if (op.equals("/"))
                left = binary(OpCode::Div, left, parse_unary());
            else
                left = binary(OpCode::Mod, left, parse_unary());
        }
        return left;
    }

    /**
     * @brief Parse a unary expression.
     */
    int parse_unary()
    {
        if (match_symbol("-"))
        {
            const int src = parse_unary();
            if (src < 0)
                return -1;
            const int dst = temp_var(m_fn->vars[src].type);
            if (dst < 0 ||
                emit(m_fn->vars[src].type == ValueType::F32 ? OpCode::NegF : OpCode::NegI,
                     dst, src, 0, 0, m_fn->vars[src].type) < 0)
                return -1;
            return dst;
        }
        if (match_symbol("!"))
        {
            const int src = parse_unary();
            if (src < 0)
                return -1;
            const int zero = temp_const(0, ValueType::Bool);
            const int dst = temp_var(ValueType::Bool);
            if (zero < 0 || dst < 0 || emit(OpCode::EqI, dst, src, zero, 0, ValueType::Bool) < 0)
                return -1;
            return dst;
        }
        if (match_symbol("~"))
        {
            const int src = parse_unary();
            if (src < 0)
                return -1;
            const int all_bits = temp_const(-1, ValueType::I32);
            if (all_bits < 0)
                return -1;
            return binary(OpCode::BitXor, src, all_bits);
        }
        return parse_primary();
    }

    /**
     * @brief Parse literals, variables, parentheses, and pointer subscript expressions.
     */
    int parse_primary()
    {
        if (cur().kind == TokenKind::Number)
        {
            const int dst = cur().is_float ? temp_const_f32(cur().fnumber) : temp_const(cur().number, ValueType::I32);
            advance();
            return dst;
        }

        if (match_symbol("("))
        {
            const int reg = parse_expr();
            if (reg < 0)
                return -1;
            if (!expect_symbol(")", "viper.compile_c: expected ')'"))
                return -1;
            return reg;
        }

        if (cur().kind != TokenKind::Name)
        {
            error("viper.compile_c: expected expression");
            return -1;
        }

        const TextRef name = cur().text;
        if (name.equals("true") || name.equals("false"))
        {
            const int dst = temp_const(name.equals("true") ? 1 : 0, ValueType::Bool);
            advance();
            return dst;
        }

        const int var = find_var(name);
        if (var < 0)
        {
            error("viper.compile_c: unknown variable in expression");
            return -1;
        }
        advance();

        if (match_symbol("["))
        {
            if (!is_ptr_type(m_fn->vars[var].type))
            {
                error("viper.compile_c: indexed expression base is not a pointer");
                return -1;
            }
            const int index = parse_expr();
            if (index < 0)
                return -1;
            if (!expect_symbol("]", "viper.compile_c: expected ']' after index"))
                return -1;
            return emit_index_load(var, index);
        }

        return var;
    }

    /**
     * @brief Emit a pointer subscript load instruction and return the destination register.
     */
    int emit_index_load(int var, int index)
    {
        const int dst = temp_var(ptr_value_type(m_fn->vars[var].type));
        if (dst < 0 || emit(load_opcode_for_ptr(m_fn->vars[var].type), dst, var, index, 0, m_fn->vars[var].type) < 0)
            return -1;
        return dst;
    }

    /**
     * @brief Write a value back to a variable or pointer subscript target.
     */
    bool emit_target_store(int var, int index, bool indexed, int value, ValueType target_type)
    {
        if (indexed)
            return emit(store_opcode_for_ptr(m_fn->vars[var].type), var, index, value, 0, m_fn->vars[var].type) >= 0;
        return emit_mov(var, value, target_type) >= 0;
    }

    /**
     * @brief Convert a compound-assignment symbol to a binary operation opcode.
     */
    static OpCode assignment_binary_opcode(TextRef op)
    {
        if (op.equals("+="))
            return OpCode::Add;
        if (op.equals("-="))
            return OpCode::Sub;
        if (op.equals("*="))
            return OpCode::Mul;
        if (op.equals("/="))
            return OpCode::Div;
        return OpCode::Mod;
    }

    /**
     * @brief Emit a binary expression instruction.
     */
    int binary(OpCode op, int left, int right)
    {
        if (left < 0 || right < 0)
            return -1;
        ValueType type = ValueType::I32;
        if ((op == OpCode::Add || op == OpCode::Sub || op == OpCode::Mul || op == OpCode::Div) &&
            (m_fn->vars[left].type == ValueType::F32 || m_fn->vars[right].type == ValueType::F32))
        {
            type = ValueType::F32;
        }
        const int dst = temp_var(type);
        if (dst < 0 || emit(typed_arith_opcode(op, type), dst, left, right, 0, type) < 0)
            return -1;
        return dst;
    }

    /**
     * @brief Find a variable register.
     */
    int find_var(TextRef name) const
    {
        if (name.empty())
            return -1;
        for (size_t i = 0; i < m_var_names.size(); ++i)
        {
            const TextRef stored = m_var_names[i];
            if (stored.len == name.len && stored.data &&
                std::memcmp(stored.data, name.data, name.len) == 0)
                return (int)i;
        }
        return -1;
    }

    /**
     * @brief Add a variable register.
     */
    int add_var(const Variable &var, TextRef name = {})
    {
        if (m_fn->vars.size() >= kMaxVariables)
        {
            error("viper.compile_c: too many variables");
            return -1;
        }
        if (!name.empty() && find_var(name) >= 0)
        {
            error("viper.compile_c: duplicate variable");
            return -1;
        }
        m_fn->vars.push_back(var);
        m_var_names.push_back(name);
        return (int)m_fn->vars.size() - 1;
    }

    /**
     * @brief Add a temporary register.
     */
    int temp_var(ValueType type)
    {
        Variable v;
        v.type = type;
        v.is_temp = true;
        return add_var(v);
    }

    /**
     * @brief Find an existing integer-constant temporary register to avoid duplicate literals inflating the variable table.
     */
    int find_const_var(int64_t value, ValueType type) const
    {
        if (!m_fn)
            return -1;
        const int64_t casted = cast_scalar(value, type);
        for (size_t i = 0; i < m_fn->vars.size(); ++i)
        {
            const Variable &v = m_fn->vars[i];
            if (v.is_const && v.is_temp && v.type == type && v.const_i == casted)
                return (int)i;
        }
        return -1;
    }

    /**
     * @brief Add or reuse an integer-constant register.
     */
    int temp_const(int64_t value, ValueType type)
    {
        const int existing = find_const_var(value, type);
        if (existing >= 0)
            return existing;

        Variable v;
        v.type = type;
        v.is_const = true;
        v.is_temp = true;
        v.const_i = cast_scalar(value, type);
        v.const_f = (float)v.const_i;
        return add_var(v);
    }

    /**
     * @brief Add an f32 constant register.
     */
    int temp_const_f32(float value)
    {
        Variable v;
        v.type = ValueType::F32;
        v.is_const = true;
        v.is_temp = true;
        v.const_f = value;
        v.const_i = (int64_t)value;
        return add_var(v);
    }

    /**
     * @brief Emit a variable move and merge adjacent temporary-register writes.
     */
    int emit_mov(int dst, int src, ValueType type)
    {
        if (dst < 0 || dst >= (int)m_fn->vars.size() || src < 0 || src >= (int)m_fn->vars.size())
        {
            error("viper.compile_c: invalid move operand");
            return -1;
        }
        const ValueType src_type = m_fn->vars[src].type;
        const ValueType dst_type = m_fn->vars[dst].type;
        if (src >= 0 && src < (int)m_fn->vars.size() &&
            m_fn->vars[src].is_temp && !m_fn->vars[src].is_const &&
            can_merge_temp_move_type(src_type, dst_type) && !m_fn->code.empty() &&
            !current_pc_is_branch_target())
        {
            Instruction &prev = m_fn->code.back();
            if (prev.a == src && prev.op != OpCode::Jump && prev.op != OpCode::JumpIfFalse &&
                prev.op != OpCode::Return && can_retarget_temp_write(prev.op, src_type, dst_type))
            {
                prev.a = dst;
                if (!is_load_opcode(prev.op))
                    prev.type = type;
                return (int)m_fn->code.size() - 1;
            }
        }
        return emit(OpCode::Mov, dst, src, 0, 0, type);
    }

    /**
     * @brief Return true when the next IR slot is already a branch target.
     *
     * Retargeting a temp write into the final assignment target is only valid
     * for straight-line code. Short-circuit expressions can create multiple
     * branches that write the same temp and then join at the final Mov. At that
     * join point the Mov must stay explicit, otherwise only the last path is
     * retargeted and earlier paths still write the temp.
     */
    bool current_pc_is_branch_target() const
    {
        const int pc = (int)m_fn->code.size();
        for (const Instruction &ins : m_fn->code)
        {
            if (ins.op == OpCode::Jump && ins.a == pc)
                return true;
            if (ins.op == OpCode::JumpIfFalse && ins.b == pc)
                return true;
        }
        return false;
    }

    /**
     * @brief Determine whether a temporary write can be retargeted to an assignment target to avoid an immediately following Mov.
     */
    static bool can_merge_temp_move_type(ValueType src_type, ValueType dst_type)
    {
        if (src_type == dst_type)
            return true;
        return (src_type == ValueType::I32 && dst_type == ValueType::U32) ||
               (src_type == ValueType::U32 && dst_type == ValueType::I32);
    }

    /**
     * @brief Determine whether retargeting the specified IR write preserves the original semantics.
     */
    static bool can_retarget_temp_write(OpCode op, ValueType src_type, ValueType dst_type)
    {
        if (src_type == dst_type)
            return true;
        if (!can_merge_temp_move_type(src_type, dst_type))
            return false;

        switch (op)
        {
        case OpCode::Div:
        case OpCode::DivI:
        case OpCode::Mod:
            return false;
        default:
            return true;
        }
    }

    /**
     * @brief Determine whether an IR instruction is a pointer read; these instructions store pointer element width in type.
     */
    static bool is_load_opcode(OpCode op)
    {
        return op == OpCode::Load || op == OpCode::Load8 ||
               op == OpCode::Load8I32 || op == OpCode::Load16 ||
               op == OpCode::Load16I32 || op == OpCode::Load32 ||
               op == OpCode::Load32I32 || op == OpCode::LoadF32;
    }

    /**
     * @brief Append an IR bytecode instruction.
     */
    int emit(OpCode op, int a, int b, int c, int64_t imm, ValueType type, float fimm = 0.0f)
    {
        if (m_fn->code.size() >= kMaxInstructions)
        {
            error("viper.compile_c: too many instructions");
            return -1;
        }
        Instruction ins;
        ins.op = op;
        ins.a = a;
        ins.b = b;
        ins.c = c;
        ins.imm = imm;
        ins.fimm = fimm;
        ins.type = type;
        m_fn->code.push_back(ins);
        return (int)m_fn->code.size() - 1;
    }
};

/**
 * @brief Determine whether two IR scalar types can preserve the same read-value semantics during copy propagation.
 */
static bool same_ir_copy_type(ValueType lhs, ValueType rhs)
{
    return lhs == rhs;
}

/**
 * @brief Determine whether a variable number points to a temporary register in the current function.
 */
static bool is_temp_var(const CompiledFunction &fn, int var)
{
    return var >= 0 && var < (int)fn.vars.size() && fn.vars[(size_t)var].is_temp;
}

/**
 * @brief Determine whether a variable number points to an integer-constant temporary register in the current function.
 */
static bool const_int_var(const CompiledFunction &fn, int var, int64_t &value)
{
    if (var < 0 || var >= (int)fn.vars.size())
        return false;
    const Variable &v = fn.vars[(size_t)var];
    if (!v.is_const || v.type == ValueType::F32 || v.type == ValueType::PtrF32)
        return false;
    value = cast_scalar(v.const_i, v.type);
    return true;
}

/**
 * @brief Determine whether an integer comparison should be folded with unsigned semantics.
 */
static bool ir_unsigned_compare(ValueType a, ValueType b)
{
    return a == ValueType::U32 || b == ValueType::U32 ||
           is_ptr_type(a) || is_ptr_type(b);
}

/**
 * @brief Find or append an integer-constant temporary register; returns -1 on failure so the optimization pass can skip it.
 */
static int get_or_add_const_int(CompiledFunction &fn, int64_t value, ValueType type)
{
    if (type == ValueType::F32 || type == ValueType::PtrF32 || type == ValueType::Void)
        return -1;

    const int64_t casted = cast_scalar(value, type);
    for (size_t i = 0; i < fn.vars.size(); ++i)
    {
        const Variable &v = fn.vars[i];
        if (v.is_const && v.is_temp && v.type == type && v.const_i == casted)
            return (int)i;
    }

    if (fn.vars.size() >= kMaxVariables)
        return -1;

    Variable v;
    v.type = type;
    v.is_const = true;
    v.is_temp = true;
    v.const_i = casted;
    v.const_f = (float)casted;
    fn.vars.push_back(v);
    return (int)fn.vars.size() - 1;
}

/**
 * @brief Return the variable number written by an IR instruction, or -1 when no variable is written.
 */
static int instruction_write_var(const Instruction &ins)
{
    switch (ins.op)
    {
    case OpCode::Const:
    case OpCode::Mov:
    case OpCode::Add:
    case OpCode::AddI:
    case OpCode::AddI32:
    case OpCode::AddF:
    case OpCode::Sub:
    case OpCode::SubI:
    case OpCode::SubI32:
    case OpCode::SubF:
    case OpCode::Mul:
    case OpCode::MulI:
    case OpCode::MulI32:
    case OpCode::MulF:
    case OpCode::Div:
    case OpCode::DivI:
    case OpCode::DivF:
    case OpCode::Mod:
    case OpCode::Shl:
    case OpCode::Shr:
    case OpCode::BitAnd:
    case OpCode::BitOr:
    case OpCode::BitXor:
    case OpCode::Neg:
    case OpCode::NegI:
    case OpCode::NegF:
    case OpCode::Eq:
    case OpCode::EqI:
    case OpCode::EqF:
    case OpCode::Ne:
    case OpCode::NeI:
    case OpCode::NeF:
    case OpCode::Lt:
    case OpCode::LtI:
    case OpCode::LtF:
    case OpCode::Le:
    case OpCode::LeI:
    case OpCode::LeF:
    case OpCode::Gt:
    case OpCode::GtI:
    case OpCode::GtF:
    case OpCode::Ge:
    case OpCode::GeI:
    case OpCode::GeF:
    case OpCode::Load:
    case OpCode::Load8:
    case OpCode::Load8I32:
    case OpCode::Load16:
    case OpCode::Load16I32:
    case OpCode::Load32:
    case OpCode::Load32I32:
    case OpCode::LoadF32:
        return ins.a;
    default:
        return -1;
    }
}

/**
 * @brief Count variable reads for one IR instruction, shared by use-counting and copy propagation.
 */
template <typename Fn>
static void for_each_instruction_read(const Instruction &ins, Fn &&fn)
{
    switch (ins.op)
    {
    case OpCode::Mov:
        fn(ins.b);
        break;
    case OpCode::JumpIfFalse:
    case OpCode::Return:
        fn(ins.a);
        break;
    case OpCode::Add:
    case OpCode::AddI:
    case OpCode::AddI32:
    case OpCode::AddF:
    case OpCode::Sub:
    case OpCode::SubI:
    case OpCode::SubI32:
    case OpCode::SubF:
    case OpCode::Mul:
    case OpCode::MulI:
    case OpCode::MulI32:
    case OpCode::MulF:
    case OpCode::Div:
    case OpCode::DivI:
    case OpCode::DivF:
    case OpCode::Mod:
    case OpCode::BitAnd:
    case OpCode::BitOr:
    case OpCode::BitXor:
    case OpCode::Shl:
    case OpCode::Shr:
    case OpCode::Eq:
    case OpCode::EqI:
    case OpCode::EqF:
    case OpCode::Ne:
    case OpCode::NeI:
    case OpCode::NeF:
    case OpCode::Lt:
    case OpCode::LtI:
    case OpCode::LtF:
    case OpCode::Le:
    case OpCode::LeI:
    case OpCode::LeF:
    case OpCode::Gt:
    case OpCode::GtI:
    case OpCode::GtF:
    case OpCode::Ge:
    case OpCode::GeI:
    case OpCode::GeF:
    case OpCode::Load:
    case OpCode::Load8:
    case OpCode::Load8I32:
    case OpCode::Load16:
    case OpCode::Load16I32:
    case OpCode::Load32:
    case OpCode::Load32I32:
    case OpCode::LoadF32:
        fn(ins.b);
        fn(ins.c);
        break;
    case OpCode::Neg:
    case OpCode::NegI:
    case OpCode::NegF:
        fn(ins.b);
        break;
    case OpCode::Store:
    case OpCode::Store8:
    case OpCode::Store16:
    case OpCode::Store32:
    case OpCode::StoreF32:
        fn(ins.a);
        fn(ins.b);
        fn(ins.c);
        break;
    default:
        break;
    }
}

/**
 * @brief Determine whether an instruction is a pure temporary write that can be safely deleted.
 */
static bool is_pure_temp_write_opcode(OpCode op)
{
    switch (op)
    {
    case OpCode::Const:
    case OpCode::Mov:
    case OpCode::Add:
    case OpCode::AddI:
    case OpCode::AddI32:
    case OpCode::AddF:
    case OpCode::Sub:
    case OpCode::SubI:
    case OpCode::SubI32:
    case OpCode::SubF:
    case OpCode::Mul:
    case OpCode::MulI:
    case OpCode::MulI32:
    case OpCode::MulF:
    case OpCode::Div:
    case OpCode::DivI:
    case OpCode::DivF:
    case OpCode::Mod:
    case OpCode::Shl:
    case OpCode::Shr:
    case OpCode::BitAnd:
    case OpCode::BitOr:
    case OpCode::BitXor:
    case OpCode::Neg:
    case OpCode::NegI:
    case OpCode::NegF:
    case OpCode::Eq:
    case OpCode::EqI:
    case OpCode::EqF:
    case OpCode::Ne:
    case OpCode::NeI:
    case OpCode::NeF:
    case OpCode::Lt:
    case OpCode::LtI:
    case OpCode::LtF:
    case OpCode::Le:
    case OpCode::LeI:
    case OpCode::LeF:
    case OpCode::Gt:
    case OpCode::GtI:
    case OpCode::GtF:
    case OpCode::Ge:
    case OpCode::GeI:
    case OpCode::GeF:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Replace variable reads with known temporary aliases in the current basic block.
 */
static int resolve_temp_alias(const CompiledFunction &fn, const ViperInternalVector<int> &alias, int var)
{
    int cur = var;
    for (size_t depth = 0; depth < 8 && cur >= 0 && cur < (int)alias.size(); ++depth)
    {
        const int next = alias[(size_t)cur];
        if (next < 0 || next == cur)
            break;
        if (!same_ir_copy_type(fn.vars[(size_t)cur].type, fn.vars[(size_t)next].type))
            break;
        cur = next;
    }
    return cur;
}

/**
 * @brief Clear all temporary aliases that depend on the specified variable.
 */
static void invalidate_alias_source(const CompiledFunction &fn, ViperInternalVector<int> &alias, int source)
{
    if (source < 0)
        return;
    for (size_t i = 0; i < alias.size(); ++i)
    {
        if (alias[i] < 0)
            continue;
        const int root = resolve_temp_alias(fn, alias, alias[i]);
        if (root == source || (int)i == source)
            alias[i] = -1;
    }
}

/**
 * @brief Rewrite all read operands in IR using the alias table.
 */
static bool rewrite_instruction_reads(CompiledFunction &fn, Instruction &ins,
                                      const ViperInternalVector<int> &alias)
{
    bool changed = false;
    auto rewrite = [&](int &var) {
        if (var < 0 || var >= (int)fn.vars.size())
            return;
        const int resolved = resolve_temp_alias(fn, alias, var);
        if (resolved != var && same_ir_copy_type(fn.vars[(size_t)var].type, fn.vars[(size_t)resolved].type))
        {
            var = resolved;
            changed = true;
        }
    };

    switch (ins.op)
    {
    case OpCode::Mov:
        rewrite(ins.b);
        break;
    case OpCode::JumpIfFalse:
    case OpCode::Return:
        rewrite(ins.a);
        break;
    case OpCode::Add:
    case OpCode::AddI:
    case OpCode::AddI32:
    case OpCode::AddF:
    case OpCode::Sub:
    case OpCode::SubI:
    case OpCode::SubI32:
    case OpCode::SubF:
    case OpCode::Mul:
    case OpCode::MulI:
    case OpCode::MulI32:
    case OpCode::MulF:
    case OpCode::Div:
    case OpCode::DivI:
    case OpCode::DivF:
    case OpCode::Mod:
    case OpCode::BitAnd:
    case OpCode::BitOr:
    case OpCode::BitXor:
    case OpCode::Shl:
    case OpCode::Shr:
    case OpCode::Eq:
    case OpCode::EqI:
    case OpCode::EqF:
    case OpCode::Ne:
    case OpCode::NeI:
    case OpCode::NeF:
    case OpCode::Lt:
    case OpCode::LtI:
    case OpCode::LtF:
    case OpCode::Le:
    case OpCode::LeI:
    case OpCode::LeF:
    case OpCode::Gt:
    case OpCode::GtI:
    case OpCode::GtF:
    case OpCode::Ge:
    case OpCode::GeI:
    case OpCode::GeF:
    case OpCode::Load:
    case OpCode::Load8:
    case OpCode::Load8I32:
    case OpCode::Load16:
    case OpCode::Load16I32:
    case OpCode::Load32:
    case OpCode::Load32I32:
    case OpCode::LoadF32:
        rewrite(ins.b);
        rewrite(ins.c);
        break;
    case OpCode::Neg:
    case OpCode::NegI:
    case OpCode::NegF:
        rewrite(ins.b);
        break;
    case OpCode::Store:
    case OpCode::Store8:
    case OpCode::Store16:
    case OpCode::Store32:
    case OpCode::StoreF32:
        rewrite(ins.a);
        rewrite(ins.b);
        rewrite(ins.c);
        break;
    default:
        break;
    }
    return changed;
}

/**
 * @brief Rewrite foldable integer expressions as Mov dst, const.
 */
static bool fold_integer_instruction(CompiledFunction &fn, Instruction &ins)
{
    int64_t lhs = 0;
    int64_t rhs = 0;
    int64_t result = 0;
    bool folded = false;

    switch (ins.op)
    {
    case OpCode::Neg:
    case OpCode::NegI:
        if (!const_int_var(fn, ins.b, lhs))
            return false;
        result = -lhs;
        folded = true;
        break;

    case OpCode::Add:
    case OpCode::AddI:
    case OpCode::AddI32:
        if (!const_int_var(fn, ins.b, lhs) || !const_int_var(fn, ins.c, rhs))
            return false;
        result = (int64_t)((uint32_t)lhs + (uint32_t)rhs);
        folded = true;
        break;

    case OpCode::Sub:
    case OpCode::SubI:
    case OpCode::SubI32:
        if (!const_int_var(fn, ins.b, lhs) || !const_int_var(fn, ins.c, rhs))
            return false;
        result = (int64_t)((uint32_t)lhs - (uint32_t)rhs);
        folded = true;
        break;

    case OpCode::Mul:
    case OpCode::MulI:
    case OpCode::MulI32:
        if (!const_int_var(fn, ins.b, lhs) || !const_int_var(fn, ins.c, rhs))
            return false;
        result = (int64_t)((uint32_t)lhs * (uint32_t)rhs);
        folded = true;
        break;

    case OpCode::Div:
    case OpCode::DivI:
        if (!const_int_var(fn, ins.b, lhs) || !const_int_var(fn, ins.c, rhs) || rhs == 0)
            return false;
        if (rhs == -1 && (int32_t)lhs == std::numeric_limits<int32_t>::min())
            return false;
        if (ir_unsigned_compare(fn.vars[(size_t)ins.b].type, fn.vars[(size_t)ins.c].type) ||
            fn.vars[(size_t)ins.a].type == ValueType::U32)
            result = (int64_t)((uint32_t)lhs / (uint32_t)rhs);
        else
            result = (int64_t)((int32_t)lhs / (int32_t)rhs);
        folded = true;
        break;

    case OpCode::Mod:
        if (!const_int_var(fn, ins.b, lhs) || !const_int_var(fn, ins.c, rhs) || rhs == 0)
            return false;
        if (rhs == -1 && (int32_t)lhs == std::numeric_limits<int32_t>::min())
            return false;
        if (ir_unsigned_compare(fn.vars[(size_t)ins.b].type, fn.vars[(size_t)ins.c].type) ||
            fn.vars[(size_t)ins.a].type == ValueType::U32)
            result = (int64_t)((uint32_t)lhs % (uint32_t)rhs);
        else
            result = (int64_t)((int32_t)lhs % (int32_t)rhs);
        folded = true;
        break;

    case OpCode::Shl:
        if (!const_int_var(fn, ins.b, lhs) || !const_int_var(fn, ins.c, rhs) || rhs < 0 || rhs >= 32)
            return false;
        result = (int64_t)((uint32_t)lhs << (uint32_t)rhs);
        folded = true;
        break;

    case OpCode::Shr:
        if (!const_int_var(fn, ins.b, lhs) || !const_int_var(fn, ins.c, rhs) || rhs < 0 || rhs >= 32)
            return false;
        result = (int64_t)((uint32_t)lhs >> (uint32_t)rhs);
        folded = true;
        break;

    case OpCode::BitAnd:
        if (!const_int_var(fn, ins.b, lhs) || !const_int_var(fn, ins.c, rhs))
            return false;
        result = (int64_t)((uint32_t)lhs & (uint32_t)rhs);
        folded = true;
        break;

    case OpCode::BitOr:
        if (!const_int_var(fn, ins.b, lhs) || !const_int_var(fn, ins.c, rhs))
            return false;
        result = (int64_t)((uint32_t)lhs | (uint32_t)rhs);
        folded = true;
        break;

    case OpCode::BitXor:
        if (!const_int_var(fn, ins.b, lhs) || !const_int_var(fn, ins.c, rhs))
            return false;
        result = (int64_t)((uint32_t)lhs ^ (uint32_t)rhs);
        folded = true;
        break;

    case OpCode::Eq:
    case OpCode::EqI:
        if (!const_int_var(fn, ins.b, lhs) || !const_int_var(fn, ins.c, rhs))
            return false;
        result = lhs == rhs ? 1 : 0;
        folded = true;
        break;

    case OpCode::Ne:
    case OpCode::NeI:
        if (!const_int_var(fn, ins.b, lhs) || !const_int_var(fn, ins.c, rhs))
            return false;
        result = lhs != rhs ? 1 : 0;
        folded = true;
        break;

    case OpCode::Lt:
    case OpCode::LtI:
        if (!const_int_var(fn, ins.b, lhs) || !const_int_var(fn, ins.c, rhs))
            return false;
        result = ir_unsigned_compare(fn.vars[(size_t)ins.b].type, fn.vars[(size_t)ins.c].type)
                     ? ((uint32_t)lhs < (uint32_t)rhs ? 1 : 0)
                     : ((int32_t)lhs < (int32_t)rhs ? 1 : 0);
        folded = true;
        break;

    case OpCode::Le:
    case OpCode::LeI:
        if (!const_int_var(fn, ins.b, lhs) || !const_int_var(fn, ins.c, rhs))
            return false;
        result = ir_unsigned_compare(fn.vars[(size_t)ins.b].type, fn.vars[(size_t)ins.c].type)
                     ? ((uint32_t)lhs <= (uint32_t)rhs ? 1 : 0)
                     : ((int32_t)lhs <= (int32_t)rhs ? 1 : 0);
        folded = true;
        break;

    case OpCode::Gt:
    case OpCode::GtI:
        if (!const_int_var(fn, ins.b, lhs) || !const_int_var(fn, ins.c, rhs))
            return false;
        result = ir_unsigned_compare(fn.vars[(size_t)ins.b].type, fn.vars[(size_t)ins.c].type)
                     ? ((uint32_t)lhs > (uint32_t)rhs ? 1 : 0)
                     : ((int32_t)lhs > (int32_t)rhs ? 1 : 0);
        folded = true;
        break;

    case OpCode::Ge:
    case OpCode::GeI:
        if (!const_int_var(fn, ins.b, lhs) || !const_int_var(fn, ins.c, rhs))
            return false;
        result = ir_unsigned_compare(fn.vars[(size_t)ins.b].type, fn.vars[(size_t)ins.c].type)
                     ? ((uint32_t)lhs >= (uint32_t)rhs ? 1 : 0)
                     : ((int32_t)lhs >= (int32_t)rhs ? 1 : 0);
        folded = true;
        break;

    default:
        return false;
    }

    if (!folded)
        return false;

    const int dst = ins.a;
    const ValueType result_type = fn.vars[(size_t)dst].type == ValueType::Bool ? ValueType::Bool : ins.type;
    const int c = get_or_add_const_int(fn, result, result_type);
    if (c < 0)
        return false;
    ins.op = OpCode::Mov;
    ins.b = c;
    ins.c = 0;
    ins.imm = 0;
    ins.fimm = 0.0f;
    ins.type = fn.vars[(size_t)dst].type;
    return true;
}

/**
 * @brief Try to simplify integer operations with identity elements into Mov.
 */
static bool simplify_integer_identity(CompiledFunction &fn, Instruction &ins)
{
    int64_t lhs = 0;
    int64_t rhs = 0;
    int replacement = -1;
    int64_t const_result = 0;
    bool use_const_result = false;

    switch (ins.op)
    {
    case OpCode::Add:
    case OpCode::AddI:
    case OpCode::AddI32:
    case OpCode::BitOr:
    case OpCode::BitXor:
        if (const_int_var(fn, ins.c, rhs) && rhs == 0)
            replacement = ins.b;
        else if (const_int_var(fn, ins.b, lhs) && lhs == 0)
            replacement = ins.c;
        break;

    case OpCode::Sub:
    case OpCode::SubI:
    case OpCode::SubI32:
    case OpCode::Shl:
    case OpCode::Shr:
        if (const_int_var(fn, ins.c, rhs) && rhs == 0)
            replacement = ins.b;
        break;

    case OpCode::Mul:
    case OpCode::MulI:
    case OpCode::MulI32:
        if (const_int_var(fn, ins.c, rhs))
        {
            if (rhs == 1)
                replacement = ins.b;
            else if (rhs == 0)
            {
                use_const_result = true;
                const_result = 0;
            }
        }
        else if (const_int_var(fn, ins.b, lhs))
        {
            if (lhs == 1)
                replacement = ins.c;
            else if (lhs == 0)
            {
                use_const_result = true;
                const_result = 0;
            }
        }
        break;

    case OpCode::Div:
    case OpCode::DivI:
        if (const_int_var(fn, ins.c, rhs) && rhs == 1)
            replacement = ins.b;
        break;

    case OpCode::BitAnd:
        if (const_int_var(fn, ins.c, rhs))
        {
            if (rhs == 0)
            {
                use_const_result = true;
                const_result = 0;
            }
            else if ((uint32_t)rhs == UINT32_MAX)
                replacement = ins.b;
        }
        else if (const_int_var(fn, ins.b, lhs))
        {
            if (lhs == 0)
            {
                use_const_result = true;
                const_result = 0;
            }
            else if ((uint32_t)lhs == UINT32_MAX)
                replacement = ins.c;
        }
        break;

    default:
        break;
    }

    if (use_const_result)
    {
        replacement = get_or_add_const_int(fn, const_result, ins.type);
        if (replacement < 0)
            return false;
    }
    if (replacement < 0)
        return false;
    if (!same_ir_copy_type(fn.vars[(size_t)ins.a].type, fn.vars[(size_t)replacement].type))
        return false;

    ins.op = OpCode::Mov;
    ins.b = replacement;
    ins.c = 0;
    ins.imm = 0;
    ins.fimm = 0.0f;
    ins.type = fn.vars[(size_t)ins.a].type;
    return true;
}

/**
 * @brief Run basic-block temp-only copy propagation and constant simplification on the current IR.
 */
static bool propagate_and_fold_temp_ir(CompiledFunction &fn)
{
    bool changed = false;
    ViperInternalVector<uint8_t> branch_targets(fn.code.size() + 1, 0);
    for (const Instruction &ins : fn.code)
    {
        if (ins.op == OpCode::Jump && ins.a >= 0 && ins.a < (int)branch_targets.size())
            branch_targets[(size_t)ins.a] = 1;
        else if (ins.op == OpCode::JumpIfFalse && ins.b >= 0 && ins.b < (int)branch_targets.size())
            branch_targets[(size_t)ins.b] = 1;
    }

    ViperInternalVector<int> alias(fn.vars.size(), -1);
    auto reset_alias = [&]() {
        for (size_t i = 0; i < alias.size(); ++i)
            alias[i] = -1;
    };

    for (size_t pc = 0; pc < fn.code.size(); ++pc)
    {
        if (pc < branch_targets.size() && branch_targets[pc])
            reset_alias();

        Instruction &ins = fn.code[pc];
        if (rewrite_instruction_reads(fn, ins, alias))
            changed = true;
        if (fold_integer_instruction(fn, ins))
            changed = true;
        else if (simplify_integer_identity(fn, ins))
            changed = true;

        const int dst = instruction_write_var(ins);
        if (dst >= 0)
        {
            if (dst < (int)alias.size())
                alias[(size_t)dst] = -1;
            invalidate_alias_source(fn, alias, dst);
        }

        if (ins.op == OpCode::Mov && is_temp_var(fn, ins.a) &&
            ins.b >= 0 && ins.b < (int)fn.vars.size() &&
            same_ir_copy_type(fn.vars[(size_t)ins.a].type, fn.vars[(size_t)ins.b].type))
        {
            alias[(size_t)ins.a] = resolve_temp_alias(fn, alias, ins.b);
        }

        if (ins.op == OpCode::Jump || ins.op == OpCode::JumpIfFalse)
            reset_alias();
    }
    return changed;
}

/**
 * @brief Delete pure temporary writes with no readers and remap jump targets.
 */
static bool eliminate_dead_temp_writes(CompiledFunction &fn)
{
    if (fn.code.empty())
        return false;

    ViperInternalVector<uint16_t> uses(fn.vars.size(), 0);
    for (const Instruction &ins : fn.code)
    {
        for_each_instruction_read(ins, [&](int var) {
            if (var >= 0 && var < (int)uses.size() && uses[(size_t)var] < UINT16_MAX)
                ++uses[(size_t)var];
        });
    }

    ViperInternalVector<uint8_t> keep(fn.code.size(), 1);
    bool changed = false;
    for (size_t pc = 0; pc < fn.code.size(); ++pc)
    {
        const Instruction &ins = fn.code[pc];
        const int dst = instruction_write_var(ins);
        if (dst >= 0 && is_temp_var(fn, dst) && !fn.vars[(size_t)dst].is_const &&
            is_pure_temp_write_opcode(ins.op) && uses[(size_t)dst] == 0)
        {
            keep[pc] = 0;
            changed = true;
        }
    }
    if (!changed)
        return false;

    ViperInternalVector<int> old_to_new(fn.code.size() + 1, 0);
    ViperInternalVector<Instruction> compacted;
    compacted.reserve(fn.code.size());
    int next_pc = 0;
    for (size_t pc = 0; pc < fn.code.size(); ++pc)
    {
        old_to_new[pc] = next_pc;
        if (keep[pc])
        {
            compacted.push_back(fn.code[pc]);
            ++next_pc;
        }
    }
    old_to_new[fn.code.size()] = next_pc;

    for (Instruction &ins : compacted)
    {
        if (ins.op == OpCode::Jump && ins.a >= 0 && ins.a < (int)old_to_new.size())
            ins.a = old_to_new[(size_t)ins.a];
        else if (ins.op == OpCode::JumpIfFalse && ins.b >= 0 && ins.b < (int)old_to_new.size())
            ins.b = old_to_new[(size_t)ins.b];
    }

    fn.code.swap(compacted);
    return true;
}

/**
 * @brief Conservative IR optimization: only propagate/delete temporaries without changing visible storage boundaries for named variables.
 */
static void optimize_temp_only_ir(CompiledFunction &fn)
{
    constexpr size_t kMaxTempOnlyPasses = 8;
    for (size_t pass = 0; pass < kMaxTempOnlyPasses; ++pass)
    {
        bool changed = propagate_and_fold_temp_ir(fn);
        changed = eliminate_dead_temp_writes(fn) || changed;
        if (!changed)
            break;
    }
}

bool parse_viper_c(const char *src, bool unsafe_ptr, bool bounds_check,
                   CompiledFunction &fn, ViperError &err)
{
    err.clear();
    ViperAllocScope alloc_scope;
    enter_viper_alloc_scope(alloc_scope);
    if (setjmp(alloc_scope.env) != 0)
    {
        err.set(alloc_scope.error ? alloc_scope.error : "viper.compile_c: out of memory");
        leave_viper_alloc_scope(alloc_scope);
        return false;
    }

    ViperScopedObject<CParser> parser(make_viper_work_object<CParser>(src, unsafe_ptr, bounds_check));
    if (!parser.get())
    {
        err.set("viper.compile_c: parser state allocation failed");
        leave_viper_alloc_scope(alloc_scope);
        return false;
    }
    if (!parser->parse(fn, err))
    {
        leave_viper_alloc_scope(alloc_scope);
        return false;
    }
    optimize_temp_only_ir(fn);
    leave_viper_alloc_scope(alloc_scope);
    return true;
}

} // namespace lua_nodemcu_viper::internal
