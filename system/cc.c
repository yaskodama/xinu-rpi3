/**
 * @file cc.c
 *
 * Tiny C-subset compiler that targets the bytecode VM defined in <aout.h>.
 *
 * Supported subset (one source file, one function `int main(void)` plus
 * optional sibling functions taking no arguments):
 *   - declarations: `int IDENT;`, `int IDENT = expr;`
 *   - statements:   if/else, while, for, return, expression, block
 *   - expressions:  ints, strings, idents, + - * / %, == != < <= > >=,
 *                   ! && ||, unary -, parenthesized
 *   - calls to builtins: printf, puts, putchar, getchar, exit
 *   - `#include <...>` and `#include "..."` are accepted and ignored
 *
 * The compiler is single-pass: it parses and emits bytecode in one walk.
 * Forward jumps are patched up via a small "fixup" mechanism.
 */

#include <stddef.h>
#include <stdint.h>
#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <memory.h>
#include <xfs.h>
#include <aout.h>

#define CC_CODE_MAX     16384
#define CC_CONSTS_MAX   16384
#define CC_NLOCALS      64
#define CC_NAMELEN      32
#define CC_MAX_LINE     1024

/* ---------- token types ---------- */

enum {
    TK_EOF = 0,
    TK_IDENT, TK_INT, TK_STR,
    TK_INTKW, TK_VOIDKW, TK_CHARKW,
    TK_RETURN, TK_IF, TK_ELSE, TK_WHILE, TK_FOR,
    TK_LP, TK_RP, TK_LB, TK_RB, TK_SEMI, TK_COMMA,
    TK_ASSIGN,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PCT,
    TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_NOT, TK_LAND, TK_LOR
};

/* ---------- compiler state ---------- */

static const char *g_src;
static int g_pos;
static int g_line;
static int g_tok;
static int32_t g_tok_int;
static char g_tok_str[256];
static int g_tok_strlen;
static int g_err;

static uint8_t *g_code;
static uint32_t g_code_pos;
static uint8_t *g_consts;
static uint32_t g_consts_pos;

struct cc_local {
    char name[CC_NAMELEN];
    int  slot;
};
static struct cc_local g_locals[CC_NLOCALS];
static int g_nlocals;

static void cc_err(const char *msg)
{
    if (!g_err)
        printf("cc: line %d: %s\n", g_line, msg);
    g_err = 1;
}

/* ---------- emit helpers ---------- */

static void emit_u8(uint8_t b)
{
    if (g_code_pos >= CC_CODE_MAX) { cc_err("code overflow"); return; }
    g_code[g_code_pos++] = b;
}

static void emit_u16(uint16_t v)
{
    emit_u8((uint8_t)(v & 0xff));
    emit_u8((uint8_t)((v >> 8) & 0xff));
}

static void emit_u32(uint32_t v)
{
    emit_u8((uint8_t)(v & 0xff));
    emit_u8((uint8_t)((v >> 8) & 0xff));
    emit_u8((uint8_t)((v >> 16) & 0xff));
    emit_u8((uint8_t)((v >> 24) & 0xff));
}

static void patch_u32(uint32_t off, uint32_t v)
{
    g_code[off + 0] = (uint8_t)(v & 0xff);
    g_code[off + 1] = (uint8_t)((v >> 8) & 0xff);
    g_code[off + 2] = (uint8_t)((v >> 16) & 0xff);
    g_code[off + 3] = (uint8_t)((v >> 24) & 0xff);
}

static uint32_t add_string(const char *s, int len)
{
    uint32_t off = g_consts_pos;
    int i;
    if (g_consts_pos + (uint32_t)len + 1 > CC_CONSTS_MAX)
    {
        cc_err("const pool overflow");
        return 0;
    }
    for (i = 0; i < len; i++) g_consts[g_consts_pos++] = (uint8_t)s[i];
    g_consts[g_consts_pos++] = 0;
    return off;
}

