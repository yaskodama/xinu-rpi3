/**
 * @file abclc.c
 *
 * ABCL/c+ → C translator.
 *
 * Subset:
 *   - Multiple `class` declarations, each with `var`s and `method`s.
 *   - Methods take parameters and have a body of statements.
 *   - Statements: if/else, while, printf(...), assignment to a field,
 *     send EXPR.method(args), block.
 *   - Implicit identifiers in method bodies: `self` (current actor id),
 *     `sender` (sender actor id).
 *   - main { new Class IDENT; ...  send IDENT.method(args); ... }
 *
 * Target:
 *   The output C is restricted to features supported by the in-XINU `cc`:
 *   only `int main(void)`, scalar `int` locals, if/while/for, arithmetic,
 *   comparisons, printf/puts/putchar.
 *   The actor model is desugared into a state machine driven by a single
 *   in-flight message envelope.  Each method body becomes one branch of
 *   a switch on (class_id, method_id).  Per-actor field storage is laid
 *   out as a flat run of `int pN_field;` declarations.
 *
 * Limitations (intentional, for tractability):
 *   - At most one `send` per method body (only the last send is honored
 *     because we have a single envelope, no queue).  PingPong fits.
 *   - All field types are int.  arg types are int.
 *   - At most 8 actors, 8 fields per class, 8 params per method, 16
 *     methods per class, 4 classes total.
 *   - Method bodies must end with at most one tail send; subsequent
 *     statements after that send may compute but are still emitted.
 */

#include <stddef.h>
#include <stdint.h>
#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <memory.h>
#include <xfs.h>

#define ABCL_NAMELEN   32
#define ABCL_STRLEN    256
#define ABCL_MAX_TOKENS 4096
#define ABCL_MAX_FIELDS  8
#define ABCL_MAX_PARAMS  8
#define ABCL_MAX_METHODS 16
#define ABCL_MAX_CLASSES 4
#define ABCL_MAX_ACTORS  8
#define ABCL_MAX_INITSENDS 8

/* ---------- token kinds ---------- */
enum {
    AT_EOF=0, AT_IDENT, AT_INT, AT_STR,
    AT_CLASS, AT_VAR, AT_METHOD, AT_NEW, AT_SEND, AT_MAIN,
    AT_IF, AT_ELSE, AT_WHILE, AT_RETURN,
    AT_LP, AT_RP, AT_LB, AT_RB, AT_SEMI, AT_COMMA, AT_DOT,
    AT_ASSIGN,
    AT_PLUS, AT_MINUS, AT_STAR, AT_SLASH, AT_PCT,
    AT_EQ, AT_NE, AT_LT, AT_LE, AT_GT, AT_GE,
    AT_NOT, AT_LAND, AT_LOR
};

struct abcl_token {
    int kind;
    int int_val;
    char str[ABCL_STRLEN];
    int  str_len;
    int  line;
};

/* ---------- state ---------- */

static const char *g_src;
static int g_srcpos;
static int g_line;

static struct abcl_token *g_tokens;
static int g_ntokens;
static int g_pos;
static int g_err;

struct abcl_method {
    char name[ABCL_NAMELEN];
    char params[ABCL_MAX_PARAMS][ABCL_NAMELEN];
    int  nparams;
    int  body_start;        /* token index of '{'  */
    int  body_end;          /* token index of '}'  */
};

struct abcl_class {
    char name[ABCL_NAMELEN];
    char fields[ABCL_MAX_FIELDS][ABCL_NAMELEN];
    int  nfields;
    struct abcl_method methods[ABCL_MAX_METHODS];
    int  nmethods;
};

static struct abcl_class g_classes[ABCL_MAX_CLASSES];
static int g_nclasses;

struct abcl_actor {
    char name[ABCL_NAMELEN];
    int  class_id;
};

static struct abcl_actor g_actors[ABCL_MAX_ACTORS];
static int g_nactors;

struct abcl_initsend {
    char target[ABCL_NAMELEN];
    char method[ABCL_NAMELEN];
    int  body_start;        /* token index of '(' */
    int  body_end;          /* token index of ')' */
};

static struct abcl_initsend g_initsends[ABCL_MAX_INITSENDS];
static int g_ninitsends;

static int g_outfd;

/* ---------- diagnostics ---------- */
static void abcl_err(const char *msg)
{
    int line = (g_pos < g_ntokens) ? g_tokens[g_pos].line : g_line;
    if (!g_err) printf("abclc: line %d: %s\n", line, msg);
    g_err = 1;
}

/* ---------- emit helpers ---------- */
static void emit(const char *s)
{
    if (g_outfd >= 0 && s != NULL)
    {
        int n = strlen(s);
        if (n > 0) xfsWrite(g_outfd, s, n);
    }
}
static void emit_int(int n)
{
    char buf[32];
    sprintf(buf, "%d", n);
    emit(buf);
}

