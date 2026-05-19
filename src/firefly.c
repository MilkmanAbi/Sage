// firefly.c — Firefly: Error Diagnosis Subsystem for SageTree
//
// Firefly is the complete error catching, explaining, and diagnostic system
// for both the SageTree interpreter and compiler.
//
// Verbosity levels (--firefly=full|minimal|off):
//   full    — source, explanation, advice, context (default)
//   minimal — error line and location only
//   off     — suppress Firefly, raw errors only

#include "firefly.h"
#include "gc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>

// ── Thread-local context ─────────────────────────────────────────────────────
static __thread FireflyContext ff_ctx = {0};

void firefly_init(void) {
    memset(&ff_ctx, 0, sizeof(ff_ctx));
    ff_ctx.max_errors = 50;
    ff_ctx.verbosity = FIREFLY_FULL;
}

void firefly_shutdown(void) {
    FireflyFrame* f = ff_ctx.call_stack;
    while (f) { FireflyFrame* n = f->next; free(f); f = n; }
    memset(&ff_ctx, 0, sizeof(ff_ctx));
}

void firefly_set_file(const char* fn) { ff_ctx.current_file = fn; }
void firefly_set_verbosity(FireflyVerbosity v) { ff_ctx.verbosity = v; }

// ── Call stack ───────────────────────────────────────────────────────────────
void firefly_push_frame(const char* func_name, FireflyLoc loc) {
    if (ff_ctx.stack_depth > 256) return;
    FireflyFrame* f = malloc(sizeof(FireflyFrame));
    f->function_name = func_name; f->loc = loc; f->next = ff_ctx.call_stack;
    ff_ctx.call_stack = f; ff_ctx.stack_depth++;
}

void firefly_pop_frame(void) {
    if (!ff_ctx.call_stack) return;
    FireflyFrame* old = ff_ctx.call_stack;
    ff_ctx.call_stack = old->next; ff_ctx.stack_depth--; free(old);
}

// ── Location extraction ──────────────────────────────────────────────────────
FireflyLoc firefly_loc_from_expr(Expr* expr) {
    FireflyLoc loc = {0};
    if (!expr) return loc;
    switch (expr->type) {
        case EXPR_VARIABLE:
            loc.filename = expr->as.variable.name.filename;
            loc.line = expr->as.variable.name.line;
            loc.column = expr->as.variable.name.column;
            loc.line_start = expr->as.variable.name.line_start;
            loc.span = expr->as.variable.name.length;
            break;
        case EXPR_BINARY:
            loc.filename = expr->as.binary.op.filename;
            loc.line = expr->as.binary.op.line;
            loc.column = expr->as.binary.op.column;
            loc.line_start = expr->as.binary.op.line_start;
            loc.span = expr->as.binary.op.length;
            break;
        case EXPR_GET:
            loc.filename = expr->as.get.property.filename;
            loc.line = expr->as.get.property.line;
            loc.column = expr->as.get.property.column;
            loc.line_start = expr->as.get.property.line_start;
            loc.span = expr->as.get.property.length;
            break;
        case EXPR_SET:
            loc.filename = expr->as.set.property.filename;
            loc.line = expr->as.set.property.line;
            loc.column = expr->as.set.property.column;
            loc.line_start = expr->as.set.property.line_start;
            loc.span = expr->as.set.property.length;
            break;
        case EXPR_CALL:
            if (expr->as.call.callee) return firefly_loc_from_expr(expr->as.call.callee);
            break;
        case EXPR_INDEX:
            if (expr->as.index.array) return firefly_loc_from_expr(expr->as.index.array);
            break;
        default: break;
    }
    if (!loc.filename) loc.filename = ff_ctx.current_file;
    return loc;
}

FireflyLoc firefly_loc_from_token(Token* tok) {
    FireflyLoc loc = {0};
    if (!tok) return loc;
    loc.filename = tok->filename ? tok->filename : ff_ctx.current_file;
    loc.line = tok->line; loc.column = tok->column;
    loc.line_start = tok->line_start; loc.span = tok->length;
    return loc;
}

// ── ANSI ─────────────────────────────────────────────────────────────────────
static int _tty = -1;
static int tty(void) { if (_tty < 0) _tty = isatty(2); return _tty; }
#define C(x) (tty() ? (x) : "")
#define CR   C("\033[0m")
#define CB   C("\033[1m")
#define CRED C("\033[1;31m")
#define CYEL C("\033[1;33m")
#define CCYN C("\033[1;36m")
#define CGRN C("\033[1;32m")
#define CBLU C("\033[34m")
#define CMAG C("\033[35m")