static int find_local(const char *name)
{
    int i;
    for (i = 0; i < g_nlocals; i++)
        if (0 == strcmp(g_locals[i].name, name)) return i;
    return -1;
}

static int alloc_local(const char *name)
{
    if (g_nlocals >= CC_NLOCALS) { cc_err("too many locals"); return -1; }
    if (find_local(name) >= 0)   { cc_err("duplicate local"); return -1; }
    if (strlen(name) >= CC_NAMELEN) { cc_err("name too long"); return -1; }
    strcpy(g_locals[g_nlocals].name, name);
    g_locals[g_nlocals].slot = g_nlocals;
    return g_nlocals++;
}

/* ---------- lexer ---------- */

static int peek(int n) { return g_src[g_pos + n]; }
static int curc(void)  { return g_src[g_pos]; }

static void skip_ws_and_comments(void)
{
    for (;;)
    {
        int c = curc();
        if (c == ' ' || c == '\t' || c == '\r')
        {
            g_pos++;
        }
        else if (c == '\n')
        {
            g_line++;
            g_pos++;
        }
        else if (c == '/' && peek(1) == '/')
        {
            while (curc() && curc() != '\n') g_pos++;
        }
        else if (c == '/' && peek(1) == '*')
        {
            g_pos += 2;
            while (curc())
            {
                if (curc() == '\n') g_line++;
                if (curc() == '*' && peek(1) == '/') { g_pos += 2; break; }
                g_pos++;
            }
        }
        else if (c == '#')
        {
            /* preprocessor line — skip to EOL */
            while (curc() && curc() != '\n') g_pos++;
        }
        else
        {
            break;
        }
    }
}

