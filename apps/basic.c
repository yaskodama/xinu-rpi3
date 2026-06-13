// apps/basic.c — a small classic line-numbered BASIC interpreter.
//
// Modelled on the line-numbered BASIC of lecture.site44.com/basic-emu.  Runs
// as its own window in the Pi 3 gwm desktop (apps/gwm.c basic_win), but the
// interpreter core here is freestanding: ALL output goes through one callback
// and INPUT pulls a line through another, so it builds + unit-tests on a host
// with -DBASIC_HOST_TEST (gcc apps/basic.c -DBASIC_HOST_TEST -lm).
//
// Direct commands:  RUN  RUN "name"  LIST  NEW  FILES  LOAD "name"
// Statements:  PRINT (";"/","/trailing ";"/"lit"/expr)   [LET] v=expr
//              INPUT ["prompt";] v[,v...]
//              IF expr THEN (stmt | lineno)
//              GOTO n   GOSUB n   RETURN   FOR v=a TO b [STEP s]   NEXT [v]
//              END   STOP   REM ...
// Expr: + - * / ^, parens, unary -, relops = <> < > <= >=, AND OR,
//       vars A..Z and A0..Z9 (numeric), funcs ABS INT SGN SQR RND.
// Numbers are double; PRINT shows integral values without a fraction.

#ifdef BASIC_HOST_TEST
#  include <stdio.h>
#  include <string.h>
#endif
/* Newton's method sqrt — no libm dependency on the bare-metal target. */
static double b_sqrt(double x)
{
    if (x <= 0) return 0;
    double g = x > 1 ? x : 1;
    for (int i = 0; i < 50; i++) g = 0.5 * (g + x / g);
    return g;
}

/* SIN/COS via range reduction + Taylor series — no libm dependency.  Good
 * enough for graphics demos (rotate.bas line rotation). */