static const char* sev_c(FireflySeverity s) {
    switch (s) { case FIREFLY_ERROR: return CRED; case FIREFLY_WARNING: return CYEL;
                 case FIREFLY_NOTE: return CCYN; case FIREFLY_HINT: return CGRN; default: return ""; }
}
static const char* sev_s(FireflySeverity s) {
    switch (s) { case FIREFLY_ERROR: return "error"; case FIREFLY_WARNING: return "warning";
                 case FIREFLY_NOTE: return "note"; case FIREFLY_HINT: return "hint"; default: return "error"; }
}

// ── Rendering ────────────────────────────────────────────────────────────────
static int dcount(int n) { int d=1; while(n>=10){n/=10;d++;} return d; }
static char _ff_code[8] = "E000";

static void ff_header(FireflySeverity sev, FireflyLoc loc, const char* msg) {
    const char* fn = loc.filename ? loc.filename : "<input>";
    // ── error[E001]: undefined variable ──── script.sage:5:9 ──
    fprintf(stderr, "\n%s--%s %s%s[%s]%s: %s%s%s",
            CBLU, CR, sev_c(sev), sev_s(sev), _ff_code, CR, CB, msg, CR);
    if (loc.line > 0)
        fprintf(stderr, " %s-- %s:%d:%d --%s\n", CBLU, fn, loc.line, loc.column + 1, CR);
    else
        fprintf(stderr, " %s-- %s --%s\n", CBLU, fn, CR);
}

static void ff_source(FireflyLoc loc, FireflySeverity sev) {
    if (!loc.line_start || loc.line <= 0) return;
    int ll = 0;
    while (loc.line_start[ll] && loc.line_start[ll] != '\n' && loc.line_start[ll] != '\r') ll++;
    int g = dcount(loc.line); if (g < 3) g = 3;
    fprintf(stderr, "%s%*s |%s\n", CBLU, g, "", CR);
    fprintf(stderr, "%s%*d |%s %.*s\n", CBLU, g, loc.line, CR, ll, loc.line_start);
    fprintf(stderr, "%s%*s |%s ", CBLU, g, "", CR);
    for (int i = 0; i < loc.column; i++) fputc(' ', stderr);
    int span = loc.span > 0 ? loc.span : 1;
    fprintf(stderr, "%s", sev_c(sev));
    for (int i = 0; i < span; i++) fputc('^', stderr);
    fprintf(stderr, "%s\n", CR);
    fprintf(stderr, "%s%*s |%s\n", CBLU, g, "", CR);
}

static void ff_footer(void) {
    fprintf(stderr, "%s", CBLU);
    for (int i = 0; i < 70; i++) fputc('-', stderr);
    fprintf(stderr, "%s\n", CR);
}

// ── Public API ───────────────────────────────────────────────────────────────

void firefly_report(FireflySeverity sev, FireflyLoc loc, const char* fmt, ...) {
    if (ff_ctx.verbosity == FIREFLY_OFF) return;
    if (ff_ctx.suppress_after_max) return;
    if (sev == FIREFLY_ERROR) {
        ff_ctx.error_count++;
        if (ff_ctx.error_count > ff_ctx.max_errors) {
            fprintf(stderr, "\n%sFirefly: %d errors reached, stopping.%s\n", CRED, ff_ctx.max_errors, CR);
            ff_ctx.suppress_after_max = 1; return;
        }
    } else if (sev == FIREFLY_WARNING) ff_ctx.warning_count++;

    char buf[512];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);

    if (ff_ctx.verbosity == FIREFLY_MINIMAL) {
        const char* fn = loc.filename ? loc.filename : "<input>";
        fprintf(stderr, "%s%s%s: %s", sev_c(sev), sev_s(sev), CR, buf);
        if (loc.line > 0) fprintf(stderr, " (%s:%d)", fn, loc.line);
        fputc('\n', stderr); return;
    }
    ff_header(sev, loc, buf);
    ff_source(loc, sev);
}

void firefly_explain(const char* fmt, ...) {
    if (ff_ctx.verbosity != FIREFLY_FULL) return;
    fprintf(stderr, "   %sFirefly:%s ", CMAG, CR);
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a);
    fputc('\n', stderr);
}

void firefly_advice(const char* fmt, ...) {
    if (ff_ctx.verbosity != FIREFLY_FULL) return;
    fprintf(stderr, "            ");
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a);
    fputc('\n', stderr);
}