static int is_id_start(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_id_cont(int c)
{
    return is_id_start(c) || (c >= '0' && c <= '9');
}

static int kw_lookup(const char *s)
{
    if (0 == strcmp(s, "int"))    return TK_INTKW;
    if (0 == strcmp(s, "void"))   return TK_VOIDKW;
    if (0 == strcmp(s, "char"))   return TK_CHARKW;
    if (0 == strcmp(s, "return")) return TK_RETURN;
    if (0 == strcmp(s, "if"))     return TK_IF;
    if (0 == strcmp(s, "else"))   return TK_ELSE;
    if (0 == strcmp(s, "while"))  return TK_WHILE;
    if (0 == strcmp(s, "for"))    return TK_FOR;
    return 0;
}

static void next_token(void)
{
    int c, kw, n;

    skip_ws_and_comments();
    c = curc();
    if (c == 0) { g_tok = TK_EOF; return; }

    if (is_id_start(c))
    {
        n = 0;
        while (is_id_cont(curc()) && n < (int)sizeof(g_tok_str) - 1)
            g_tok_str[n++] = g_src[g_pos++];
        g_tok_str[n] = 0;
        g_tok_strlen = n;
        kw = kw_lookup(g_tok_str);
        g_tok = kw ? kw : TK_IDENT;
        return;
    }
    if (c >= '0' && c <= '9')
    {
        int32_t v = 0;
        while (curc() >= '0' && curc() <= '9')
            v = v * 10 + (g_src[g_pos++] - '0');
        g_tok_int = v;
        g_tok = TK_INT;
        return;
    }
    if (c == '"')
    {
        g_pos++;
        n = 0;
        while (curc() && curc() != '"')
        {
            int ch = g_src[g_pos++];
            if (ch == '\\')
            {
                int e = g_src[g_pos++];
                switch (e)
                {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case '0': ch = '\0'; break;
                case '\\': ch = '\\'; break;
                case '"': ch = '"'; break;
                default:  ch = e;    break;
                }
            }
            if (n < (int)sizeof(g_tok_str) - 1) g_tok_str[n++] = ch;
        }
        if (curc() == '"') g_pos++;
        g_tok_str[n] = 0;
        g_tok_strlen = n;
        g_tok = TK_STR;
        return;
    }
    if (c == '\'')
    {
        int ch;
        g_pos++;
        ch = g_src[g_pos++];
        if (ch == '\\')
        {
            int e = g_src[g_pos++];
            switch (e)
            {
            case 'n': ch = '\n'; break;
            case 't': ch = '\t'; break;
            case 'r': ch = '\r'; break;
            case '0': ch = '\0'; break;
            case '\\': ch = '\\'; break;
            case '\'': ch = '\''; break;
            default:  ch = e;    break;
            }
        }
        if (curc() == '\'') g_pos++;
        g_tok_int = ch;
        g_tok = TK_INT;
        return;
    }

    g_pos++;
    switch (c)
    {
    case '(': g_tok = TK_LP;     return;
    case ')': g_tok = TK_RP;     return;
    case '{': g_tok = TK_LB;     return;
    case '}': g_tok = TK_RB;     return;
    case ';': g_tok = TK_SEMI;   return;
    case ',': g_tok = TK_COMMA;  return;
    case '+': g_tok = TK_PLUS;   return;
    case '-': g_tok = TK_MINUS;  return;
    case '*': g_tok = TK_STAR;   return;
    case '/': g_tok = TK_SLASH;  return;
    case '%': g_tok = TK_PCT;    return;
    case '=':
        if (curc() == '=') { g_pos++; g_tok = TK_EQ; }
        else                 g_tok = TK_ASSIGN;
        return;
    case '!':
        if (curc() == '=') { g_pos++; g_tok = TK_NE; }
        else                 g_tok = TK_NOT;
        return;
    case '<':
        if (curc() == '=') { g_pos++; g_tok = TK_LE; }
        else                 g_tok = TK_LT;
        return;
    case '>':
        if (curc() == '=') { g_pos++; g_tok = TK_GE; }
        else                 g_tok = TK_GT;
        return;
    case '&':
        if (curc() == '&') { g_pos++; g_tok = TK_LAND; return; }
        cc_err("unsupported operator '&'"); g_tok = TK_EOF; return;
    case '|':
        if (curc() == '|') { g_pos++; g_tok = TK_LOR; return; }
        cc_err("unsupported operator '|'"); g_tok = TK_EOF; return;
    default:
        cc_err("unexpected character"); g_tok = TK_EOF; return;
    }
}

static int accept(int t) { if (g_tok == t) { next_token(); return 1; } return 0; }
static void expect(int t, const char *what)
{
    if (!accept(t)) cc_err(what);
}

/* ---------- parser / codegen ---------- */

static void compile_expr(void);

static int builtin_id(const char *name)
{
    if (0 == strcmp(name, "printf"))    return BI_PRINTF;
    if (0 == strcmp(name, "puts"))      return BI_PUTS;
    if (0 == strcmp(name, "putchar"))   return BI_PUTCHAR;
    if (0 == strcmp(name, "getchar"))   return BI_GETCHAR;
    if (0 == strcmp(name, "exit"))      return BI_EXIT;
    if (0 == strcmp(name, "rgb"))       return BI_RGB;
    if (0 == strcmp(name, "wm_line"))   return BI_WM_LINE;
    if (0 == strcmp(name, "wm_render")) return BI_WM_RENDER;
    if (0 == strcmp(name, "wm_clear"))  return BI_WM_CLEAR;
    if (0 == strcmp(name, "sleep_ms"))  return BI_SLEEP_MS;
    if (0 == strcmp(name, "isin"))      return BI_ISIN;
    if (0 == strcmp(name, "icos"))      return BI_ICOS;
    if (0 == strcmp(name, "screen_w"))  return BI_SCREEN_W;
    if (0 == strcmp(name, "screen_h"))  return BI_SCREEN_H;
    return -1;
}

static void compile_call(const char *callee)
{
    int bi = builtin_id(callee);
    int nargs = 0;
    if (bi < 0) { cc_err("unknown function"); return; }

    expect(TK_LP, "expected '('");
    if (g_tok != TK_RP)
    {
        for (;;)
        {
            /* If the argument is a bare string literal, emit PUSH_STR with
             * the const-pool offset.  Otherwise compile as integer expr. */
            if (g_tok == TK_STR)
            {
                uint32_t off = add_string(g_tok_str, g_tok_strlen);
                emit_u8(OP_PUSH_STR);
                emit_u32(off);
                next_token();
            }
            else
            {
                compile_expr();
            }
            nargs++;
            if (!accept(TK_COMMA)) break;
        }
    }
    expect(TK_RP, "expected ')'");

    if (nargs > 255) { cc_err("too many call args"); nargs = 255; }
    emit_u8(OP_CALL_BI);
    emit_u8((uint8_t)bi);
    emit_u8((uint8_t)nargs);
}

/* primary := IDENT | IDENT '(' args ')' | INT | STR | '(' expr ')' */
static void compile_primary(void)
{
    if (g_tok == TK_INT)
    {
        emit_u8(OP_PUSH_I32);
        emit_u32((uint32_t)g_tok_int);
        next_token();
        return;
    }
    if (g_tok == TK_STR)
    {
        uint32_t off = add_string(g_tok_str, g_tok_strlen);
        emit_u8(OP_PUSH_STR);
        emit_u32(off);
        next_token();
        return;
    }
    if (g_tok == TK_LP)
    {
        next_token();
        compile_expr();
        expect(TK_RP, "expected ')'");
        return;
    }
    if (g_tok == TK_IDENT)
    {
        char name[CC_NAMELEN];
        strlcpy(name, g_tok_str, sizeof(name));
        next_token();
        if (g_tok == TK_LP)
        {
            compile_call(name);
            return;
        }
        {
            int slot = find_local(name);
            if (slot < 0) { cc_err("undeclared identifier"); return; }
            emit_u8(OP_LOAD_LOC);
            emit_u16((uint16_t)slot);
        }
        return;
    }
    cc_err("expected expression");
}

/* unary := ('-' | '!') unary | primary */
static void compile_unary(void)
{
    if (accept(TK_MINUS))
    {
        compile_unary();
        emit_u8(OP_NEG);
        return;
    }
    if (accept(TK_NOT))
    {
        compile_unary();
        emit_u8(OP_NOT);
        return;
    }
    compile_primary();
}

static void compile_mul(void)
{
    int op;
    compile_unary();
    while (g_tok == TK_STAR || g_tok == TK_SLASH || g_tok == TK_PCT)
    {
        op = g_tok;
        next_token();
        compile_unary();
        emit_u8((uint8_t)(op == TK_STAR  ? OP_MUL :
                          op == TK_SLASH ? OP_DIV : OP_MOD));
    }
}

static void compile_add(void)
{
    int op;
    compile_mul();
    while (g_tok == TK_PLUS || g_tok == TK_MINUS)
    {
        op = g_tok;
        next_token();
        compile_mul();
        emit_u8((uint8_t)(op == TK_PLUS ? OP_ADD : OP_SUB));
    }
}

static void compile_rel(void)
{
    int op;
    compile_add();
    while (g_tok == TK_LT || g_tok == TK_LE ||
           g_tok == TK_GT || g_tok == TK_GE)
    {
        op = g_tok;
        next_token();
        compile_add();
        emit_u8((uint8_t)(op == TK_LT ? OP_LT :
                          op == TK_LE ? OP_LE :
                          op == TK_GT ? OP_GT : OP_GE));
    }
}

static void compile_equ(void)
{
    int op;
    compile_rel();
    while (g_tok == TK_EQ || g_tok == TK_NE)
    {
        op = g_tok;
        next_token();
        compile_rel();
        emit_u8((uint8_t)(op == TK_EQ ? OP_EQ : OP_NE));
    }
}

static void compile_land(void)
{
    compile_equ();
    while (accept(TK_LAND))
    {
        compile_equ();
        emit_u8(OP_LAND);
    }
}

static void compile_lor(void)
{
    compile_land();
    while (accept(TK_LOR))
    {
        compile_land();
        emit_u8(OP_LOR);
    }
}

/* assignment := IDENT '=' assignment | lor
 * (Only an identifier is a valid assignment target.) */
static void compile_assign(void)
{
    /* peek for IDENT '=' */
    if (g_tok == TK_IDENT)
    {
        int saved_pos = g_pos;
        int saved_line = g_line;
        char name[CC_NAMELEN];
        strlcpy(name, g_tok_str, sizeof(name));
        next_token();
        if (g_tok == TK_ASSIGN)
        {
            int slot;
            next_token();
            compile_assign();           /* right-hand side */
            slot = find_local(name);
            if (slot < 0) { cc_err("undeclared identifier"); return; }
            /* leave value on stack but also store: DUP, STORE_LOC */
            emit_u8(OP_DUP);
            emit_u8(OP_STORE_LOC);
            emit_u16((uint16_t)slot);
            emit_u8(OP_POP);            /* drop the duplicate */
            /* Push the stored value back so that compound expressions like
             * `a = b = 1` work.  Simpler: just LOAD_LOC again. */
            emit_u8(OP_LOAD_LOC);
            emit_u16((uint16_t)slot);
            return;
        }
        /* not assignment — rewind: reparse identifier as primary */
        g_pos = saved_pos;
        g_line = saved_line;
        /* reload the identifier token */
        g_tok = TK_IDENT;
        strlcpy(g_tok_str, name, sizeof(g_tok_str));
        g_tok_strlen = strlen(name);
    }
    compile_lor();
}

static void compile_expr(void) { compile_assign(); }

/* Forward decls */
static void compile_stmt(void);
static void compile_block(void);

static void compile_decl(void)
{
    /* `int IDENT (= expr)? ;`   (current token already consumed: TK_INTKW) */
    int slot;
    if (g_tok != TK_IDENT) { cc_err("expected identifier"); return; }
    slot = alloc_local(g_tok_str);
    next_token();
    if (accept(TK_ASSIGN))
    {
        compile_expr();
        emit_u8(OP_STORE_LOC);
        emit_u16((uint16_t)slot);
    }
    expect(TK_SEMI, "expected ';'");
}

static void compile_if(void)
{
    uint32_t jz_at, jmp_at;

    expect(TK_LP, "expected '(' after if");
    compile_expr();
    expect(TK_RP, "expected ')'");

    /* JZ over then-branch */
    emit_u8(OP_JZ);
    jz_at = g_code_pos;
    emit_u32(0);

    compile_stmt();

    if (accept(TK_ELSE))
    {
        emit_u8(OP_JMP);
        jmp_at = g_code_pos;
        emit_u32(0);
        /* patch JZ to land here */
        patch_u32(jz_at, g_code_pos - (jz_at + 4));
        compile_stmt();
        patch_u32(jmp_at, g_code_pos - (jmp_at + 4));
    }
    else
    {
        patch_u32(jz_at, g_code_pos - (jz_at + 4));
    }
}

static void compile_while(void)
{
    uint32_t loop_top, jz_at;

    expect(TK_LP, "expected '(' after while");
    loop_top = g_code_pos;
    compile_expr();
    expect(TK_RP, "expected ')'");

    emit_u8(OP_JZ);
    jz_at = g_code_pos;
    emit_u32(0);

    compile_stmt();

    /* unconditional jmp back to loop_top */
    emit_u8(OP_JMP);
    emit_u32(loop_top - (g_code_pos + 4));

    patch_u32(jz_at, g_code_pos - (jz_at + 4));
}

static void compile_for(void)
{
    /* `for ( init? ; cond? ; step? ) stmt`
     *
     * Layout:
     *   <init>
     * top:
     *   <cond>          (default: PUSH_I32 1)
     *   JZ end
     *   JMP body
     * step:
     *   <step>
     *   JMP top
     * body:
     *   <stmt>
     *   JMP step
     * end:
     */
    uint32_t top, jz_end_at, jmp_body_at, step, body, end, jmp_top_at, jmp_step_at;

    expect(TK_LP, "expected '(' after for");

    /* init */
    if (g_tok == TK_INTKW) { next_token(); compile_decl(); }
    else if (g_tok != TK_SEMI) { compile_expr(); emit_u8(OP_POP); expect(TK_SEMI, "expected ';'"); }
    else { next_token(); }

    top = g_code_pos;

    /* cond */
    if (g_tok != TK_SEMI) compile_expr();
    else { emit_u8(OP_PUSH_I32); emit_u32(1); }
    expect(TK_SEMI, "expected ';'");

    emit_u8(OP_JZ);
    jz_end_at = g_code_pos;
    emit_u32(0);

    emit_u8(OP_JMP);
    jmp_body_at = g_code_pos;
    emit_u32(0);

    step = g_code_pos;
    if (g_tok != TK_RP) { compile_expr(); emit_u8(OP_POP); }
    expect(TK_RP, "expected ')'");

    emit_u8(OP_JMP);
    jmp_top_at = g_code_pos;
    emit_u32(0);

    body = g_code_pos;
    patch_u32(jmp_body_at, body - (jmp_body_at + 4));

    compile_stmt();
    emit_u8(OP_JMP);
    jmp_step_at = g_code_pos;
    emit_u32(0);

    patch_u32(jmp_top_at, top  - (jmp_top_at  + 4));
    patch_u32(jmp_step_at, step - (jmp_step_at + 4));

    end = g_code_pos;
    patch_u32(jz_end_at, end - (jz_end_at + 4));
}

static void compile_return(void)
{
    if (g_tok != TK_SEMI) compile_expr();
    else { emit_u8(OP_PUSH_I32); emit_u32(0); }
    expect(TK_SEMI, "expected ';'");
    emit_u8(OP_RET);
}

static void compile_stmt(void)
{
    if (g_tok == TK_LB) { compile_block(); return; }

    if (accept(TK_INTKW)) { compile_decl();   return; }
    if (accept(TK_IF))    { compile_if();     return; }
    if (accept(TK_WHILE)) { compile_while();  return; }
    if (accept(TK_FOR))   { compile_for();    return; }
    if (accept(TK_RETURN)){ compile_return(); return; }

    if (accept(TK_SEMI)) return;

    /* expression statement */
    compile_expr();
    emit_u8(OP_POP);
    expect(TK_SEMI, "expected ';'");
}

static void compile_block(void)
{
    expect(TK_LB, "expected '{'");
    while (g_tok != TK_RB && g_tok != TK_EOF) compile_stmt();
    expect(TK_RB, "expected '}'");
}

/* program := 'int' 'main' '(' 'void'? ')' block (more functions ignored for v1) */
static int compile_program(uint32_t *out_entry, uint32_t *out_nlocals)
{
    uint32_t enter_at, nlocals_off;
    int saw_return = 0;

    next_token();

    /* Find `int main(...)` — skip any leading decls we don't support yet. */
    while (g_tok != TK_EOF)
    {
        if (g_tok == TK_INTKW)
        {
            next_token();
            if (g_tok == TK_IDENT && 0 == strcmp(g_tok_str, "main"))
            {
                next_token();
                expect(TK_LP, "expected '('");
                if (g_tok == TK_VOIDKW) next_token();
                else if (g_tok != TK_RP) { cc_err("only int main(void) supported"); }
                expect(TK_RP, "expected ')'");
                break;
            }
            cc_err("only int main(void) supported in v1");
            return -1;
        }
        next_token();
    }
    if (g_tok != TK_LB) { cc_err("expected '{' for main body"); return -1; }

    *out_entry = g_code_pos;
    emit_u8(OP_ENTER);
    nlocals_off = g_code_pos;
    emit_u16(0);                /* fixed up after parse */
    enter_at = nlocals_off;     /* unused */
    (void)enter_at;

    compile_block();

    /* implicit "return 0" if the body didn't end with one */
    {
        uint32_t last = (g_code_pos > 0) ? (g_code_pos - 1) : 0;
        if (g_code_pos == 0 || g_code[last] != OP_RET) saw_return = 0;
        else saw_return = 1;
    }
    if (!saw_return)
    {
        emit_u8(OP_PUSH_I32);
        emit_u32(0);
        emit_u8(OP_RET);
    }

    /* fix up nlocals operand of ENTER */
    g_code[nlocals_off + 0] = (uint8_t)(g_nlocals & 0xff);
    g_code[nlocals_off + 1] = (uint8_t)((g_nlocals >> 8) & 0xff);
    *out_nlocals = (uint32_t)g_nlocals;
    return 0;
}

/* ---------- entry point ---------- */

static int load_source(const char *path, char **out_buf, uint *out_len)
{
    struct xinode in;
    int fd, n;
    char *buf;

    if (OK != xfsStat(path, &in, NULL)) return SYSERR;
    buf = (char *)memget(in.size + 1);
    if ((void *)SYSERR == buf) return SYSERR;
    fd = xfsOpen(path, XFS_O_RDONLY);
    if (fd < 0) { memfree(buf, in.size + 1); return SYSERR; }
    n = xfsRead(fd, buf, in.size);
    xfsClose(fd);
    if (n < 0) { memfree(buf, in.size + 1); return SYSERR; }
    buf[n] = 0;
    *out_buf = buf;
    *out_len = (uint)(in.size + 1);
    return OK;
}

static int write_aout(const char *path, struct aout_header *hdr)
{
    int fd, w;
    fd = xfsOpen(path, XFS_O_RDWR | XFS_O_CREAT | XFS_O_TRUNC);
    if (fd < 0) return SYSERR;
    w = xfsWrite(fd, hdr, sizeof(*hdr));
    if (w != (int)sizeof(*hdr)) { xfsClose(fd); return SYSERR; }
    if (hdr->code_size > 0 &&
        xfsWrite(fd, g_code, hdr->code_size) != (int)hdr->code_size)
    { xfsClose(fd); return SYSERR; }
    if (hdr->const_size > 0 &&
        xfsWrite(fd, g_consts, hdr->const_size) != (int)hdr->const_size)
    { xfsClose(fd); return SYSERR; }
    xfsClose(fd);
    return OK;
}

int ccCompile(const char *src_path, const char *out_path)
{
    char *src_buf = NULL;
    uint  src_len = 0;
    struct aout_header hdr;
    uint32_t entry = 0, nlocals = 0;
    int rc;

    g_code   = (uint8_t *)memget(CC_CODE_MAX);
    g_consts = (uint8_t *)memget(CC_CONSTS_MAX);
    if ((void *)SYSERR == g_code || (void *)SYSERR == g_consts)
    {
        printf("cc: out of memory\n");
        return SYSERR;
    }
    g_code_pos = 0;
    g_consts_pos = 0;
    g_nlocals = 0;
    g_err = 0;

    rc = load_source(src_path, &src_buf, &src_len);
    if (rc != OK) { printf("cc: cannot read %s\n", src_path); goto fail; }

    g_src = src_buf;
    g_pos = 0;
    g_line = 1;

    if (compile_program(&entry, &nlocals) < 0 || g_err) goto fail;

    memcpy(hdr.magic, AOUT_MAGIC, 4);
    hdr.version    = AOUT_VERSION;
    hdr.code_size  = g_code_pos;
    hdr.const_size = g_consts_pos;
    hdr.entry      = entry;
    hdr.nlocals    = nlocals;
    hdr.reserved   = 0;

    if (OK != write_aout(out_path, &hdr))
    {
        printf("cc: cannot write %s\n", out_path);
        goto fail;
    }

    if (src_buf) memfree(src_buf, src_len);
    memfree(g_code,   CC_CODE_MAX);
    memfree(g_consts, CC_CONSTS_MAX);
    return OK;

fail:
    if (src_buf) memfree(src_buf, src_len);
    if (g_code   && (void*)SYSERR != g_code)   memfree(g_code,   CC_CODE_MAX);
    if (g_consts && (void*)SYSERR != g_consts) memfree(g_consts, CC_CONSTS_MAX);
    return SYSERR;
}