static double b_sin(double x)
{
    const double PI = 3.14159265358979, TWO_PI = 6.28318530717959;
    while (x >  PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;
    double term = x, sum = x, x2 = x * x;
    for (int n = 1; n <= 9; n++) {
        term *= -x2 / (double)((2 * n) * (2 * n + 1));
        sum  += term;
    }
    return sum;
}
static double b_cos(double x) { return b_sin(x + 1.57079632679490); }

/* ---- I/O callbacks ------------------------------------------------- */
static void (*g_emit)(const char *);
static int  (*g_input)(char *buf, int max);  /* read one line for INPUT */
static void (*g_cls)(void);                  /* CLS: clear the screen */
static void (*g_plot)(int x, int y, int ch); /* PLOT x,y[,c]: char at cell */
static void (*g_pause)(int ms);              /* PAUSE n: sleep n ms */
static void (*g_line)(int x1, int y1, int x2, int y2, int color); /* LINE seg */
void basic_set_emit(void (*fn)(const char *))        { g_emit = fn; }
void basic_set_input(int (*fn)(char *, int))         { g_input = fn; }
void basic_set_cls(void (*fn)(void))                 { g_cls = fn; }
void basic_set_plot(void (*fn)(int, int, int))       { g_plot = fn; }
void basic_set_pause(void (*fn)(int))                { g_pause = fn; }
void basic_set_line(void (*fn)(int, int, int, int, int)) { g_line = fn; }
static void emit(const char *s) { if (g_emit) g_emit(s); }
static void emitc(char c) { char b[2]; b[0] = c; b[1] = 0; emit(b); }

/* ---- helpers ------------------------------------------------------- */
static int  b_isdigit(char c) { return c >= '0' && c <= '9'; }
static int  b_isalpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static char b_up(char c)      { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* double -> string: integral values print with no fraction. */
static void num_str(double v, char *out)
{
    int p = 0;
    if (v < 0) { out[p++] = '-'; v = -v; }
    long ip_ = (long)v;
    double fr = (v - (double)ip_) * 1000000.0 + 0.5;
    long frac = (long)fr;
    if (frac >= 1000000L) { frac -= 1000000L; ip_++; }
    char tmp[24]; int n = 0; long q = ip_;
    if (q == 0) tmp[n++] = '0';
    while (q > 0) { tmp[n++] = (char)('0' + q % 10); q /= 10; }
    while (n > 0) out[p++] = tmp[--n];
    if (frac != 0) {
        char fb[6]; for (int i = 5; i >= 0; i--) { fb[i] = (char)('0' + frac % 10); frac /= 10; }
        int last = 5; while (last >= 0 && fb[last] == '0') last--;
        if (last >= 0) { out[p++] = '.'; for (int i = 0; i <= last; i++) out[p++] = fb[i]; }
    }
    out[p] = 0;
}

/* ---- program + variables ------------------------------------------ */
#define MAXPROG  240
#define PLINELEN 96
static struct { int no; char text[PLINELEN]; } prog[MAXPROG];
static int    nprog;
static double vars[26 * 11];          /* A..Z (col 0), A0..Z9 (cols 1..10) */

/* String variables A$..Z$ and A0$..Z9$ — same 26*11 layout as vars[]. */
#define SVAR_LEN 64
static char svars[26 * 11][SVAR_LEN];

/* Numeric arrays A(..)..Z(..), 1-D or 2-D, carved from a shared pool by
 * DIM (or auto-dimensioned to (10) on first subscript, classic-BASIC
 * style).  arrtab[L].ndim==0 means letter L has no array yet. */
#define ARR_POOL 2048
static double arrpool[ARR_POOL];
static int    arrtop;
static struct { int base, d1, d2, ndim; } arrtab[26];

/* DATA/READ/RESTORE cursor: data_pc = prog[] index of the DATA line being
 * read (-1 before the first READ / after RESTORE); data_ip walks its
 * items, NULL forces a re-scan from data_pc+1. */
static int         data_pc = -1;
static const char *data_ip;

/* ---- runtime state ------------------------------------------------- */
static int         running;
static int         pc;                /* current prog[] index while running */
static const char *ip;                /* statement cursor                   */
static int         err;
static char        errmsg[64];
static int         g_goto;            /* -1 none, -2 pc/ip already set, >=0 target line */

#define FOR_MAX 16
static struct { int var; double limit, step; int idx; const char *stmt; } forstk[FOR_MAX];
static int fortop;
#define GOSUB_MAX 24
static struct { int pc; const char *ip; } gosubstk[GOSUB_MAX];   /* return addr */
static int gosubtop;
#define WHILE_MAX 16
static struct { int pc; const char *ip; } whilestk[WHILE_MAX];   /* the WHILE's condition */
static int whiletop;

static void berr(const char *m)
{ if (err) return; err = 1; int i = 0; for (; m[i] && i < (int)sizeof errmsg - 1; i++) errmsg[i] = m[i]; errmsg[i] = 0; }

/* ---- lexer over ip ------------------------------------------------- */
static void skipsp(void) { while (*ip == ' ' || *ip == '\t') ip++; }
static int kw(const char *k)
{
    skipsp(); const char *p = ip; int i = 0;
    while (k[i]) { if (b_up(*p) != k[i]) return 0; p++; i++; }
    if (b_isalpha(*p) || b_isdigit(*p)) return 0;
    ip = p; return 1;
}
static int varidx(void)
{
    skipsp(); if (!b_isalpha(*ip)) return -1;
    int base = b_up(*ip) - 'A', col = 0; ip++;
    if (b_isdigit(*ip)) { col = (*ip - '0') + 1; ip++; }
    return base * 11 + col;
}

static double expr(void);
static void   seval(char *out, int max);   /* evaluate a string expression  */
static int    peek_is_string(void);        /* does a string factor come next? */
static double *arr_elem(int li);           /* &A(i[,j]); ip at '(' on entry  */
static void   paren_sexpr(char *out, int max);
static double factor(void)
{
    skipsp();
    if (*ip == '(') { ip++; double v = expr(); skipsp(); if (*ip == ')') ip++; else berr("expected )"); return v; }
    if (*ip == '-') { ip++; return -factor(); }
    if (*ip == '+') { ip++; return factor(); }
    if (b_isdigit(*ip) || *ip == '.') {
        double v = 0; while (b_isdigit(*ip)) v = v * 10 + (*ip++ - '0');
        if (*ip == '.') { ip++; double f = 0.1; while (b_isdigit(*ip)) { v += (*ip++ - '0') * f; f *= 0.1; } }
        return v;
    }
    if (kw("ABS")) { double v = factor(); return v < 0 ? -v : v; }
    if (kw("INT")) { double v = factor(); long l = (long)v; if (v < 0 && (double)l != v) l--; return (double)l; }
    if (kw("SGN")) { double v = factor(); return v > 0 ? 1 : (v < 0 ? -1 : 0); }
    if (kw("SQR")) { double v = factor(); return v > 0 ? b_sqrt(v) : 0; }
    if (kw("SIN")) { return b_sin(factor()); }
    if (kw("COS")) { return b_cos(factor()); }
    if (kw("RND")) { (void)factor();
        static unsigned long s = 22695477UL;
        s = s * 1103515245UL + 12345UL;
        return (double)((s >> 16) & 0x7FFF) / 32768.0; }
    if (kw("LEN")) { char s[SVAR_LEN]; paren_sexpr(s, sizeof s);
        int n = 0; while (s[n]) n++; return (double)n; }
    if (kw("ASC")) { char s[SVAR_LEN]; paren_sexpr(s, sizeof s);
        return (double)(unsigned char)s[0]; }
    if (kw("VAL")) { char s[SVAR_LEN]; paren_sexpr(s, sizeof s);
        const char *q = s; while (*q == ' ') q++;
        int neg = 0; if (*q == '-') { neg = 1; q++; } else if (*q == '+') q++;
        double v = 0, f; while (b_isdigit(*q)) v = v * 10 + (*q++ - '0');
        if (*q == '.') { q++; f = 0.1; while (b_isdigit(*q)) { v += (*q++ - '0') * f; f *= 0.1; } }
        return neg ? -v : v; }
    /* A(i[,j]) array element — a single letter immediately followed by '('
     * (multi-letter names were matched as functions above). */
    { const char *save = ip; skipsp();
      if (b_isalpha(*ip) && ip[1] == '(') { int li = b_up(*ip) - 'A'; ip++;
          double *e = arr_elem(li); return e ? *e : 0; }
      ip = save; }
    { const char *save = ip; int vi = varidx(); if (vi >= 0) return vars[vi]; ip = save; berr("syntax"); return 0; }
}
static double power(void)
{
    double v = factor(); skipsp();
    while (*ip == '^') { ip++; int n = (int)factor(); double r = 1; for (int i = 0; i < n; i++) r *= v; v = r; skipsp(); }
    return v;
}
static double term(void)
{
    double v = power(); skipsp();
    for (;;) {
        if (*ip == '*' || *ip == '/') { char op = *ip++; double r = power();
            if (op == '*') v *= r; else { if (r == 0) berr("div0"); else v /= r; } }
        else if (kw("MOD")) { double r = power();
            long a = (long)v, b = (long)r; if (b == 0) { berr("div0"); }
            else v = (double)(a - (a / b) * b); }
        else break;
        skipsp();
    }
    return v;
}
static double addsub(void)
{
    double v = term(); skipsp();
    while (*ip == '+' || *ip == '-') { char op = *ip++; double r = term(); v = (op == '+') ? v + r : v - r; skipsp(); }
    return v;
}
static double relexpr(void)
{
    if (peek_is_string()) {                      /* string relational compare */
        char l[SVAR_LEN]; seval(l, sizeof l); skipsp();
        char a = *ip, b = ip[1]; int op = 0;
        if (a == '<' && b == '=') { op = 4; ip += 2; }
        else if (a == '>' && b == '=') { op = 5; ip += 2; }
        else if (a == '<' && b == '>') { op = 6; ip += 2; }
        else if (a == '<') { op = 1; ip++; }
        else if (a == '>') { op = 2; ip++; }
        else if (a == '=') { op = 3; ip++; }
        if (!op) { berr("string relop"); return 0; }
        char r[SVAR_LEN]; seval(r, sizeof r);
        const char *x = l, *y = r; while (*x && *x == *y) { x++; y++; }
        int c = (int)(unsigned char)*x - (int)(unsigned char)*y;
        switch (op) { case 1: return c < 0; case 2: return c > 0; case 3: return c == 0;
                      case 4: return c <= 0; case 5: return c >= 0; default: return c != 0; }
    }
    double v = addsub(); skipsp();
    char a = *ip, b = ip[1]; int op = 0;
    if (a == '<' && b == '=') { op = 4; ip += 2; }
    else if (a == '>' && b == '=') { op = 5; ip += 2; }
    else if (a == '<' && b == '>') { op = 6; ip += 2; }
    else if (a == '<') { op = 1; ip++; }
    else if (a == '>') { op = 2; ip++; }
    else if (a == '=') { op = 3; ip++; }
    if (op) { double r = addsub();
        switch (op) { case 1: return v < r; case 2: return v > r; case 3: return v == r;
                      case 4: return v <= r; case 5: return v >= r; default: return v != r; } }
    return v;
}
static double expr(void)
{
    double v = relexpr(); skipsp();
    for (;;) { if (kw("AND")) { double r = relexpr(); v = (v != 0 && r != 0); }
               else if (kw("OR")) { double r = relexpr(); v = (v != 0 || r != 0); }
               else break; skipsp(); }
    return v;
}

/* ---- string variables, string expressions ------------------------- */

/* Parse a string-variable name (letter, optional digit, '$') at ip.
 * Returns the 0..285 slot index and consumes it, or -1 (ip unchanged). */
static int svaridx(void)
{
    skipsp(); const char *p = ip;
    if (!b_isalpha(*p)) return -1;
    int base = b_up(*p) - 'A', col = 0; p++;
    if (b_isdigit(*p)) { col = (*p - '0') + 1; p++; }
    if (*p != '$') return -1;
    ip = p + 1;
    return base * 11 + col;
}

/* True if the next factor is a string (literal, A$/A1$ var, or a $-suffixed
 * function like MID$): a run of letters [+ one digit] ending in '$'. */
static int peek_is_string(void)
{
    skipsp();
    if (*ip == '"') return 1;
    const char *p = ip;
    if (!b_isalpha(*p)) return 0;
    while (b_isalpha(*p)) p++;
    if (b_isdigit(*p)) p++;
    return *p == '$';
}

static void scopy(char *out, int max, const char *src)
{
    int n = 0; while (src[n] && n < max - 1) { out[n] = src[n]; n++; } out[n] = 0;
}

/* string '(' numeric-or-string ')' helper used by LEN/ASC/VAL/CHR$/STR$ */
static void paren_sexpr(char *out, int max)
{
    skipsp(); int par = 0; if (*ip == '(') { ip++; par = 1; }
    seval(out, max);
    skipsp(); if (par) { if (*ip == ')') ip++; else berr("expected )"); }
}
static double paren_num(void)
{
    skipsp(); int par = 0; if (*ip == '(') { ip++; par = 1; }
    double v = expr();
    skipsp(); if (par) { if (*ip == ')') ip++; else berr("expected )"); }
    return v;
}

static void sfactor(char *out, int max)
{
    skipsp(); out[0] = 0;
    if (*ip == '"') { int n = 0; ip++; while (*ip && *ip != '"' && n < max - 1) out[n++] = *ip++;
                      out[n] = 0; if (*ip == '"') ip++; return; }
    if (*ip == '(') { ip++; seval(out, max); skipsp(); if (*ip == ')') ip++; else berr("expected )"); return; }
    if (kw("MID$")) {                                   /* MID$(s, start [, len]) */
        char s[SVAR_LEN]; skipsp(); if (*ip == '(') ip++; else berr("MID$ (");
        seval(s, sizeof s); skipsp(); if (*ip == ',') ip++; else berr("MID$ ,");
        int start = (int)expr(); int len = max; skipsp();
        if (*ip == ',') { ip++; len = (int)expr(); skipsp(); }
        if (*ip == ')') ip++; else berr("MID$ )");
        int slen = 0; while (s[slen]) slen++;
        int i = start - 1; if (i < 0) i = 0; int n = 0;
        while (i < slen && n < len && n < max - 1) out[n++] = s[i++]; out[n] = 0; return;
    }
    if (kw("LEFT$")) {                                  /* LEFT$(s, n) */
        char s[SVAR_LEN]; skipsp(); if (*ip == '(') ip++; else berr("LEFT$ (");
        seval(s, sizeof s); skipsp(); if (*ip == ',') ip++; else berr("LEFT$ ,");
        int len = (int)expr(); skipsp(); if (*ip == ')') ip++; else berr("LEFT$ )");
        int n = 0; while (s[n] && n < len && n < max - 1) { out[n] = s[n]; n++; } out[n] = 0; return;
    }
    if (kw("RIGHT$")) {                                 /* RIGHT$(s, n) */
        char s[SVAR_LEN]; skipsp(); if (*ip == '(') ip++; else berr("RIGHT$ (");
        seval(s, sizeof s); skipsp(); if (*ip == ',') ip++; else berr("RIGHT$ ,");
        int len = (int)expr(); skipsp(); if (*ip == ')') ip++; else berr("RIGHT$ )");
        int slen = 0; while (s[slen]) slen++;
        int i = slen - len; if (i < 0) i = 0; int n = 0;
        while (s[i] && n < max - 1) out[n++] = s[i++]; out[n] = 0; return;
    }
    if (kw("CHR$")) { int c = (int)paren_num(); out[0] = (char)c; out[1] = 0; return; }
    if (kw("STR$")) { double v = paren_num(); num_str(v, out); return; }
    { const char *save = ip; int si = svaridx(); if (si >= 0) { scopy(out, max, svars[si]); return; } ip = save; }
    berr("string expr"); out[0] = 0;
}

static void seval(char *out, int max)                   /* '+' concatenation */
{
    sfactor(out, max); skipsp();
    while (*ip == '+') { ip++; char rhs[SVAR_LEN]; sfactor(rhs, sizeof rhs);
        int n = 0; while (out[n]) n++;
        for (int i = 0; rhs[i] && n < max - 1; i++) out[n++] = rhs[i]; out[n] = 0; skipsp(); }
}

/* ---- arrays -------------------------------------------------------- */

/* Return &A(i[,j]).  ip is positioned at '(' on entry.  Auto-dimensions an
 * undeclared letter to (10).  Sets err + returns NULL on overflow / bad
 * subscript. */
static double *arr_elem(int li)
{
    if (arrtab[li].ndim == 0) {                         /* auto-DIM to (10) */
        if (arrtop + 11 > ARR_POOL) { berr("array space"); return 0; }
        arrtab[li].base = arrtop; arrtab[li].d1 = 11; arrtab[li].d2 = 1; arrtab[li].ndim = 1;
        for (int k = 0; k < 11; k++) arrpool[arrtop + k] = 0; arrtop += 11;
    }
    if (*ip == '(') ip++; else { berr("array ("); return 0; }
    int i1 = (int)expr(), i2 = 0; skipsp();
    if (*ip == ',') { ip++; i2 = (int)expr(); skipsp(); }
    if (*ip == ')') ip++; else { berr("array )"); return 0; }
    if (i1 < 0 || i1 >= arrtab[li].d1 || i2 < 0 || i2 >= arrtab[li].d2) { berr("subscript"); return 0; }
    return &arrpool[arrtab[li].base + i1 * arrtab[li].d2 + i2];
}

/* ---- DATA / READ --------------------------------------------------- */

/* Does text t begin with keyword k (case-insensitive), not glued to more
 * letters/digits?  Used to scan ahead for WHILE/WEND nesting. */
static int b_streqi_kw(const char *t, const char *k)
{
    int i = 0; while (k[i]) { if (b_up(t[i]) != k[i]) return 0; i++; }
    return !(b_isalpha(t[i]) || b_isdigit(t[i]));
}

/* Is prog[i].text a DATA statement?  (leading spaces then DATA keyword) */
static int line_is_data(const char *t)
{
    while (*t == ' ' || *t == '\t') t++;
    const char *k = "DATA"; int i = 0;
    while (k[i]) { if (b_up(*t) != k[i]) return 0; t++; i++; }
    return !(b_isalpha(*t) || b_isdigit(*t));
}

/* Fetch the next DATA item into out[].  Returns 0 when DATA is exhausted. */
static int data_next(char *out, int max)
{
    for (;;) {
        if (data_ip == 0 || *data_ip == 0) {
            int start = (data_pc < 0) ? 0 : data_pc + 1, found = -1;
            for (int i = start; i < nprog; i++) if (line_is_data(prog[i].text)) { found = i; break; }
            if (found < 0) return 0;
            data_pc = found; const char *t = prog[found].text;
            while (*t == ' ' || *t == '\t') t++; t += 4;     /* skip "DATA" */
            data_ip = t;
        }
        while (*data_ip == ' ' || *data_ip == '\t' || *data_ip == ',') data_ip++;
        if (*data_ip == 0) continue;
        int n = 0;
        if (*data_ip == '"') { data_ip++; while (*data_ip && *data_ip != '"' && n < max - 1) out[n++] = *data_ip++;
                               if (*data_ip == '"') data_ip++; }
        else { while (*data_ip && *data_ip != ',' && n < max - 1) out[n++] = *data_ip++;
               while (n > 0 && (out[n-1] == ' ' || out[n-1] == '\t')) n--; }
        out[n] = 0; return 1;
    }
}
static double str_to_num(const char *q)
{
    while (*q == ' ') q++; int neg = 0;
    if (*q == '-') { neg = 1; q++; } else if (*q == '+') q++;
    double v = 0, f; while (b_isdigit(*q)) v = v * 10 + (*q++ - '0');
    if (*q == '.') { q++; f = 0.1; while (b_isdigit(*q)) { v += (*q++ - '0') * f; f *= 0.1; } }
    return neg ? -v : v;
}

/* ---- program edit -------------------------------------------------- */
static int find_line(int no) { for (int i = 0; i < nprog; i++) if (prog[i].no == no) return i; return -1; }
static void prog_set(int no, const char *text)
{
    int empty = 1; for (const char *p = text; *p; p++) if (*p != ' ' && *p != '\t') { empty = 0; break; }
    int i = find_line(no);
    if (i >= 0) {
        if (empty) { for (int j = i; j < nprog - 1; j++) prog[j] = prog[j + 1]; nprog--; return; }
        int k = 0; for (; text[k] && k < PLINELEN - 1; k++) prog[i].text[k] = text[k]; prog[i].text[k] = 0; return;
    }
    if (empty) return;
    if (nprog >= MAXPROG) { emit("?too many lines\n"); return; }
    int pos = nprog; for (int j = 0; j < nprog; j++) if (prog[j].no > no) { pos = j; break; }
    for (int j = nprog; j > pos; j--) prog[j] = prog[j - 1];
    prog[pos].no = no;
    int k = 0; for (; text[k] && k < PLINELEN - 1; k++) prog[pos].text[k] = text[k]; prog[pos].text[k] = 0;
    nprog++;
}
static void do_list(void)
{
    char nb[16];
    for (int i = 0; i < nprog; i++) { num_str((double)prog[i].no, nb); emit(nb); emit(" "); emit(prog[i].text); emit("\n"); }
}

/* ---- pre-loaded sample programs (read-only) ----------------------- *
 * Text-only samples from lecture.site44.com/basic-emu.  The graphics
 * samples there (line/circle/star/...) are omitted because this BASIC
 * has no graphics output — only the computational/PRINT ones are kept. */
static const char *S_hello[]   = { "10 PRINT \"HELLO, WORLD!\"", "20 PRINT \"WELCOME TO BASIC.\"" };
static const char *S_forloop[] = { "10 FOR I=1 TO 10", "20 PRINT I", "30 NEXT" };
static const char *S_mtable[]  = { "10 FOR I=1 TO 9", "20 K=7*I", "30 PRINT K", "40 NEXT" };
static const char *S_add[]     = { "10 A=10", "20 B=20", "30 C=A+B", "40 PRINT C" };
static const char *S_sum100[]  = { "10 S=0", "20 FOR I=1 TO 100", "30 S=S+I", "40 NEXT", "50 PRINT S" };
static const char *S_fibon[]   = { "10 A=0", "20 B=1", "30 FOR I=1 TO 12", "40 PRINT A", "50 C=A+B", "60 A=B", "70 B=C", "80 NEXT" };
static const char *S_squares[] = { "10 FOR I=1 TO 10", "20 K=I*I", "30 PRINT K", "40 NEXT" };
/* rotate.bas — spin a line segment about the centre of the graphics screen,
 * ten times, drawing it with the LINE statement (CLS + LINE + SIN/COS). */
static const char *S_rotate[] = {
    "10 REM ROTATING LINE SEGMENT",
    "20 FOR N=1 TO 10",
    "30 FOR A=0 TO 170 STEP 10",
    "40 CLS",
    "50 R=A*3.14159/180",
    "60 X1=200-130*COS(R) : Y1=150-130*SIN(R)",
    "70 X2=200+130*COS(R) : Y2=150+130*SIN(R)",
    "80 LINE(X1,Y1)-(X2,Y2),5",
    "90 PAUSE 80",
    "100 NEXT",
    "110 NEXT",
    "120 END"
};

/* New-feature demos: strings, arrays (DIM), WHILE/WEND + MOD, DATA/READ. */
static const char *S_strings[] = {
    "10 A$=\"HELLO\"", "20 B$=\"WORLD\"", "30 C$=A$+\", \"+B$+\"!\"",
    "40 PRINT C$", "50 PRINT \"LENGTH=\";LEN(C$)",
    "60 PRINT \"UPPER 5: \";LEFT$(C$,5)", "70 PRINT \"MID: \";MID$(C$,8,5)" };
static const char *S_bsort[] = {                 /* bubble sort an array */
    "10 DIM A(7)", "20 FOR I=0 TO 7", "30 READ A(I)", "40 NEXT",
    "50 DATA 5,2,9,1,7,3,8,4",
    "60 FOR I=0 TO 6", "70 FOR J=0 TO 6-I",
    "80 IF A(J)<=A(J+1) THEN GOTO 120",
    "90 T=A(J) : A(J)=A(J+1) : A(J+1)=T",
    "120 NEXT", "130 NEXT",
    "140 FOR I=0 TO 7 : PRINT A(I);\" \"; : NEXT : PRINT" };
static const char *S_fizz[] = {                  /* FizzBuzz: MOD + multi-stmt */
    "10 FOR N=1 TO 20",
    "20 IF N MOD 15=0 THEN PRINT \"FIZZBUZZ\" : GOTO 60",
    "30 IF N MOD 3=0 THEN PRINT \"FIZZ\" : GOTO 60",
    "40 IF N MOD 5=0 THEN PRINT \"BUZZ\" : GOTO 60",
    "50 PRINT N", "60 NEXT" };
static const char *S_table[] = {                 /* DATA/READ name=value table */
    "10 FOR I=1 TO 3", "20 READ N$,V", "30 PRINT N$;\" = \";V", "40 NEXT",
    "50 DATA \"APPLE\",10,\"BANANA\",20,\"CHERRY\",30" };
static const char *S_count[] = {                 /* WHILE/WEND countdown */
    "10 N=10", "20 WHILE N>0", "30 PRINT N;\" \";", "40 N=N-1", "50 WEND",
    "60 PRINT \"LIFTOFF!\"" };

static const struct { const char *name; const char *const *line; int n; } samples[] = {
    { "hello.bas",   S_hello,   2 },
    { "forloop.bas", S_forloop, 3 },
    { "mtable.bas",  S_mtable,  4 },
    { "add.bas",     S_add,     4 },
    { "sum100.bas",  S_sum100,  5 },
    { "fibon.bas",   S_fibon,   8 },
    { "squares.bas", S_squares, 4 },
    { "rotate.bas",  S_rotate,  12 },
    { "strings.bas", S_strings, 7 },
    { "bsort.bas",   S_bsort,   12 },
    { "fizz.bas",    S_fizz,    6 },
    { "table.bas",   S_table,   5 },
    { "count.bas",   S_count,   6 },
};
#define NSAMPLE ((int)(sizeof(samples) / sizeof(samples[0])))

/* case-insensitive string equality */
static int b_streqi(const char *a, const char *b)
{
    while (*a && *b) { if (b_up(*a) != b_up(*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

/* Normalise a raw filename into nm[] (lower-cased, ".bas" appended when no
 * extension was given) so RUN "HELLO" matches the stored "hello.bas". */
static void norm_name(const char *raw, char *nm, int max)
{
    int i = 0, dot = 0;
    while (raw[i] && i < max - 5) {
        char c = raw[i]; if (c >= 'A' && c <= 'Z') c += 32;
        if (c == '.') dot = 1;
        nm[i] = c; i++;
    }
    nm[i] = 0;
    if (!dot) { nm[i++] = '.'; nm[i++] = 'b'; nm[i++] = 'a'; nm[i++] = 's'; nm[i] = 0; }
}

/* FILES — list the saved (sample) programs, one per line. */
static void do_files(void)
{
    for (int s = 0; s < NSAMPLE; s++) { emit(samples[s].name); emit("\n"); }
}

/* Load a named sample into the program buffer; returns 1 on success. */
static int load_named(const char *raw)
{
    char nm[40]; norm_name(raw, nm, sizeof nm);
    for (int s = 0; s < NSAMPLE; s++) {
        if (b_streqi(samples[s].name, nm)) {
            nprog = 0;
            for (int k = 0; k < 26 * 11; k++) vars[k] = 0;
            for (int j = 0; j < samples[s].n; j++) {
                const char *p = samples[s].line[j];
                int no = 0; while (*p >= '0' && *p <= '9') no = no * 10 + (*p++ - '0');
                while (*p == ' ') p++;
                prog_set(no, p);
            }
            return 1;
        }
    }
    return 0;
}

/* Parse a quoted "name" starting at ip into buf; returns 1 if present. */
static int parse_quoted(char *buf, int max)
{
    skipsp();
    if (*ip != '"') return 0;
    ip++;
    int n = 0; while (*ip && *ip != '"' && n < max - 1) buf[n++] = *ip++;
    buf[n] = 0;
    if (*ip == '"') ip++;
    return 1;
}

/* ---- statements ---------------------------------------------------- */
static void do_print(void)
{
    int trailing = 0;
    for (;;) {
        skipsp();
        if (*ip == 0 || *ip == ':') break;
        if (peek_is_string()) { char s[SVAR_LEN]; seval(s, sizeof s); emit(s); }
        else { double v = expr(); char nb[40]; num_str(v, nb); emit(nb); }
        skipsp(); trailing = 0;
        if (*ip == ';') { ip++; trailing = 1; }
        else if (*ip == ',') { ip++; emit("\t"); trailing = 1; }
        else break;
    }
    if (!trailing) emit("\n");
}
static void do_input(void)
{
    skipsp();
    if (*ip == '"') { ip++; while (*ip && *ip != '"') emitc(*ip++); if (*ip == '"') ip++; skipsp(); if (*ip == ';' || *ip == ',') ip++; }
    emit("? ");
    char inbuf[96]; int n = g_input ? g_input(inbuf, sizeof inbuf) : -1;
    if (n < 0) { berr("no input"); return; }
    const char *src = inbuf;
    for (;;) {
        const char *save = ip; int si = svaridx();
        if (si >= 0) { while (*src == ' ' || *src == ',') src++;
            int n = 0; while (*src && *src != ',' && n < SVAR_LEN - 1) svars[si][n++] = *src++;
            while (n > 0 && svars[si][n-1] == ' ') n--; svars[si][n] = 0;
            skipsp(); if (*ip == ',') { ip++; continue; } break; }
        ip = save;
        int vi = varidx(); if (vi < 0) { berr("INPUT var"); return; }
        while (*src == ' ' || *src == ',') src++;
        int neg = 0; double v = 0, f;
        if (*src == '-') { neg = 1; src++; }
        while (b_isdigit(*src)) v = v * 10 + (*src++ - '0');
        if (*src == '.') { src++; f = 0.1; while (b_isdigit(*src)) { v += (*src++ - '0') * f; f *= 0.1; } }
        vars[vi] = neg ? -v : v;
        skipsp(); if (*ip == ',') { ip++; continue; } break;
    }
}

static void exec_stmt(void)
{
    skipsp();
    while (*ip == ':') { ip++; skipsp(); }   /* tolerate a resumed-at ':' */
    if (*ip == 0) return;
    if (kw("REM")) { while (*ip) ip++; return; }
    if (*ip == '?') { ip++; do_print(); return; }
    if (kw("PRINT")) { do_print(); return; }
    if (kw("LIST")) { do_list(); return; }
    if (kw("NEW"))  { nprog = 0; for (int i = 0; i < 26 * 11; i++) { vars[i] = 0; svars[i][0] = 0; }
                      arrtop = 0; for (int i = 0; i < 26; i++) arrtab[i].ndim = 0;
                      whiletop = 0; data_pc = -1; data_ip = 0; emit("Ok\n"); return; }
    if (kw("END") || kw("STOP")) { running = 0; return; }
    if (kw("GOTO"))  { g_goto = (int)expr(); return; }
    if (kw("GOSUB")) { int tgt = (int)expr();
        if (gosubtop < GOSUB_MAX) { gosubstk[gosubtop].pc = pc; gosubstk[gosubtop].ip = ip; gosubtop++; }
        else berr("GOSUB overflow");
        g_goto = tgt; return; }
    if (kw("RETURN")){ if (gosubtop > 0) { pc = gosubstk[--gosubtop].pc; ip = gosubstk[gosubtop].ip; g_goto = -2; }
                       else berr("RETURN without GOSUB"); return; }
    if (kw("INPUT")) { do_input(); return; }
    if (kw("IF")) {
        double c = expr(); skipsp();
        if (!kw("THEN")) { berr("expected THEN"); return; }
        skipsp();
        if (c != 0) { if (b_isdigit(*ip)) g_goto = (int)expr(); else exec_stmt(); }
        else while (*ip) ip++;
        return;
    }
    if (kw("FOR")) {
        int vi = varidx(); skipsp();
        if (*ip != '=') { berr("FOR ="); return; } ip++;
        double start = expr(); if (!kw("TO")) { berr("FOR TO"); return; }
        double limit = expr(); double step = 1; if (kw("STEP")) step = expr();
        vars[vi] = start;
        if (fortop < FOR_MAX) { forstk[fortop].var = vi; forstk[fortop].limit = limit; forstk[fortop].step = step;
                                forstk[fortop].idx = pc; forstk[fortop].stmt = ip; fortop++; }
        else berr("FOR overflow");
        return;
    }
    if (kw("NEXT")) {
        const char *save = ip; if (varidx() < 0) ip = save;     /* optional var */
        if (fortop == 0) { berr("NEXT without FOR"); return; }
        int t = fortop - 1;
        vars[forstk[t].var] += forstk[t].step;
        double v = vars[forstk[t].var];
        int again = (forstk[t].step >= 0) ? (v <= forstk[t].limit) : (v >= forstk[t].limit);
        if (again) { pc = forstk[t].idx; ip = forstk[t].stmt; g_goto = -2; } else fortop--;
        return;
    }
    if (kw("CLS")) {                          /* CLS [mode] — clear screen */
        skipsp(); if (b_isdigit(*ip) || *ip == '-') (void)expr();
        if (g_cls) g_cls();
        return;
    }
    if (kw("PLOT")) {                         /* PLOT x,y[,charcode] */
        int x = (int)expr(); skipsp();
        if (*ip != ',') { berr("PLOT ,"); return; } ip++;
        int y = (int)expr();
        int ch = '*'; skipsp();
        if (*ip == ',') { ip++; ch = (int)expr(); }
        if (g_plot) g_plot(x, y, ch);
        return;
    }
    if (kw("LINE")) {                         /* LINE(x1,y1)-(x2,y2)[,color] */
        int x1, y1, x2, y2, c = 7;
        skipsp(); if (*ip != '(') { berr("LINE ("); return; } ip++;
        x1 = (int)expr(); skipsp(); if (*ip != ',') { berr("LINE ,"); return; } ip++;
        y1 = (int)expr(); skipsp(); if (*ip != ')') { berr("LINE )"); return; } ip++;
        skipsp(); if (*ip != '-') { berr("LINE -"); return; } ip++;
        skipsp(); if (*ip != '(') { berr("LINE ("); return; } ip++;
        x2 = (int)expr(); skipsp(); if (*ip != ',') { berr("LINE ,"); return; } ip++;
        y2 = (int)expr(); skipsp(); if (*ip != ')') { berr("LINE )"); return; } ip++;
        skipsp(); if (*ip == ',') { ip++; c = (int)expr(); }
        if (g_line) g_line(x1, y1, x2, y2, c);
        return;
    }
    if (kw("PAUSE")) { int ms = (int)expr(); if (g_pause) g_pause(ms); return; }
    if (kw("DIM")) {
        for (;;) { skipsp(); if (!b_isalpha(*ip)) { berr("DIM var"); return; }
            int li = b_up(*ip) - 'A'; ip++; skipsp();
            if (*ip != '(') { berr("DIM ("); return; } ip++;
            int d1 = (int)expr() + 1, d2 = 1, nd = 1; skipsp();
            if (*ip == ',') { ip++; d2 = (int)expr() + 1; nd = 2; skipsp(); }
            if (*ip != ')') { berr("DIM )"); return; } ip++;
            int sz = d1 * d2;
            if (d1 < 1 || d2 < 1 || arrtop + sz > ARR_POOL) { berr("array space"); return; }
            arrtab[li].base = arrtop; arrtab[li].d1 = d1; arrtab[li].d2 = d2; arrtab[li].ndim = nd;
            for (int k = 0; k < sz; k++) arrpool[arrtop + k] = 0; arrtop += sz;
            skipsp(); if (*ip == ',') { ip++; continue; } break; }
        return;
    }
    if (kw("DATA")) { while (*ip) ip++; return; }        /* inert when reached */
    if (kw("RESTORE")) { data_pc = -1; data_ip = 0; skipsp();
        if (b_isdigit(*ip)) { int idx = find_line((int)expr()); if (idx >= 0) data_pc = idx - 1; }
        return;
    }
    if (kw("READ")) {
        for (;;) { char tok[SVAR_LEN]; skipsp(); const char *save = ip; int si = svaridx();
            if (si >= 0) { if (!data_next(tok, sizeof tok)) { berr("out of DATA"); return; } scopy(svars[si], SVAR_LEN, tok); }
            else if (b_isalpha(*ip) && ip[1] == '(') {       /* READ into A(i[,j]) */
                int li = b_up(*ip) - 'A'; ip++; double *e = arr_elem(li);
                if (!data_next(tok, sizeof tok)) { berr("out of DATA"); return; } if (e) *e = str_to_num(tok); }
            else { ip = save; int vi = varidx(); if (vi < 0) { berr("READ var"); return; }
                   if (!data_next(tok, sizeof tok)) { berr("out of DATA"); return; } vars[vi] = str_to_num(tok); }
            skipsp(); if (*ip == ',') { ip++; continue; } break; }
        return;
    }
    if (kw("WHILE")) {
        const char *cond_ip = ip; int cond_pc = pc;
        double c = expr();
        if (c != 0) {
            if (whiletop < WHILE_MAX) { whilestk[whiletop].pc = cond_pc; whilestk[whiletop].ip = cond_ip; whiletop++; }
            else berr("WHILE overflow");
        } else {                                          /* skip to matching WEND */
            int depth = 1;
            for (int i = pc + 1; i < nprog; i++) {
                const char *t = prog[i].text; while (*t == ' ' || *t == '\t') t++;
                if (b_streqi_kw(t, "WHILE")) depth++;
                else if (b_streqi_kw(t, "WEND")) { if (--depth == 0) { pc = i; ip = prog[i].text;
                    while (*ip) ip++; g_goto = -2; return; } }
            }
            berr("WHILE without WEND");
        }
        return;
    }
    if (kw("WEND")) {
        if (whiletop == 0) { berr("WEND without WHILE"); return; }
        int t = whiletop - 1; const char *after_ip = ip; int after_pc = pc;
        ip = whilestk[t].ip; pc = whilestk[t].pc;
        double c = expr();
        if (c != 0) { g_goto = -2; }                      /* loop: resume body after cond */
        else { whiletop--; ip = after_ip; pc = after_pc; }/* exit: continue after WEND */
        return;
    }
    (void)kw("LET");
    { const char *save = ip; int si = svaridx(); skipsp();           /* A$ = strexpr */
      if (si >= 0 && *ip == '=') { ip++; seval(svars[si], SVAR_LEN); return; }
      ip = save; }
    { const char *save = ip; skipsp();                               /* A(i[,j]) = expr */
      if (b_isalpha(*ip) && ip[1] == '(') { int li = b_up(*ip) - 'A'; ip++;
          double *e = arr_elem(li); skipsp();
          if (e && *ip == '=') { ip++; *e = expr(); return; } }
      ip = save; }
    { const char *save = ip; int vi = varidx(); skipsp();
      if (vi >= 0 && *ip == '=') { ip++; vars[vi] = expr(); return; }
      ip = save; }
    berr("syntax error");
}

/* ---- RUN ----------------------------------------------------------- */
static void do_run(void)
{
    running = 1; fortop = 0; gosubtop = 0; whiletop = 0; err = 0;
    data_pc = -1; data_ip = 0;                 /* rewind DATA          */
    arrtop = 0; for (int i = 0; i < 26; i++) arrtab[i].ndim = 0;   /* clear arrays (re-DIM) */
    if (nprog == 0) { running = 0; emit("Ok\n"); return; }
    pc = 0; ip = prog[0].text;
    long guard = 0;
    while (running) {
        if (pc < 0 || pc >= nprog) break;
        if (++guard > 8000000L) { emit("\n?runaway stopped\n"); break; }

        g_goto = -1;
        exec_stmt();                          /* runs at the current ip */

        if (err) {
            char nb[16]; emit("?"); emit(errmsg); emit(" in ");
            num_str((double)prog[pc].no, nb); emit(nb); emit("\n");
            break;
        }
        if (g_goto >= 0) {                    /* GOTO / GOSUB / IF->lineno */
            int idx = find_line(g_goto);
            if (idx < 0) { emit("?undef'd line "); { char nb[16]; num_str((double)g_goto, nb); emit(nb); } emit("\n"); break; }
            pc = idx; ip = prog[pc].text; continue;
        }
        if (g_goto == -2) continue;           /* RETURN / NEXT set pc+ip — resume there */

        /* g_goto == -1: statement finished normally */
        skipsp();
        if (*ip == ':') { ip++; continue; }   /* next statement on the same line */
        pc++;                                 /* advance to the next line */
        if (pc < nprog) ip = prog[pc].text;
    }
    running = 0;
    if (!err) emit("Ok\n");
}

/* ---- public: process one typed line ------------------------------- */
void basic_exec_line(const char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == 0) return;
    if (b_isdigit(*line)) {                   /* program line */
        int no = 0; while (b_isdigit(*line)) no = no * 10 + (*line++ - '0');
        while (*line == ' ') line++;
        prog_set(no, line);
        return;
    }
    err = 0; ip = line;
    if (kw("FILES")) { do_files(); emit("Ok\n"); return; }
    if (kw("LOAD")) {
        char fn[40];
        if (parse_quoted(fn, sizeof fn) && load_named(fn)) emit("Ok\n");
        else emit("?file not found\n");
        return;
    }
    if (kw("RUN")) {
        char fn[40]; const char *save = ip;
        if (parse_quoted(fn, sizeof fn)) {
            if (!load_named(fn)) { emit("?file not found\n"); return; }
        } else { ip = save; }
        do_run(); return;
    }
    /* immediate statement(s) */
    for (;;) {
        g_goto = -1; exec_stmt();
        if (err) { emit("?"); emit(errmsg); emit("\n"); return; }
        skipsp();
        if (*ip == ':') { ip++; continue; }
        break;
    }
    emit("Ok\n");
}

void basic_init(void) { nprog = 0; for (int i = 0; i < 26 * 11; i++) { vars[i] = 0; svars[i][0] = 0; }
    arrtop = 0; for (int i = 0; i < 26; i++) arrtab[i].ndim = 0;
    whiletop = 0; data_pc = -1; data_ip = 0; running = 0; }

#ifdef BASIC_HOST_TEST
static void host_emit(const char *s) { fputs(s, stdout); }
static int  host_input(char *buf, int max) {
    if (!fgets(buf, max, stdin)) return -1;
    int n = (int)strlen(buf); while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0; return n;
}
int main(void)
{
    basic_set_emit(host_emit); basic_set_input(host_input); basic_init();
    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        int n = (int)strlen(line); while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        basic_exec_line(line);
    }
    return 0;
}
#endif