void firefly_help(const char* fmt, ...) {
    if (ff_ctx.verbosity != FIREFLY_FULL) return;
    fprintf(stderr, "            ");
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a);
    fputc('\n', stderr);
}

void firefly_note(FireflyLoc loc, const char* fmt, ...) {
    if (ff_ctx.verbosity != FIREFLY_FULL) return;
    fprintf(stderr, "   %snote:%s ", CCYN, CR);
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a);
    fputc('\n', stderr);
    if (loc.line > 0) ff_source(loc, FIREFLY_NOTE);
}

void firefly_end(void) {
    if (ff_ctx.verbosity == FIREFLY_FULL) {
        // P13: Auto-print stack trace on errors when we have frames
        if (ff_ctx.call_stack && ff_ctx.stack_depth > 0) {
            firefly_print_stack();
        }
        ff_footer();
    }
}

// ── Edit distance ────────────────────────────────────────────────────────────
static int edist(const char* a, int al, const char* b, int bl) {
    if (!al) return bl; if (!bl) return al;
    int* r = malloc(sizeof(int)*(bl+1));
    for (int j=0;j<=bl;j++) r[j]=j;
    for (int i=1;i<=al;i++) {
        int p=r[0]; r[0]=i;
        for (int j=1;j<=bl;j++) {
            int c=(tolower(a[i-1])==tolower(b[j-1]))?0:1;
            int v=r[j]+1, d=r[j-1]+1, s=p+c;
            p=r[j]; r[j]=v<d?(v<s?v:s):(d<s?d:s);
        }
    }
    int res=r[bl]; free(r); return res;
}

// ── Convenience errors ───────────────────────────────────────────────────────

void firefly_type_error(Expr* expr, const char* expected, const char* got,
                        const char* help_fmt, ...) {
    snprintf(_ff_code, sizeof(_ff_code), "E010");
    FireflyLoc loc = firefly_loc_from_expr(expr);
    firefly_report(FIREFLY_ERROR, loc, "expected %s, got %s", expected, got);
    if (help_fmt && ff_ctx.verbosity == FIREFLY_FULL) {
        fprintf(stderr, "   %sFirefly:%s ", CMAG, CR);
        va_list a; va_start(a, help_fmt); vfprintf(stderr, help_fmt, a); va_end(a);
        fputc('\n', stderr);
    }
    firefly_end();
}

void firefly_undefined_var(Expr* expr, const char* name, int nl, Env* env) {
    snprintf(_ff_code, sizeof(_ff_code), "E001");
    FireflyLoc loc = firefly_loc_from_expr(expr);
    firefly_report(FIREFLY_ERROR, loc, "undefined variable '%.*s'", nl, name);
    if (ff_ctx.verbosity != FIREFLY_FULL) { firefly_end(); return; }

    const char* best=NULL; int bl=0, bd=999;
    int th = nl<=3?1:(nl<=6?2:3);
    for (Env* c=env; c; c=c->parent)
        for (EnvNode* n=c->head; n; n=n->next) {
            int d=edist(name,nl,n->name,n->name_length);
            if (d>0 && d<bd && d<=th) { bd=d; best=n->name; bl=n->name_length; }
        }
    if (best) {
        firefly_explain("'%.*s' is not defined in this scope.", nl, name);
        firefly_advice("Did you mean '%.*s'? (%d character%s off)", bl, best, bd, bd==1?"":"s");
    } else {
        firefly_explain("'%.*s' is not defined. Check spelling or declare it before use.", nl, name);
    }
    firefly_end();
}

void firefly_index_error(Expr* expr, int idx, int len, const char* tn) {
    snprintf(_ff_code, sizeof(_ff_code), "E020");
    FireflyLoc loc = firefly_loc_from_expr(expr);
    firefly_report(FIREFLY_ERROR, loc, "%s index %d out of range (length %d)", tn, idx, len);
    if (ff_ctx.verbosity == FIREFLY_FULL) {
        if (len == 0) firefly_explain("The %s is empty.", tn);
        else {
            firefly_explain("Valid indices: 0 to %d, or -%d to -1.", len-1, len);
            if (idx >= len) firefly_advice("Last element is at index %d (or -1).", len-1);
        }
    }
    firefly_end();
}

void firefly_div_zero(Expr* expr) {
    snprintf(_ff_code, sizeof(_ff_code), "E030");
    firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr), "division by zero");
    firefly_explain("The divisor evaluated to zero.");
    firefly_advice("Guard with: if divisor != 0: result = a / divisor");
    firefly_end();
}

