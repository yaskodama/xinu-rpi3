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
void basic_set_emit(void (*fn)(const char *))        { g_emit = fn; }
void basic_set_input(int (*fn)(char *, int))         { g_input = fn; }
void basic_set_cls(void (*fn)(void))                 { g_cls = fn; }
void basic_set_plot(void (*fn)(int, int, int))       { g_plot = fn; }
void basic_set_pause(void (*fn)(int))                { g_pause = fn; }
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
    while (*ip == '*' || *ip == '/') { char op = *ip++; double r = power();
        if (op == '*') v *= r; else { if (r == 0) berr("div0"); else v /= r; } skipsp(); }
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
/* rotate.bas — clear the screen (CLS) and spin a line segment about the
 * centre using CLS + PLOT + SIN/COS + PAUSE. */
static const char *S_rotate[] = {
    "10 REM ROTATING LINE SEGMENT",
    "15 FOR N=1 TO 20",
    "20 FOR A=0 TO 170 STEP 10",
    "30 CLS",
    "40 R=A*3.14159/180",
    "50 FOR T=-16 TO 16",
    "60 X=28+T*COS(R)",
    "70 Y=16+T*SIN(R)*0.55",
    "80 PLOT X,Y,42",
    "90 NEXT",
    "100 PLOT 28,16,43",
    "110 PAUSE 90",
    "120 NEXT",
    "125 NEXT",
    "130 END"
};

static const struct { const char *name; const char *const *line; int n; } samples[] = {
    { "hello.bas",   S_hello,   2 },
    { "forloop.bas", S_forloop, 3 },
    { "mtable.bas",  S_mtable,  4 },
    { "add.bas",     S_add,     4 },
    { "sum100.bas",  S_sum100,  5 },
    { "fibon.bas",   S_fibon,   8 },
    { "squares.bas", S_squares, 4 },
    { "rotate.bas",  S_rotate,  15 },
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
        if (*ip == '"') { ip++; while (*ip && *ip != '"') emitc(*ip++); if (*ip == '"') ip++; }
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
    if (kw("NEW"))  { nprog = 0; for (int i = 0; i < 26 * 11; i++) vars[i] = 0; emit("Ok\n"); return; }
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
    if (kw("PAUSE")) { int ms = (int)expr(); if (g_pause) g_pause(ms); return; }
    (void)kw("LET");
    { const char *save = ip; int vi = varidx(); skipsp();
      if (vi >= 0 && *ip == '=') { ip++; vars[vi] = expr(); return; }
      ip = save; }
    berr("syntax error");
}

/* ---- RUN ----------------------------------------------------------- */
static void do_run(void)
{
    running = 1; fortop = 0; gosubtop = 0; err = 0;
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

void basic_init(void) { nprog = 0; for (int i = 0; i < 26 * 11; i++) vars[i] = 0; running = 0; }

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