/* ---------- lexer ---------- */
static int peekc(int k) { return g_src[g_srcpos + k]; }
static int curc(void)   { return g_src[g_srcpos]; }

static void skip_ws(void)
{
    for (;;)
    {
        int c = curc();
        if (c == ' ' || c == '\t' || c == '\r') g_srcpos++;
        else if (c == '\n') { g_line++; g_srcpos++; }
        else if (c == '/' && peekc(1) == '/')
        {
            while (curc() && curc() != '\n') g_srcpos++;
        }
        else if (c == '/' && peekc(1) == '*')
        {
            g_srcpos += 2;
            while (curc())
            {
                if (curc() == '\n') g_line++;
                if (curc() == '*' && peekc(1) == '/') { g_srcpos += 2; break; }
                g_srcpos++;
            }
        }
        else if (c == '#')
        {
            while (curc() && curc() != '\n') g_srcpos++;
        }
        else break;
    }
}

static int is_id_start(int c)
{ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int is_id_cont(int c)
{ return is_id_start(c)||(c>='0'&&c<='9'); }

static int kw_kind(const char *s)
{
    if (0==strcmp(s,"class")) return AT_CLASS;
    if (0==strcmp(s,"var"))   return AT_VAR;
    if (0==strcmp(s,"method")) return AT_METHOD;
    if (0==strcmp(s,"new")) return AT_NEW;
    if (0==strcmp(s,"send")) return AT_SEND;
    if (0==strcmp(s,"main")) return AT_MAIN;
    if (0==strcmp(s,"if")) return AT_IF;
    if (0==strcmp(s,"else")) return AT_ELSE;
    if (0==strcmp(s,"while")) return AT_WHILE;
    if (0==strcmp(s,"return")) return AT_RETURN;
    return 0;
}

static int lex_one(struct abcl_token *t)
{
    int c, n, kw;
    skip_ws();
    c = curc();
    t->line = g_line;
    if (c == 0) { t->kind = AT_EOF; return 0; }

    if (is_id_start(c))
    {
        n = 0;
        while (is_id_cont(curc()) && n < ABCL_NAMELEN-1)
            t->str[n++] = g_src[g_srcpos++];
        t->str[n] = 0;
        t->str_len = n;
        kw = kw_kind(t->str);
        t->kind = kw ? kw : AT_IDENT;
        return 1;
    }
    if (c >= '0' && c <= '9')
    {
        int v = 0;
        while (curc() >= '0' && curc() <= '9')
            v = v*10 + (g_src[g_srcpos++] - '0');
        t->int_val = v;
        t->kind = AT_INT;
        return 1;
    }
    if (c == '"')
    {
        g_srcpos++;
        n = 0;
        while (curc() && curc() != '"')
        {
            int ch = g_src[g_srcpos++];
            if (ch == '\\' && curc())
            {
                int e = g_src[g_srcpos++];
                /* Keep escapes as-is — they pass through to the C output. */
                if (n < ABCL_STRLEN-2) t->str[n++] = '\\';
                if (n < ABCL_STRLEN-1) t->str[n++] = e;
                continue;
            }
            if (n < ABCL_STRLEN-1) t->str[n++] = ch;
        }
        if (curc() == '"') g_srcpos++;
        t->str[n] = 0;
        t->str_len = n;
        t->kind = AT_STR;
        return 1;
    }

    g_srcpos++;
    switch (c)
    {
    case '(': t->kind = AT_LP;     return 1;
    case ')': t->kind = AT_RP;     return 1;
    case '{': t->kind = AT_LB;     return 1;
    case '}': t->kind = AT_RB;     return 1;
    case ';': t->kind = AT_SEMI;   return 1;
    case ',': t->kind = AT_COMMA;  return 1;
    case '.': t->kind = AT_DOT;    return 1;
    case '+': t->kind = AT_PLUS;   return 1;
    case '-': t->kind = AT_MINUS;  return 1;
    case '*': t->kind = AT_STAR;   return 1;
    case '/': t->kind = AT_SLASH;  return 1;
    case '%': t->kind = AT_PCT;    return 1;
    case '=':
        if (curc()=='=') { g_srcpos++; t->kind = AT_EQ; return 1; }
        t->kind = AT_ASSIGN; return 1;
    case '!':
        if (curc()=='=') { g_srcpos++; t->kind = AT_NE; return 1; }
        t->kind = AT_NOT; return 1;
    case '<':
        if (curc()=='=') { g_srcpos++; t->kind = AT_LE; return 1; }
        t->kind = AT_LT; return 1;
    case '>':
        if (curc()=='=') { g_srcpos++; t->kind = AT_GE; return 1; }
        t->kind = AT_GT; return 1;
    case '&':
        if (curc()=='&') { g_srcpos++; t->kind = AT_LAND; return 1; }
        abcl_err("unsupported &"); t->kind = AT_EOF; return 0;
    case '|':
        if (curc()=='|') { g_srcpos++; t->kind = AT_LOR; return 1; }
        abcl_err("unsupported |"); t->kind = AT_EOF; return 0;
    default:
        abcl_err("unexpected char"); t->kind = AT_EOF; return 0;
    }
}

static int tokenize(void)
{
    g_ntokens = 0;
    g_srcpos  = 0;
    g_line    = 1;
    while (g_ntokens < ABCL_MAX_TOKENS)
    {
        struct abcl_token *t = &g_tokens[g_ntokens++];
        memset(t, 0, sizeof(*t));
        if (!lex_one(t))
        {
            t->kind = AT_EOF;
            break;
        }
        if (g_err) return -1;
    }
    return 0;
}

/* ---------- parser ---------- */

static struct abcl_token *peek(int k)
{
    int i = g_pos + k;
    if (i >= g_ntokens) return &g_tokens[g_ntokens - 1];
    return &g_tokens[i];
}
static int  accept(int k) { if (peek(0)->kind == k) { g_pos++; return 1; } return 0; }
static void expect(int k, const char *what) { if (!accept(k)) abcl_err(what); }

static int find_class(const char *name)
{
    int i;
    for (i = 0; i < g_nclasses; i++)
        if (0 == strcmp(g_classes[i].name, name)) return i;
    return -1;
}

static int find_actor(const char *name)
{
    int i;
    for (i = 0; i < g_nactors; i++)
        if (0 == strcmp(g_actors[i].name, name)) return i;
    return -1;
}

static int find_field(struct abcl_class *cl, const char *name)
{
    int i;
    for (i = 0; i < cl->nfields; i++)
        if (0 == strcmp(cl->fields[i], name)) return i;
    return -1;
}

static int find_method(struct abcl_class *cl, const char *name)
{
    int i;
    for (i = 0; i < cl->nmethods; i++)
        if (0 == strcmp(cl->methods[i].name, name)) return i;
    return -1;
}

static int find_param(struct abcl_method *m, const char *name)
{
    int i;
    for (i = 0; i < m->nparams; i++)
        if (0 == strcmp(m->params[i], name)) return i;
    return -1;
}

static void parse_class(void)
{
    struct abcl_class *cl;
    if (g_nclasses >= ABCL_MAX_CLASSES) { abcl_err("too many classes"); return; }
    cl = &g_classes[g_nclasses++];
    cl->nfields = 0;
    cl->nmethods = 0;

    if (peek(0)->kind != AT_IDENT) { abcl_err("class name"); return; }
    strlcpy(cl->name, peek(0)->str, ABCL_NAMELEN);
    g_pos++;
    expect(AT_LB, "expected '{' after class");

    while (peek(0)->kind != AT_RB && peek(0)->kind != AT_EOF)
    {
        if (accept(AT_VAR))
        {
            if (peek(0)->kind != AT_IDENT) { abcl_err("var name"); return; }
            if (cl->nfields >= ABCL_MAX_FIELDS)
            { abcl_err("too many vars"); return; }
            strlcpy(cl->fields[cl->nfields++], peek(0)->str, ABCL_NAMELEN);
            g_pos++;
            expect(AT_SEMI, "expected ';' after var");
            continue;
        }
        if (accept(AT_METHOD))
        {
            struct abcl_method *m;
            if (cl->nmethods >= ABCL_MAX_METHODS)
            { abcl_err("too many methods"); return; }
            m = &cl->methods[cl->nmethods++];
            m->nparams = 0;

            if (peek(0)->kind != AT_IDENT) { abcl_err("method name"); return; }
            strlcpy(m->name, peek(0)->str, ABCL_NAMELEN);
            g_pos++;
            expect(AT_LP, "expected '(' after method name");
            if (peek(0)->kind != AT_RP)
            {
                for (;;)
                {
                    if (peek(0)->kind != AT_IDENT)
                    { abcl_err("param name"); return; }
                    if (m->nparams >= ABCL_MAX_PARAMS)
                    { abcl_err("too many params"); return; }
                    strlcpy(m->params[m->nparams++], peek(0)->str, ABCL_NAMELEN);
                    g_pos++;
                    if (!accept(AT_COMMA)) break;
                }
            }
            expect(AT_RP, "expected ')'");
            if (peek(0)->kind != AT_LB) { abcl_err("expected '{' for method body"); return; }
            m->body_start = g_pos;
            {
                int depth = 0;
                while (peek(0)->kind != AT_EOF)
                {
                    if (peek(0)->kind == AT_LB) depth++;
                    else if (peek(0)->kind == AT_RB)
                    {
                        depth--;
                        if (depth == 0) break;
                    }
                    g_pos++;
                }
            }
            m->body_end = g_pos;
            expect(AT_RB, "expected '}'");
            continue;
        }
        abcl_err("expected var or method");
        return;
    }
    expect(AT_RB, "expected '}'");
}

static void parse_main(void)
{
    expect(AT_LB, "expected '{' after main");
    while (peek(0)->kind != AT_RB && peek(0)->kind != AT_EOF)
    {
        if (accept(AT_NEW))
        {
            int cls;
            if (g_nactors >= ABCL_MAX_ACTORS) { abcl_err("too many actors"); return; }
            if (peek(0)->kind != AT_IDENT) { abcl_err("class name in new"); return; }
            cls = find_class(peek(0)->str);
            if (cls < 0) { abcl_err("unknown class in new"); return; }
            g_pos++;
            if (peek(0)->kind != AT_IDENT) { abcl_err("actor name in new"); return; }
            strlcpy(g_actors[g_nactors].name, peek(0)->str, ABCL_NAMELEN);
            g_actors[g_nactors].class_id = cls;
            g_nactors++;
            g_pos++;
            expect(AT_SEMI, "expected ';' after new");
            continue;
        }
        if (accept(AT_SEND))
        {
            struct abcl_initsend *s;
            if (g_ninitsends >= ABCL_MAX_INITSENDS)
            { abcl_err("too many initial sends"); return; }
            s = &g_initsends[g_ninitsends++];
            if (peek(0)->kind != AT_IDENT) { abcl_err("target in send"); return; }
            strlcpy(s->target, peek(0)->str, ABCL_NAMELEN);
            g_pos++;
            expect(AT_DOT, "expected '.' in send");
            if (peek(0)->kind != AT_IDENT) { abcl_err("method in send"); return; }
            strlcpy(s->method, peek(0)->str, ABCL_NAMELEN);
            g_pos++;
            if (peek(0)->kind != AT_LP) { abcl_err("expected '(' in send"); return; }
            s->body_start = g_pos;
            {
                int depth = 0;
                while (peek(0)->kind != AT_EOF)
                {
                    if (peek(0)->kind == AT_LP) depth++;
                    else if (peek(0)->kind == AT_RP)
                    {
                        depth--;
                        if (depth == 0) break;
                    }
                    g_pos++;
                }
            }
            s->body_end = g_pos;
            expect(AT_RP, "expected ')' in send");
            expect(AT_SEMI, "expected ';' after send");
            continue;
        }
        abcl_err("expected new/send in main");
        return;
    }
    expect(AT_RB, "expected '}'");
}

static int parse_program(void)
{
    g_pos = 0;
    while (peek(0)->kind != AT_EOF)
    {
        if (accept(AT_CLASS)) { parse_class(); continue; }
        if (accept(AT_MAIN))  { parse_main();  continue; }
        abcl_err("expected class or main at top level");
        return -1;
    }
    if (g_err) return -1;
    return 0;
}

/* ---------- code generation ---------- */

/* Maximum number of params used across any method (drives envelope size). */
static int max_method_params(void)
{
    int i, j, m = 0;
    for (i = 0; i < g_nclasses; i++)
        for (j = 0; j < g_classes[i].nmethods; j++)
            if (g_classes[i].methods[j].nparams > m)
                m = g_classes[i].methods[j].nparams;
    return m;
}

/* Translate one expression-token into C, recognizing identifiers as
 * params/fields/actors/self/sender.  Returns next token index. */
static int emit_expr_until(int start, int end, struct abcl_class *cl,
                           struct abcl_method *meth);

/* Emit an identifier reference, expanding self/sender/params/fields/actors. */
static void emit_ident_ref(const char *name, struct abcl_class *cl,
                           struct abcl_method *meth)
{
    int idx;

    if (0 == strcmp(name, "self"))   { emit("m_recv");   return; }
    if (0 == strcmp(name, "sender")) { emit("m_sender"); return; }

    if (meth)
    {
        idx = find_param(meth, name);
        if (idx >= 0) { emit("m_arg"); emit_int(idx); return; }
    }
    if (cl)
    {
        idx = find_field(cl, name);
        if (idx >= 0)
        {
            /* "current copy" of the active actor's field */
            emit("cur_"); emit(cl->name); emit("_"); emit(name);
            return;
        }
    }
    /* actor name (only valid in main / send-arg context) */
    idx = find_actor(name);
    if (idx >= 0) { emit_int(idx); return; }

    /* fallback: emit literally (allows referring to method-local synthetic
     * names that the translator has emitted, like `new_arg0`) */
    emit(name);
}

/* Emit text for an expression delimited by [start,end) — token indices. */
static int emit_expr_until(int start, int end, struct abcl_class *cl,
                           struct abcl_method *meth)
{
    int i;
    for (i = start; i < end; i++)
    {
        struct abcl_token *t = &g_tokens[i];
        switch (t->kind)
        {
        case AT_IDENT:  emit_ident_ref(t->str, cl, meth); break;
        case AT_INT:    emit_int(t->int_val); break;
        case AT_STR:    emit("\""); emit(t->str); emit("\""); break;
        case AT_LP:     emit("("); break;
        case AT_RP:     emit(")"); break;
        case AT_COMMA:  emit(", "); break;
        case AT_DOT:    emit("."); break;
        case AT_PLUS:   emit(" + "); break;
        case AT_MINUS:  emit(" - "); break;
        case AT_STAR:   emit(" * "); break;
        case AT_SLASH:  emit(" / "); break;
        case AT_PCT:    emit(" %% "); break;
        case AT_EQ:     emit(" == "); break;
        case AT_NE:     emit(" != "); break;
        case AT_LT:     emit(" < "); break;
        case AT_LE:     emit(" <= "); break;
        case AT_GT:     emit(" > "); break;
        case AT_GE:     emit(" >= "); break;
        case AT_NOT:    emit(" !"); break;
        case AT_LAND:   emit(" && "); break;
        case AT_LOR:    emit(" || "); break;
        default:        emit(" "); break;
        }
    }
    return end;
}

/* Recursive emitter for method body statements. */
static void emit_block(int *pp, int end, struct abcl_class *cl,
                       struct abcl_method *meth, int indent);

static void emit_indent(int n)
{
    while (n-- > 0) emit("    ");
}

/* Find the matching closing token (RB for LB, RP for LP). */
static int find_match(int p, int open_kind, int close_kind)
{
    int depth = 1;
    p++;
    while (p < g_ntokens)
    {
        if (g_tokens[p].kind == open_kind) depth++;
        else if (g_tokens[p].kind == close_kind)
        {
            depth--;
            if (depth == 0) return p;
        }
        p++;
    }
    return p;
}

/* Find next semicolon at depth 0 (relative to () and []) starting at p. */
static int find_semi(int p)
{
    int paren = 0;
    while (p < g_ntokens)
    {
        if (g_tokens[p].kind == AT_LP) paren++;
        else if (g_tokens[p].kind == AT_RP) paren--;
        else if (g_tokens[p].kind == AT_SEMI && paren == 0) return p;
        p++;
    }
    return p;
}

/* Emit a `send` statement: writes new_recv/new_sender/new_arg* and sets
 * sent=1 inside the method body. */
static void emit_send(int target_tok, int method_tok,
                      int args_start, int args_end,
                      struct abcl_class *cl, struct abcl_method *meth,
                      int indent)
{
    /* target: an expression yielding actor id (e.g. `other`, `self`, or
     * a global actor name). */
    int target_recv_id_emit_start = target_tok;
    int target_recv_id_emit_end   = method_tok - 1;  /* skips the '.' */

    /* Compute new_recv */
    emit_indent(indent); emit("new_recv = ");
    emit_expr_until(target_recv_id_emit_start, target_recv_id_emit_end,
                    cl, meth);
    emit(";\n");

    /* new_sender = self */
    emit_indent(indent); emit("new_sender = m_recv;\n");

    /* new_class is determined by the receiver's class.  In the current
     * subset, all actors are of the same class as the receiver — we
     * encode the method id assuming the receiver is of class `cl`.
     * For a single-class program (PingPong), this is always correct. */

    /* Method id within the receiver's class.  Look up in same class as
     * caller for now (single-class assumption). */
    {
        int mid;
        char name[ABCL_NAMELEN];
        strlcpy(name, g_tokens[method_tok].str, ABCL_NAMELEN);
        mid = find_method(cl, name);
        if (mid < 0)
        {
            abcl_err("unknown method in send");
            return;
        }
        emit_indent(indent); emit("new_meth = ");
        emit_int(mid);
        emit(";\n");
    }

    /* Arguments — comma-separated within (args_start, args_end). */
    {
        int p = args_start + 1;     /* skip '(' */
        int aidx = 0;
        int last = p;
        while (p < args_end)
        {
            if (g_tokens[p].kind == AT_COMMA && /* depth 0 only */ 1)
            {
                /* check depth 0: we don't allow nested calls in send args
                 * but we do count parens */
                int depth = 0;
                int q;
                for (q = args_start + 1; q < p; q++)
                {
                    if (g_tokens[q].kind == AT_LP) depth++;
                    else if (g_tokens[q].kind == AT_RP) depth--;
                }
                if (depth == 0)
                {
                    emit_indent(indent); emit("new_arg"); emit_int(aidx++);
                    emit(" = ");
                    emit_expr_until(last, p, cl, meth);
                    emit(";\n");
                    last = p + 1;
                }
            }
            p++;
        }
        if (last < args_end)
        {
            emit_indent(indent); emit("new_arg"); emit_int(aidx++);
            emit(" = ");
            emit_expr_until(last, args_end, cl, meth);
            emit(";\n");
        }
    }

    emit_indent(indent); emit("sent = 1;\n");
}

/* Emit a printf statement. */
static void emit_printf(int args_start, int args_end,
                        struct abcl_class *cl, struct abcl_method *meth,
                        int indent)
{
    emit_indent(indent); emit("printf(");
    /* args_start is '(' of printf, args_end is ')' */
    emit_expr_until(args_start + 1, args_end, cl, meth);
    emit(");\n");
}

/* Emit a single statement starting at *pp. */
static void emit_stmt(int *pp, struct abcl_class *cl,
                      struct abcl_method *meth, int indent)
{
    int p = *pp;
    struct abcl_token *t = &g_tokens[p];

    if (t->kind == AT_LB)
    {
        int end = find_match(p, AT_LB, AT_RB);
        emit_indent(indent); emit("{\n");
        p++;
        emit_block(&p, end, cl, meth, indent + 1);
        emit_indent(indent); emit("}\n");
        if (p < end) p = end;
        *pp = p + 1;
        return;
    }
    if (t->kind == AT_IF)
    {
        int lp = p + 1;
        int rp = find_match(lp, AT_LP, AT_RP);
        int body_p;
        emit_indent(indent); emit("if (");
        emit_expr_until(lp + 1, rp, cl, meth);
        emit(") ");
        body_p = rp + 1;
        if (g_tokens[body_p].kind == AT_LB)
        {
            int end = find_match(body_p, AT_LB, AT_RB);
            emit("{\n");
            body_p++;
            emit_block(&body_p, end, cl, meth, indent + 1);
            emit_indent(indent); emit("}");
            body_p = end + 1;
        }
        else
        {
            emit("{\n");
            emit_stmt(&body_p, cl, meth, indent + 1);
            emit_indent(indent); emit("}");
        }
        if (g_tokens[body_p].kind == AT_ELSE)
        {
            int else_p = body_p + 1;
            emit(" else ");
            if (g_tokens[else_p].kind == AT_LB)
            {
                int end = find_match(else_p, AT_LB, AT_RB);
                emit("{\n");
                else_p++;
                emit_block(&else_p, end, cl, meth, indent + 1);
                emit_indent(indent); emit("}\n");
                else_p = end + 1;
            }
            else
            {
                emit("{\n");
                emit_stmt(&else_p, cl, meth, indent + 1);
                emit_indent(indent); emit("}\n");
            }
            *pp = else_p;
        }
        else
        {
            emit("\n");
            *pp = body_p;
        }
        return;
    }
    if (t->kind == AT_WHILE)
    {
        int lp = p + 1;
        int rp = find_match(lp, AT_LP, AT_RP);
        int body_p = rp + 1;
        emit_indent(indent); emit("while (");
        emit_expr_until(lp + 1, rp, cl, meth);
        emit(") {\n");
        if (g_tokens[body_p].kind == AT_LB)
        {
            int end = find_match(body_p, AT_LB, AT_RB);
            body_p++;
            emit_block(&body_p, end, cl, meth, indent + 1);
            body_p = end + 1;
        }
        else
        {
            emit_stmt(&body_p, cl, meth, indent + 1);
        }
        emit_indent(indent); emit("}\n");
        *pp = body_p;
        return;
    }
    if (t->kind == AT_SEND)
    {
        int target = p + 1;
        int dot = target + 1;
        int method = dot + 1;
        int lp = method + 1;
        int rp = find_match(lp, AT_LP, AT_RP);
        int semi = find_semi(rp + 1);
        emit_send(target, method, lp, rp, cl, meth, indent);
        *pp = semi + 1;
        return;
    }
    /* Generic function-call statement: IDENT '(' args ')' ';'
     * Covers printf, wm_line, wm_render, sleep_ms, etc.  The C compiler
     * recognizes the callee by name and emits the matching builtin. */
    if (t->kind == AT_IDENT && g_tokens[p+1].kind == AT_LP)
    {
        int lp = p + 1;
        int rp = find_match(lp, AT_LP, AT_RP);
        int semi = find_semi(rp + 1);
        emit_indent(indent);
        emit(t->str);
        emit("(");
        emit_expr_until(lp + 1, rp, cl, meth);
        emit(");\n");
        *pp = semi + 1;
        return;
    }
    /* assignment: IDENT = expr ; */
    if (t->kind == AT_IDENT && g_tokens[p+1].kind == AT_ASSIGN)
    {
        int semi = find_semi(p + 2);
        emit_indent(indent);
        emit_ident_ref(t->str, cl, meth);
        emit(" = ");
        emit_expr_until(p + 2, semi, cl, meth);
        emit(";\n");
        *pp = semi + 1;
        return;
    }
    if (t->kind == AT_RETURN)
    {
        int semi = find_semi(p + 1);
        /* return is a no-op in method bodies (no return value) */
        *pp = semi + 1;
        return;
    }
    if (t->kind == AT_SEMI) { *pp = p + 1; return; }

    /* fallback: skip to next semicolon */
    {
        int semi = find_semi(p);
        emit_indent(indent); emit("/* skipped: ");
        emit_expr_until(p, semi, cl, meth);
        emit(" */\n");
        *pp = semi + 1;
    }
}

static void emit_block(int *pp, int end, struct abcl_class *cl,
                       struct abcl_method *meth, int indent)
{
    while (*pp < end && g_tokens[*pp].kind != AT_EOF)
    {
        emit_stmt(pp, cl, meth, indent);
        if (g_err) return;
    }
}

/* Emit the entire C program. */
static void emit_program(void)
{
    int i, j, k, mp;

    emit("/* Generated by abclc — DO NOT EDIT */\n");
    emit("#include <stdio.h>\n\n");
    emit("int main(void) {\n");

    /* envelope + temps */
    mp = max_method_params();
    if (mp < 1) mp = 1;
    emit("    int m_recv;\n");
    emit("    int m_sender;\n");
    emit("    int m_meth;\n");
    for (i = 0; i < mp; i++)
    { emit("    int m_arg"); emit_int(i); emit(";\n"); }
    emit("    int new_recv;\n");
    emit("    int new_sender;\n");
    emit("    int new_meth;\n");
    for (i = 0; i < mp; i++)
    { emit("    int new_arg"); emit_int(i); emit(";\n"); }
    emit("    int sent;\n\n");

    /* per-actor field storage */
    for (i = 0; i < g_nactors; i++)
    {
        struct abcl_class *cl = &g_classes[g_actors[i].class_id];
        for (j = 0; j < cl->nfields; j++)
        {
            emit("    int p"); emit_int(i); emit("_");
            emit(cl->name); emit("_"); emit(cl->fields[j]);
            emit(";\n");
        }
    }
    emit("\n");

    /* current-actor copies (per class, per field) */
    for (i = 0; i < g_nclasses; i++)
    {
        for (j = 0; j < g_classes[i].nfields; j++)
        {
            emit("    int cur_"); emit(g_classes[i].name); emit("_");
            emit(g_classes[i].fields[j]); emit(";\n");
        }
    }
    emit("\n");

    /* init fields to 0 */
    for (i = 0; i < g_nactors; i++)
    {
        struct abcl_class *cl = &g_classes[g_actors[i].class_id];
        for (j = 0; j < cl->nfields; j++)
        {
            emit("    p"); emit_int(i); emit("_");
            emit(cl->name); emit("_"); emit(cl->fields[j]);
            emit(" = 0;\n");
        }
    }
    emit("\n");

    /* Initial sends become a chain — only the LAST initial send wins (single
     * envelope).  PingPong uses one initial send, so this is fine. */
    if (g_ninitsends == 0)
    {
        emit("    m_recv = -1;\n");
    }
    else
    {
        struct abcl_initsend *s = &g_initsends[g_ninitsends - 1];
        int ai;
        int args_start = s->body_start;
        int args_end   = s->body_end;
        int target_idx = find_actor(s->target);
        int cls;
        int mid;

        if (target_idx < 0)
        {
            abcl_err("unknown actor in initial send"); return;
        }
        cls = g_actors[target_idx].class_id;
        mid = find_method(&g_classes[cls], s->method);
        if (mid < 0) { abcl_err("unknown method in initial send"); return; }

        emit("    m_recv = "); emit_int(target_idx); emit(";\n");
        emit("    m_sender = -1;\n");
        emit("    m_meth = "); emit_int(mid); emit(";\n");

        /* Walk args, emit m_arg<i> = <expr>; */
        {
            int p = args_start + 1;
            int last = p;
            ai = 0;
            while (p < args_end)
            {
                if (g_tokens[p].kind == AT_COMMA)
                {
                    int depth = 0, q;
                    for (q = args_start + 1; q < p; q++)
                    {
                        if (g_tokens[q].kind == AT_LP) depth++;
                        else if (g_tokens[q].kind == AT_RP) depth--;
                    }
                    if (depth == 0)
                    {
                        emit("    m_arg"); emit_int(ai++); emit(" = ");
                        emit_expr_until(last, p, NULL, NULL);
                        emit(";\n");
                        last = p + 1;
                    }
                }
                p++;
            }
            if (last < args_end)
            {
                emit("    m_arg"); emit_int(ai++); emit(" = ");
                emit_expr_until(last, args_end, NULL, NULL);
                emit(";\n");
            }
            for (; ai < mp; ai++)
            { emit("    m_arg"); emit_int(ai); emit(" = 0;\n"); }
        }
    }

    emit("\n    while (m_recv >= 0) {\n");
    emit("        sent = 0;\n");

    /* Load current actor's field copies. */
    for (i = 0; i < g_nactors; i++)
    {
        struct abcl_class *cl = &g_classes[g_actors[i].class_id];
        if (cl->nfields == 0) continue;
        emit("        if (m_recv == "); emit_int(i); emit(") {\n");
        for (j = 0; j < cl->nfields; j++)
        {
            emit("            cur_"); emit(cl->name); emit("_");
            emit(cl->fields[j]); emit(" = p"); emit_int(i); emit("_");
            emit(cl->name); emit("_"); emit(cl->fields[j]); emit(";\n");
        }
        emit("        }\n");
    }

    /* Dispatch on (class, method). */
    for (i = 0; i < g_nclasses; i++)
    {
        struct abcl_class *cl = &g_classes[i];
        for (j = 0; j < cl->nmethods; j++)
        {
            struct abcl_method *m = &cl->methods[j];
            int p, end;

            emit("        if (m_meth == "); emit_int(j); emit(") {\n");

            /* Bind params: nothing to do since refs map directly to m_argN */

            p = m->body_start + 1;        /* skip '{' */
            end = m->body_end;             /* index of '}' */
            emit_block(&p, end, cl, m, 3);
            if (g_err) return;

            emit("        }\n");
        }
    }

    /* Save current actor's field copies back. */
    for (i = 0; i < g_nactors; i++)
    {
        struct abcl_class *cl = &g_classes[g_actors[i].class_id];
        if (cl->nfields == 0) continue;
        emit("        if (m_recv == "); emit_int(i); emit(") {\n");
        for (j = 0; j < cl->nfields; j++)
        {
            emit("            p"); emit_int(i); emit("_");
            emit(cl->name); emit("_"); emit(cl->fields[j]);
            emit(" = cur_"); emit(cl->name); emit("_");
            emit(cl->fields[j]); emit(";\n");
        }
        emit("        }\n");
    }

    /* Commit next message or terminate. */
    emit("        if (sent == 1) {\n");
    emit("            m_recv = new_recv;\n");
    emit("            m_sender = new_sender;\n");
    emit("            m_meth = new_meth;\n");
    for (i = 0; i < mp; i++)
    { emit("            m_arg"); emit_int(i); emit(" = new_arg"); emit_int(i); emit(";\n"); }
    emit("        } else {\n");
    emit("            m_recv = -1;\n");
    emit("        }\n");
    emit("    }\n\n");

    /* Final summary */
    emit("    printf(\"[abcl] done\\n\");\n");
    for (i = 0; i < g_nactors; i++)
    {
        struct abcl_class *cl = &g_classes[g_actors[i].class_id];
        for (j = 0; j < cl->nfields; j++)
        {
            emit("    printf(\"  ");
            emit(g_actors[i].name);
            emit(".");
            emit(cl->fields[j]);
            emit(" = %d\\n\", p");
            emit_int(i); emit("_"); emit(cl->name); emit("_");
            emit(cl->fields[j]);
            emit(");\n");
        }
        (void)k;
    }
    emit("    return 0;\n");
    emit("}\n");
}

/* ---------- top-level entry ---------- */

int abclcTranslate(const char *src_path, const char *out_path)
{
    char *src_buf = NULL;
    uint  src_len = 0;
    int   rc = SYSERR;
    int   fd, n;
    struct xinode in;

    /* Init globals */
    g_nclasses = 0;
    g_nactors = 0;
    g_ninitsends = 0;
    g_err = 0;
    g_outfd = -1;

    if (OK != xfsStat(src_path, &in, NULL))
    { printf("abclc: cannot stat %s\n", src_path); return SYSERR; }

    src_buf = (char *)memget(in.size + 1);
    if ((void *)SYSERR == src_buf) { printf("abclc: out of memory\n"); return SYSERR; }
    fd = xfsOpen(src_path, XFS_O_RDONLY);
    if (fd < 0) { printf("abclc: cannot open %s\n", src_path); goto out; }
    n = xfsRead(fd, src_buf, in.size);
    xfsClose(fd);
    if (n < 0) goto out;
    src_buf[n] = 0;

    g_tokens = (struct abcl_token *)
                  memget(ABCL_MAX_TOKENS * sizeof(struct abcl_token));
    if ((void *)SYSERR == g_tokens) { printf("abclc: oom tokens\n"); goto out; }

    g_src = src_buf;
    if (tokenize() < 0) goto out;
    if (parse_program() < 0) goto out;
    if (g_nclasses == 0) { printf("abclc: no class defined\n"); goto out; }

    g_outfd = xfsOpen(out_path,
                      XFS_O_RDWR | XFS_O_CREAT | XFS_O_TRUNC);
    if (g_outfd < 0)
    { printf("abclc: cannot create %s\n", out_path); goto out; }

    emit_program();
    if (g_err) goto out;

    rc = OK;

out:
    if (g_outfd >= 0) xfsClose(g_outfd);
    if (g_tokens) memfree(g_tokens, ABCL_MAX_TOKENS * sizeof(struct abcl_token));
    if (src_buf)  memfree(src_buf, in.size + 1);
    src_len = (uint)src_len;
    (void)src_len;
    return rc;
}