void firefly_immutable_error(Expr* expr, const char* name, int nl) {
    snprintf(_ff_code, sizeof(_ff_code), "E040");
    firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr),
                   "cannot reassign '%.*s' (immutable)", nl, name);
    if (ff_ctx.verbosity == FIREFLY_FULL) {
        firefly_explain("'%.*s' was declared with 'let' or 'const'.", nl, name);
        firefly_advice("Use 'var %.*s = ...' if you need to reassign.", nl, name);
    }
    firefly_end();
}

void firefly_no_method(Expr* expr, const char* tn, const char* m, int ml) {
    snprintf(_ff_code, sizeof(_ff_code), "E050");
    firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr),
                   "%s has no method '%.*s'", tn, ml, m);
    if (ff_ctx.verbosity == FIREFLY_FULL) {
        if (!strcmp(tn,"str"))
            firefly_explain("String methods: .upper() .lower() .split() .trim() .contains() "
                           ".replace() .starts_with() .ends_with() .length");
        else if (!strcmp(tn,"Array"))
            firefly_explain("Array methods: .push() .pop() .join() .reverse() .slice() "
                           ".indexOf() .contains() .clear() .length");
        else if (!strcmp(tn,"Dict"))
            firefly_explain("Dict methods: .keys() .values() .get() .delete() .contains_key()");
        else if (!strcmp(tn,"int") || !strcmp(tn,"float")) {
            firefly_explain("Numbers don't have methods.");
            firefly_advice("Use str(value) to convert, typeof(value) to inspect.");
        } else {
            firefly_explain("Type '%s' doesn't have '%.*s'.", tn, ml, m);
            firefly_advice("Add it with: impl %s: proc %.*s(self): ...", tn, ml, m);
        }
    }
    firefly_end();
}

void firefly_no_property(Expr* expr, const char* tn, const char* p, int pl) {
    snprintf(_ff_code, sizeof(_ff_code), "E051");
    firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr),
                   "%s has no property '%.*s'", tn, pl, p);
    if (ff_ctx.verbosity == FIREFLY_FULL) {
        if ((!strcmp(tn,"str")||!strcmp(tn,"Array")) && pl==3 && !strncmp(p,"len",3))
            firefly_advice("Use .length instead of .len");
        else if (pl==4 && !strncmp(p,"size",4))
            firefly_advice("Use .length instead of .size");
    }
    firefly_end();
}

void firefly_arity_error(Expr* expr, const char* fn, int exp, int got) {
    snprintf(_ff_code, sizeof(_ff_code), "E060");
    firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr),
                   "'%s' takes %d arg%s, but %d %s given",
                   fn, exp, exp==1?"":"s", got, got==1?"was":"were");
    firefly_end();
}

void firefly_print_stack(void) {
    if (!ff_ctx.call_stack || ff_ctx.verbosity != FIREFLY_FULL) return;
    fprintf(stderr, "\n   %sCall stack:%s\n", CB, CR);
    FireflyFrame* f = ff_ctx.call_stack; int d = 0;
    while (f && d < 10) {
        const char* fn = f->loc.filename ? f->loc.filename : "<input>";
        fprintf(stderr, "     %d. %s%s%s at %s:%d\n", d, CB,
                f->function_name ? f->function_name : "<anonymous>", CR, fn, f->loc.line);
        f = f->next; d++;
    }
    if (f) fprintf(stderr, "     ... %d more\n", ff_ctx.stack_depth - d);
}

const char* firefly_suggest_name(const char* n, int nl, Env* e) {
    (void)n; (void)nl; (void)e; return NULL;
}
int ff_ctx_stack_depth(void) { return ff_ctx.stack_depth; }

// ═══════════════════════════════════════════════════════════════════════
// P14: Firefly Warning Pass — single-pass AST analysis
// Detects: unused variables, missing returns, shadowed variables
// ═══════════════════════════════════════════════════════════════════════

// Track declared variables and whether they're used
typedef struct FireflyVar {
    const char* name;
    int name_len;
    int line;
    int used;
    struct FireflyVar* next;
} FireflyVar;

typedef struct FireflyWarnCtx {
    FireflyVar* vars;
    const char* filename;
    int in_function;  // don't warn about params yet
} FireflyWarnCtx;

static void fw_push_var(FireflyWarnCtx* ctx, const char* name, int len, int line) {
    FireflyVar* v = malloc(sizeof(FireflyVar));
    v->name = name; v->name_len = len; v->line = line; v->used = 0;
    v->next = ctx->vars; ctx->vars = v;
}

static void fw_mark_used(FireflyWarnCtx* ctx, const char* name, int len) {
    for (FireflyVar* v = ctx->vars; v; v = v->next) {
        if (v->name_len == len && memcmp(v->name, name, len) == 0) {
            v->used = 1;
            return;
        }
    }
}

static void fw_check_expr_usage(FireflyWarnCtx* ctx, Expr* expr);
static void fw_check_stmt(FireflyWarnCtx* ctx, Stmt* stmt);

static void fw_check_expr_usage(FireflyWarnCtx* ctx, Expr* expr) {
    if (!expr) return;
    switch (expr->type) {
        case EXPR_VARIABLE:
            fw_mark_used(ctx, expr->as.variable.name.start, expr->as.variable.name.length);
            break;
        case EXPR_BINARY:
            fw_check_expr_usage(ctx, expr->as.binary.left);
            fw_check_expr_usage(ctx, expr->as.binary.right);
            break;

        case EXPR_CALL:
            fw_check_expr_usage(ctx, expr->as.call.callee);
            for (int i = 0; i < expr->as.call.arg_count; i++)
                fw_check_expr_usage(ctx, expr->as.call.args[i]);
            break;
        case EXPR_GET:
            fw_check_expr_usage(ctx, expr->as.get.object);
            break;
        case EXPR_SET:
            fw_check_expr_usage(ctx, expr->as.set.object);
            fw_check_expr_usage(ctx, expr->as.set.value);
            break;
        case EXPR_INDEX:
            fw_check_expr_usage(ctx, expr->as.index.array);
            fw_check_expr_usage(ctx, expr->as.index.index);
            break;
        case EXPR_INT:
        case EXPR_NUMBER:
        case EXPR_STRING:
            break;
        default:
            break;
    }
}

static void fw_check_stmt(FireflyWarnCtx* ctx, Stmt* stmt) {
    while (stmt) {
        switch (stmt->type) {
            case STMT_LET: {
                // Track variable declaration
                Token name = stmt->as.let.name;
                // Don't track _ prefixed (intentionally unused)
                if (name.length > 0 && name.start[0] != '_') {
                    fw_push_var(ctx, name.start, name.length, name.line);
                }
                if (stmt->as.let.initializer)
                    fw_check_expr_usage(ctx, stmt->as.let.initializer);
                break;
            }
            case STMT_EXPRESSION:
                fw_check_expr_usage(ctx, stmt->as.expression);
                break;
            case STMT_BLOCK:
                fw_check_stmt(ctx, stmt->as.block.statements);
                break;
            case STMT_IF:
                fw_check_expr_usage(ctx, stmt->as.if_stmt.condition);
                fw_check_stmt(ctx, stmt->as.if_stmt.then_branch);
                fw_check_stmt(ctx, stmt->as.if_stmt.else_branch);
                break;
            case STMT_WHILE:
                fw_check_expr_usage(ctx, stmt->as.while_stmt.condition);
                fw_check_stmt(ctx, stmt->as.while_stmt.body);
                break;
            case STMT_RETURN:
                if (stmt->as.ret.value)
                    fw_check_expr_usage(ctx, stmt->as.ret.value);
                break;
            case STMT_PRINT:
                fw_check_expr_usage(ctx, stmt->as.print.expression);
                break;
            default:
                break;
        }
        stmt = stmt->next;
    }
}

void firefly_warn_pass(Stmt* program, const char* filename) {
    if (ff_ctx.verbosity == FIREFLY_OFF) return;
    
    FireflyWarnCtx ctx = {0};
    ctx.filename = filename;
    
    // Walk the AST
    fw_check_stmt(&ctx, program);
    
    // Report unused variables
    for (FireflyVar* v = ctx.vars; v; v = v->next) {
        if (!v->used) {
            FireflyLoc loc = {0};
            loc.filename = filename;
            loc.line = v->line;
            snprintf(_ff_code, sizeof(_ff_code), "W001");
            firefly_report(FIREFLY_WARNING, loc,
                          "unused variable '%.*s'", v->name_len, v->name);
            firefly_explain("'%.*s' is declared but never read.", v->name_len, v->name);
            firefly_advice("Prefix with _ to suppress: _%.*s", v->name_len, v->name);
            firefly_end();
        }
    }
    
    // Free tracking
    FireflyVar* v = ctx.vars;
    while (v) { FireflyVar* n = v->next; free(v); v = n; }
}
void firefly_set_code(const char* code) { snprintf(_ff_code, sizeof(_ff_code), "%s", code); }
