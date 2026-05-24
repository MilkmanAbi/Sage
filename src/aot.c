// aot.c — SageTree AOT Compiler (IP1-C rewrite)
// High-performance C code generation using sage_runtime.h
// Key: unboxed fast paths, range loop specialization, -O3 -flto output
#define _GNU_SOURCE
#include "aot.h"
#include "gc.h"
#include "lexer.h"
#include "parser.h"
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
// Forward declarations for closure capture helpers
static void _collect_free_vars(Expr* e, const char** caps, int* ncaps, int maxcaps, ProcStmt* ps);
static void _collect_free_vars_stmt(Stmt* s, const char** caps, int* ncaps, int maxcaps, ProcStmt* ps);
// Forward declarations for generator detection
static int _has_yield(Stmt* body);
static int _yields_are_sequential(Stmt* body);
// Forward declarations for proc emission (used by STMT_IMPORT before definition)
static void aot_emit_proc(AotCompiler* aot, Stmt* s);
static void aot_emit_nested_procs(AotCompiler* aot, Stmt* body);
// Forward declaration for expression compiler (used in aot_infer_types for default params)
static char* aot_expr(AotCompiler* aot, Expr* expr, JitTypeTag hint);


void aot_init(AotCompiler* aot, int opt_level) {
    memset(aot, 0, sizeof(AotCompiler));
    aot->opt_level   = opt_level;
    aot->emit_guards = 0;
}

void aot_free(AotCompiler* aot) {
    for (int i = 0; i < aot->line_count; i++) free(aot->lines[i]);
    free(aot->lines);
    for (int i = 0; i < aot->type_env.count; i++) free(aot->type_env.vars[i].name);
    free(aot->type_env.vars);
    memset(aot, 0, sizeof(AotCompiler));
}

static void aot_emit_raw(AotCompiler* aot, const char* s) {
    // Append to last line (no indent, no newline push)
    if (aot->line_count == 0) return;
    int last = aot->line_count - 1;
    size_t old_len = strlen(aot->lines[last]);
    size_t add_len = strlen(s);
    aot->lines[last] = realloc(aot->lines[last], old_len + add_len + 1);
    memcpy(aot->lines[last] + old_len, s, add_len + 1);
}

static void aot_emit(AotCompiler* aot, const char* fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int msg_len = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (msg_len < 0) { va_end(ap); return; }
    int indent = aot->indent * 4;
    char* line = malloc(indent + msg_len + 2);
    if (!line) { va_end(ap); return; }
    if (indent) memset(line, ' ', indent);
    vsnprintf(line + indent, msg_len + 1, fmt, ap);
    va_end(ap);
    if (aot->line_count >= aot->line_capacity) {
        aot->line_capacity = aot->line_capacity ? aot->line_capacity * 2 : 512;
        aot->lines = realloc(aot->lines, sizeof(char*) * aot->line_capacity);
    }
    aot->lines[aot->line_count++] = line;
}

static void aot_blank(AotCompiler* aot) { aot_emit(aot, ""); }

static char* aot_cname(const char* start, int len) {
    char* out = malloc(len + 4);
    out[0]='s'; out[1]='g'; out[2]='_';
    for (int i = 0; i < len; i++) {
        char c = start[i];
        out[i+3] = ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_') ? c : '_';
    }
    out[len+3] = '\0';
    return out;
}
static char* aot_cname_tok(Token t) { return aot_cname(t.start, t.length); }

static char* aot_temp(AotCompiler* aot) {
    char buf[24]; snprintf(buf, sizeof(buf), "_t%d", aot->next_temp++);
    return strdup(buf);
}

static char* aot_escape(const char* s) {
    int n = (int)strlen(s);
    char* out = malloc(n*4+1);
    int j = 0;
    for (int i = 0; i < n; i++) {
        switch ((unsigned char)s[i]) {
            case '\n': out[j++]='\\'; out[j++]='n';  break;
            case '\r': out[j++]='\\'; out[j++]='r';  break;
            case '\t': out[j++]='\\'; out[j++]='t';  break;
            case '\\': out[j++]='\\'; out[j++]='\\'; break;
            case '"':  out[j++]='\\'; out[j++]='"';  break;
            default:
                if ((unsigned char)s[i] < 0x20)
                    j += snprintf(out+j, 6, "\\x%02x", (unsigned char)s[i]);
                else out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return out;
}

void aot_set_var_type(AotCompiler* aot, const char* name, JitTypeTag type) {
    for (int i = 0; i < aot->type_env.count; i++) {
        if (strcmp(aot->type_env.vars[i].name, name) == 0) {
            if (aot->type_env.vars[i].inferred_type != type)
                aot->type_env.vars[i].inferred_type = JIT_TYPE_UNKNOWN;
            return;
        }
    }
    if (aot->type_env.count >= aot->type_env.capacity) {
        aot->type_env.capacity = aot->type_env.capacity ? aot->type_env.capacity*2 : 64;
        aot->type_env.vars = realloc(aot->type_env.vars, sizeof(AotVarType)*aot->type_env.capacity);
    }
    aot->type_env.vars[aot->type_env.count].name = strdup(name);
    aot->type_env.vars[aot->type_env.count].inferred_type = type;
    aot->type_env.count++;
}

JitTypeTag aot_get_var_type(AotCompiler* aot, const char* name) {
    for (int i = 0; i < aot->type_env.count; i++)
        if (strcmp(aot->type_env.vars[i].name, name) == 0)
            return aot->type_env.vars[i].inferred_type;
    return JIT_TYPE_UNKNOWN;
}

static JitTypeTag aot_infer_expr(AotCompiler* aot, Expr* expr) {
    if (!expr) return JIT_TYPE_NIL;
    switch (expr->type) {
        case EXPR_INT: return JIT_TYPE_INT;
        case EXPR_NUMBER:
            return JIT_TYPE_FLOAT;  // EXPR_NUMBER = float literal; EXPR_INT = integer
        case EXPR_STRING: return JIT_TYPE_STRING;
        case EXPR_BOOL:   return JIT_TYPE_BOOL;
        case EXPR_NIL:    return JIT_TYPE_NIL;
        case EXPR_ARRAY:  return JIT_TYPE_ARRAY;
        case EXPR_DICT:   return JIT_TYPE_DICT;
        case EXPR_TUPLE:  return JIT_TYPE_TUPLE;
        case EXPR_VARIABLE: {
            char name[256];
            int len = expr->as.variable.name.length < 255 ? expr->as.variable.name.length : 255;
            memcpy(name, expr->as.variable.name.start, len); name[len] = '\0';
            return aot_get_var_type(aot, name);
        }
        case EXPR_BINARY: {
            int op = expr->as.binary.op.type;
            // Unary ops stored as binary with NULL right
            if (op == TOKEN_NOT || op == TOKEN_TILDE) return JIT_TYPE_BOOL;
            if (!expr->as.binary.right) return JIT_TYPE_UNKNOWN;
            JitTypeTag L = aot_infer_expr(aot, expr->as.binary.left);
            JitTypeTag R = aot_infer_expr(aot, expr->as.binary.right);
            if (op==TOKEN_EQ||op==TOKEN_NEQ||op==TOKEN_GT||op==TOKEN_LT||op==TOKEN_GTE||op==TOKEN_LTE)
                return JIT_TYPE_BOOL;
            if (L==JIT_TYPE_INT && R==JIT_TYPE_INT) {
                if (op==TOKEN_PLUS||op==TOKEN_MINUS||op==TOKEN_STAR||op==TOKEN_PERCENT||
                    op==TOKEN_AMP||op==TOKEN_PIPE||op==TOKEN_CARET||op==TOKEN_LSHIFT||op==TOKEN_RSHIFT||
                    op==TOKEN_SLASH)
                    return JIT_TYPE_INT;
            }
            if ((L==JIT_TYPE_INT||L==JIT_TYPE_FLOAT) && (R==JIT_TYPE_INT||R==JIT_TYPE_FLOAT)) {
                if (op==TOKEN_PLUS||op==TOKEN_MINUS||op==TOKEN_STAR||op==TOKEN_SLASH)
                    return JIT_TYPE_FLOAT;
            }
            if (L==JIT_TYPE_STRING && R==JIT_TYPE_STRING && op==TOKEN_PLUS) return JIT_TYPE_STRING;
            return JIT_TYPE_UNKNOWN;
        }
        case EXPR_CALL: {
            if (expr->as.call.callee && expr->as.call.callee->type == EXPR_VARIABLE) {
                const char* n = expr->as.call.callee->as.variable.name.start;
                int nl = expr->as.call.callee->as.variable.name.length;
                #define BM(s) (nl==(int)strlen(s)&&memcmp(n,s,nl)==0)
                if (BM("len")||BM("int")) return JIT_TYPE_INT;
                if (BM("float")||BM("clock")) return JIT_TYPE_FLOAT;
                if (BM("str")||BM("typeof")) return JIT_TYPE_STRING;
                if (BM("bool")) return JIT_TYPE_BOOL;
                if (BM("range")||BM("range_inc")) return JIT_TYPE_ARRAY;
                // Struct constructor → STRUCT type (value semantics, copy on assign)
                for (int _si=0; _si<aot->known_struct_count; _si++) {
                    if (nl==(int)strlen(aot->known_structs[_si]) &&
                        memcmp(n, aot->known_structs[_si], nl)==0)
                        return JIT_TYPE_STRUCT;
                }
                #undef BM
            }
            return JIT_TYPE_UNKNOWN;
        }
        default: return JIT_TYPE_UNKNOWN;
    }
}

static void aot_register_proc(AotCompiler* aot, const char* name);
static int aot_is_known_proc(AotCompiler* aot, const char* name, int len);
void aot_infer_types(AotCompiler* aot, Stmt* program) {
    for (Stmt* s = program; s; s = s->next) {
        if (s->type == STMT_LET && s->as.let.initializer) {
            JitTypeTag t = aot_infer_expr(aot, s->as.let.initializer);
            char name[256];
            int len = s->as.let.name.length < 255 ? s->as.let.name.length : 255;
            memcpy(name, s->as.let.name.start, len); name[len] = '\0';
            aot_set_var_type(aot, name, t);
        }
        // Register all known C-callable names (procs, class constructors)
        if (s->type == STMT_PROC || s->type == STMT_ASYNC_PROC) {
            ProcStmt* ps = (s->type==STMT_PROC)?&s->as.proc:&s->as.async_proc;
            char* cn = aot_cname_tok(ps->name);
            aot_register_proc(aot, cn);
            // Generators always take SageValue params (constructor wraps them) — treat as ctors
            if (ps->body && _has_yield(ps->body)) {
                if (aot->known_ctor_count < 128)
                    snprintf(aot->known_ctors[aot->known_ctor_count++], 64, "%s", cn);
            }
            // Register default parameter values for call-site filling
            if (ps->defaults) {
                for (int _di=0; _di<ps->param_count; _di++) {
                    if (!ps->defaults[_di]) continue;
                    if (aot->proc_default_count >= 512) break;
                    // Store the default expr pointer — will be compiled at call site
                    snprintf(aot->proc_defaults[aot->proc_default_count].proc_cname, 64, "%s", cn);
                    aot->proc_defaults[aot->proc_default_count].param_idx = _di;
                    // Mark as "needs compilation" with a sentinel
                    snprintf(aot->proc_defaults[aot->proc_default_count].default_expr, 256, "__DEFAULT_PENDING_%p_%d",
                             (void*)ps->defaults[_di], _di);
                    aot->proc_defaults[aot->proc_default_count].default_ast = ps->defaults[_di];
                    aot->proc_default_count++;
                }
            }
            free(cn);
        }
        if (s->type == STMT_CLASS) {
            char* cn = aot_cname_tok(s->as.class_stmt.name);
            aot_register_proc(aot, cn);
            if (aot->known_ctor_count < 128)
                snprintf(aot->known_ctors[aot->known_ctor_count++], 64, "%s", cn);
            free(cn);
        }
        if (s->type == STMT_ENUM) {
            char* en = aot_cname_tok(s->as.enum_stmt.name);
            aot_register_proc(aot, en);
            // Enum namespace vars are DICT type (for field access via sage_rt_dict_get)
            char raw_en[256]; int enl=s->as.enum_stmt.name.length<255?s->as.enum_stmt.name.length:255;
            memcpy(raw_en,s->as.enum_stmt.name.start,enl); raw_en[enl]='\0';
            aot_set_var_type(aot, raw_en, JIT_TYPE_DICT);
            free(en);
        }
        if (s->type == STMT_STRUCT) {
            char* sn = aot_cname_tok(s->as.struct_stmt.name);
            aot_register_proc(aot, sn);
            // Register as ctor (always SageValue params)
            if (aot->known_ctor_count < 128)
                snprintf(aot->known_ctors[aot->known_ctor_count++], 64, "%s", sn);
            // Register raw name for struct-copy tracking
            if (aot->known_struct_count < 64) {
                char sraw[64]; int sl=s->as.struct_stmt.name.length<63?s->as.struct_stmt.name.length:63;
                memcpy(sraw,s->as.struct_stmt.name.start,sl); sraw[sl]='\0';
                snprintf(aot->known_structs[aot->known_struct_count++], 64, "%s", sraw);
            }
            free(sn);
        }
    }
    // Second pass: register enum variants with field names for ADT pattern matching
    for (Stmt* s = program; s; s = s->next) {
        if (s->type == STMT_ENUM) {
            EnumStmt* es = &s->as.enum_stmt;
            char en_raw[64]; int enl=es->name.length<63?es->name.length:63;
            memcpy(en_raw,es->name.start,enl); en_raw[enl]='\0';
            // Register raw enum name
            if (aot->known_enum_count < 64)
                snprintf(aot->known_enums[aot->known_enum_count++], 64, "%s", en_raw);
            // Register variant field info
            for (int vi=0; vi<es->variant_count; vi++) {
                if (!es->variant_field_counts || es->variant_field_counts[vi]==0) continue;
                if (aot->adt_variant_count >= 128) break;
                int slot = aot->adt_variant_count++;
                snprintf(aot->adt_variants[slot].enum_raw, 64, "%s", en_raw);
                int vnl=es->variant_names[vi].length<63?es->variant_names[vi].length:63;
                memcpy(aot->adt_variants[slot].variant_raw, es->variant_names[vi].start, vnl);
                aot->adt_variants[slot].variant_raw[vnl]='\0';
                int nf=es->variant_field_counts[vi]; if(nf>8)nf=8;
                aot->adt_variants[slot].field_count=nf;
                for (int fi=0;fi<nf;fi++) {
                    int fl=es->variant_fields[vi][fi].length<63?es->variant_fields[vi][fi].length:63;
                    memcpy(aot->adt_variants[slot].field_names[fi], es->variant_fields[vi][fi].start, fl);
                    aot->adt_variants[slot].field_names[fi][fl]='\0';
                }
            }
        }
    }
}

static void aot_infer_body(AotCompiler* aot, Stmt* body) {
    for (Stmt* s = body; s; s = s->next) {
        if (s->type == STMT_LET && s->as.let.initializer) {
            JitTypeTag t = aot_infer_expr(aot, s->as.let.initializer);
            char name[256];
            int len = s->as.let.name.length < 255 ? s->as.let.name.length : 255;
            memcpy(name, s->as.let.name.start, len); name[len] = '\0';
            aot_set_var_type(aot, name, t);
        } else if (s->type == STMT_BLOCK) {
            aot_infer_body(aot, s->as.block.statements);
        } else if (s->type == STMT_FOR) {
            // Infer the loop variable type from the iterable
            // range(a,b) and 0..N → INT loop var
            if (s->as.for_stmt.iterable) {
                JitTypeTag lt = JIT_TYPE_UNKNOWN;
                Expr* it = s->as.for_stmt.iterable;
                if (it->type == EXPR_RANGE) lt = JIT_TYPE_INT;
                else if (it->type == EXPR_CALL && it->as.call.callee &&
                         it->as.call.callee->type == EXPR_VARIABLE) {
                    int nl = it->as.call.callee->as.variable.name.length;
                    const char* ns = it->as.call.callee->as.variable.name.start;
                    if ((nl==5&&memcmp(ns,"range",5)==0)||(nl==8&&memcmp(ns,"range_in",8)==0))
                        lt = JIT_TYPE_INT;
                }
                if (lt != JIT_TYPE_UNKNOWN && s->as.for_stmt.variable.length > 0) {
                    char vn[256]; int vl=s->as.for_stmt.variable.length<255?s->as.for_stmt.variable.length:255;
                    memcpy(vn,s->as.for_stmt.variable.start,vl); vn[vl]='\0';
                    aot_set_var_type(aot, vn, lt);
                }
            }
            aot_infer_body(aot, s->as.for_stmt.body);
        } else if (s->type == STMT_WHILE) {
            aot_infer_body(aot, s->as.while_stmt.body);
        } else if (s->type == STMT_IF) {
            aot_infer_body(aot, s->as.if_stmt.then_branch);
            if (s->as.if_stmt.else_branch) aot_infer_body(aot, s->as.if_stmt.else_branch);
        }
    }
}

// Track known C-callable procs (not SageValue function vars)
static void aot_register_proc(AotCompiler* aot, const char* name) {
    if (aot->known_proc_count >= 256) return;
    snprintf(aot->known_procs[aot->known_proc_count++], 64, "%s", name);
}
static int aot_is_known_proc(AotCompiler* aot, const char* name, int len) {
    char buf[65]; int l = len < 64 ? len : 64;
    memcpy(buf, name, l); buf[l] = '\0';
    // Check raw name
    for (int i = 0; i < aot->known_proc_count; i++)
        if (strcmp(aot->known_procs[i], buf) == 0) return 1;
    // Check cname (sg_-prefixed)
    char cn[70] = "sg_";
    strncat(cn, buf, 63);
    for (int i = 0; i < aot->known_proc_count; i++)
        if (strcmp(aot->known_procs[i], cn) == 0) return 1;
    return 0;
}

static const char* jit_ctype(JitTypeTag t) {
    switch (t) {
        case JIT_TYPE_INT:    return "int64_t";
        case JIT_TYPE_FLOAT:  return "double";
        case JIT_TYPE_BOOL:   return "int";
        case JIT_TYPE_STRING: return "const char*";
        default:              return "SageValue";
    }
}
static int jit_is_unboxed(JitTypeTag t) {
    return t==JIT_TYPE_INT||t==JIT_TYPE_FLOAT||t==JIT_TYPE_BOOL||t==JIT_TYPE_STRING;
}

/* Forward declarations for call-site analysis (defined later in file) */
static JitTypeTag aot_param_type(AotCompiler* aot, const char* fname, int flen, int idx);

static char* aot_box(JitTypeTag from, const char* val) {
    char* out = malloc(strlen(val)+64);
    switch (from) {
        case JIT_TYPE_INT:    sprintf(out,"sage_rt_int(%s)",val);    break;
        case JIT_TYPE_FLOAT:  sprintf(out,"sage_rt_float(%s)",val);  break;
        case JIT_TYPE_BOOL:   sprintf(out,"sage_rt_bool(%s)",val);   break;
        case JIT_TYPE_STRING: sprintf(out,"sage_rt_string(%s)",val); break;
        default: strcpy(out,val); break;
    }
    return out;
}

// Returns a SageValue expression for `e`, boxing if the expr is an unboxed scalar.
// Use this whenever you need to pass to a runtime function expecting SageValue.
static char* aot_expr_boxed(AotCompiler* aot, Expr* e) {
    if (!e) return strdup("sage_rt_nil()");
    JitTypeTag t = aot_infer_expr(aot, e);
    if (jit_is_unboxed(t)) {
        if (e->type == EXPR_VARIABLE) {
            // Variable: get raw scalar and box it
            char* raw = aot_expr(aot, e, t);
            char* boxed = aot_box(t, raw);
            free(raw);
            return boxed;
        }
        // Other exprs: UNKNOWN hint auto-boxes binary ops etc.
        char* v = aot_expr(aot, e, JIT_TYPE_UNKNOWN);
        if (strncmp(v, "sage_rt_", 8) == 0 || strncmp(v, "({", 2) == 0)
            return v;
        char* boxed = aot_box(t, v);
        free(v);
        return boxed;
    }
    return aot_expr(aot, e, JIT_TYPE_UNKNOWN);
}

static char* aot_expr(AotCompiler* aot, Expr* expr, JitTypeTag hint) {
    if (!expr) return strdup("sage_rt_nil()");
    JitTypeTag inferred = aot_infer_expr(aot, expr);
    (void)inferred;

    switch (expr->type) {
        case EXPR_INT: {
            char buf[32];
            snprintf(buf,sizeof(buf),"INT64_C(%lld)",(long long)expr->as.int_val.value);
            if (hint==JIT_TYPE_INT) return strdup(buf);
            if (hint==JIT_TYPE_FLOAT) {
                // Return as raw double constant for float arithmetic
                char* b=malloc(48); sprintf(b,"(double)INT64_C(%lld)",(long long)expr->as.int_val.value); return b;
            }
            char* b=malloc(64); sprintf(b,"sage_rt_int(INT64_C(%lld))",(long long)expr->as.int_val.value); return b;
        }
        case EXPR_NUMBER: {
            double d = expr->as.number.value;
            if (hint==JIT_TYPE_FLOAT || hint==JIT_TYPE_INT) {
                char buf[32]; snprintf(buf,sizeof(buf),"%.17g",d); return strdup(buf);
            }
            char* b=malloc(64); sprintf(b,"sage_rt_float(%.17g)",d); return b;
        }
        case EXPR_STRING: {
            char* esc = aot_escape(expr->as.string.value);
            if (hint==JIT_TYPE_STRING) { char* b=malloc(strlen(esc)+4); sprintf(b,"\"%s\"",esc); free(esc); return b; }
            char* b=malloc(strlen(esc)+32); sprintf(b,"sage_rt_string(\"%s\")",esc); free(esc); return b;
        }
        case EXPR_BOOL:
            if (hint==JIT_TYPE_BOOL) return strdup(expr->as.boolean.value?"1":"0");
            return strdup(expr->as.boolean.value?"sage_rt_bool(1)":"sage_rt_bool(0)");
        case EXPR_NIL: return strdup("sage_rt_nil()");

        case EXPR_VARIABLE: {
            char name[256];
            int len = expr->as.variable.name.length<255?expr->as.variable.name.length:255;
            memcpy(name,expr->as.variable.name.start,len); name[len]='\0';
            // Handle None/nil keywords
            if (!strcmp(name,"None") || !strcmp(name,"nil")) return strdup("sage_rt_nil()");
            if (!strcmp(name,"True")) return hint==JIT_TYPE_BOOL ? strdup("1") : strdup("sage_rt_bool(1)");
            if (!strcmp(name,"False")) return hint==JIT_TYPE_BOOL ? strdup("0") : strdup("sage_rt_bool(0)");
            JitTypeTag vtype = aot_get_var_type(aot,name);
            char* cname = aot_cname(expr->as.variable.name.start, expr->as.variable.name.length);
            if (!jit_is_unboxed(vtype)) return cname;
            // Caller wants same unboxed type, or any unboxed type (caller casts) — return raw name
            if (hint==vtype || hint==JIT_TYPE_UNKNOWN) return cname;
            if (jit_is_unboxed(hint)) return cname; // caller will cast (e.g. int->double)
            // Caller needs SageValue — box it
            char* boxed = aot_box(vtype, cname);
            free(cname); return boxed;
        }

        case EXPR_BINARY: {
            int op = expr->as.binary.op.type;
            // Unary ops stored as binary with NULL right (parser convention)
            if (op == TOKEN_NOT) {
                // `not expr` — right is NULL, operand is left
                char* operand = aot_expr(aot, expr->as.binary.left, JIT_TYPE_UNKNOWN);
                JitTypeTag ot = aot_infer_expr(aot, expr->as.binary.left);
                char* raw = (expr->as.binary.left->type == EXPR_VARIABLE && jit_is_unboxed(ot))
                    ? aot_box(ot, operand) : operand;
                char* out = malloc(strlen(raw) + 64);
                sprintf(out, "sage_rt_bool(!sage_rt_truthy(%s))", raw);
                if (raw != operand) free(raw);
                free(operand); return out;
            }
            if (op == TOKEN_TILDE) {
                // Bitwise NOT: ~expr
                char* operand = aot_expr(aot, expr->as.binary.left, JIT_TYPE_INT);
                JitTypeTag ot = aot_infer_expr(aot, expr->as.binary.left);
                char* out;
                if (ot == JIT_TYPE_INT) {
                    out = malloc(strlen(operand) + 32);
                    sprintf(out, "sage_rt_int(~(%s))", operand);
                } else {
                    char* raw = (expr->as.binary.left->type == EXPR_VARIABLE && jit_is_unboxed(ot))
                        ? aot_box(ot, operand) : operand;
                    out = malloc(strlen(raw) + 32);
                    sprintf(out, "sage_rt_bnot(%s)", raw);
                    if (raw != operand) free(raw);
                }
                free(operand); return out;
            }
            if (!expr->as.binary.right) return strdup("sage_rt_nil()"); // safety
            JitTypeTag L = aot_infer_expr(aot, expr->as.binary.left);
            JitTypeTag R = aot_infer_expr(aot, expr->as.binary.right);

            // -- Unboxed integer arithmetic --
            if (L==JIT_TYPE_INT && R==JIT_TYPE_INT) {
                char* lv = aot_expr(aot, expr->as.binary.left,  JIT_TYPE_INT);
                char* rv = aot_expr(aot, expr->as.binary.right, JIT_TYPE_INT);
                char* out = malloc(strlen(lv)+strlen(rv)+32);
                int is_cmp=0;
                switch(op) {
                    case TOKEN_PLUS:    sprintf(out,"((%s)+(%s))",lv,rv); break;
                    case TOKEN_MINUS:   sprintf(out,"((%s)-(%s))",lv,rv); break;
                    case TOKEN_STAR:    sprintf(out,"((%s)*(%s))",lv,rv); break;
                    case TOKEN_SLASH:   sprintf(out,"((%s)/(%s))",lv,rv); break;
                    case TOKEN_PERCENT: sprintf(out,"((%s)%%(%s))",lv,rv); break;
                    case TOKEN_AMP:     sprintf(out,"((%s)&(%s))",lv,rv); break;
                    case TOKEN_PIPE:    sprintf(out,"((%s)|(%s))",lv,rv); break;
                    case TOKEN_CARET:   sprintf(out,"((%s)^(%s))",lv,rv); break;
                    case TOKEN_LSHIFT:  sprintf(out,"((%s)<<(int)(%s))",lv,rv); break;
                    case TOKEN_RSHIFT:  sprintf(out,"((%s)>>(int)(%s))",lv,rv); break;
                    case TOKEN_EQ:  is_cmp=1; sprintf(out,"((%s)==(%s))",lv,rv); break;
                    case TOKEN_NEQ: is_cmp=1; sprintf(out,"((%s)!=(%s))",lv,rv); break;
                    case TOKEN_GT:  is_cmp=1; sprintf(out,"((%s)>(%s))",lv,rv);  break;
                    case TOKEN_LT:  is_cmp=1; sprintf(out,"((%s)<(%s))",lv,rv);  break;
                    case TOKEN_GTE: is_cmp=1; sprintf(out,"((%s)>=(%s))",lv,rv); break;
                    case TOKEN_LTE: is_cmp=1; sprintf(out,"((%s)<=(%s))",lv,rv); break;
                    default: sprintf(out,"sage_rt_add(sage_rt_int(%s),sage_rt_int(%s))",lv,rv); break;
                }
                free(lv); free(rv);
                JitTypeTag ret_t = is_cmp ? JIT_TYPE_BOOL : JIT_TYPE_INT;
                if (hint==ret_t || (hint!=JIT_TYPE_UNKNOWN && jit_is_unboxed(hint))) return out;
                char* b = aot_box(ret_t,out); free(out); return b;
            }

            // -- Unboxed float arithmetic --
            if ((L==JIT_TYPE_FLOAT||L==JIT_TYPE_INT) && (R==JIT_TYPE_FLOAT||R==JIT_TYPE_INT)) {
                char* lv = aot_expr(aot, expr->as.binary.left,  JIT_TYPE_FLOAT);
                char* rv = aot_expr(aot, expr->as.binary.right, JIT_TYPE_FLOAT);
                if (L==JIT_TYPE_INT) { char* t=malloc(strlen(lv)+16); sprintf(t,"(double)(%s)",lv); free(lv); lv=t; }
                if (R==JIT_TYPE_INT) { char* t=malloc(strlen(rv)+16); sprintf(t,"(double)(%s)",rv); free(rv); rv=t; }
                char* out=malloc(strlen(lv)+strlen(rv)+32);
                int is_cmp=0;
                switch(op) {
                    case TOKEN_PLUS:    sprintf(out,"((%s)+(%s))",lv,rv); break;
                    case TOKEN_MINUS:   sprintf(out,"((%s)-(%s))",lv,rv); break;
                    case TOKEN_STAR:    sprintf(out,"((%s)*(%s))",lv,rv); break;
                    case TOKEN_SLASH:   sprintf(out,"((%s)/(%s))",lv,rv); break;
                    case TOKEN_PERCENT: sprintf(out,"fmod((%s),(%s))",lv,rv); break;
                    case TOKEN_EQ:  is_cmp=1; sprintf(out,"((%s)==(%s))",lv,rv); break;
                    case TOKEN_NEQ: is_cmp=1; sprintf(out,"((%s)!=(%s))",lv,rv); break;
                    case TOKEN_GT:  is_cmp=1; sprintf(out,"((%s)>(%s))",lv,rv);  break;
                    case TOKEN_LT:  is_cmp=1; sprintf(out,"((%s)<(%s))",lv,rv);  break;
                    case TOKEN_GTE: is_cmp=1; sprintf(out,"((%s)>=(%s))",lv,rv); break;
                    case TOKEN_LTE: is_cmp=1; sprintf(out,"((%s)<=(%s))",lv,rv); break;
                    default: sprintf(out,"(%s)+(%s)",lv,rv); break;
                }
                free(lv); free(rv);
                JitTypeTag ret_t = is_cmp ? JIT_TYPE_BOOL : JIT_TYPE_FLOAT;
                if (hint==ret_t || (hint!=JIT_TYPE_UNKNOWN && jit_is_unboxed(hint))) return out;
                char* b = aot_box(ret_t,out); free(out); return b;
            }

            // -- String concat: box raw string vars; pass SageValue exprs directly --
            if (L==JIT_TYPE_STRING && R==JIT_TYPE_STRING && op==TOKEN_PLUS) {
                // aot_expr(., UNKNOWN) returns const char* for STRING variables, SageValue for everything else
                char* lv = aot_expr(aot, expr->as.binary.left,  JIT_TYPE_UNKNOWN);
                char* rv = aot_expr(aot, expr->as.binary.right, JIT_TYPE_UNKNOWN);
                int lv_needs_box = (expr->as.binary.left->type  == EXPR_VARIABLE);
                int rv_needs_box = (expr->as.binary.right->type == EXPR_VARIABLE);
                char* ls = lv_needs_box ? aot_box(JIT_TYPE_STRING, lv) : lv;
                char* rs = rv_needs_box ? aot_box(JIT_TYPE_STRING, rv) : rv;
                char* out = malloc(strlen(ls)+strlen(rs)+48);
                sprintf(out,"sage_rt_string_concat(%s,%s)",ls,rs);
                if (ls != lv) free(ls);
                if (rs != rv) free(rs);
                free(lv); free(rv); return out;
            }

            // -- Logical --
            if (op==TOKEN_AND) {
                char* lv=aot_expr(aot,expr->as.binary.left,JIT_TYPE_UNKNOWN);
                char* rv=aot_expr(aot,expr->as.binary.right,JIT_TYPE_UNKNOWN);
                char* out=malloc(strlen(lv)*2+strlen(rv)+64);
                sprintf(out,"(sage_rt_truthy(%s)?(%s):(%s))",lv,rv,lv);
                free(lv); free(rv); return out;
            }
            if (op==TOKEN_OR) {
                char* lv=aot_expr(aot,expr->as.binary.left,JIT_TYPE_UNKNOWN);
                char* rv=aot_expr(aot,expr->as.binary.right,JIT_TYPE_UNKNOWN);
                char* out=malloc(strlen(lv)*2+strlen(rv)+64);
                sprintf(out,"(sage_rt_truthy(%s)?(%s):(%s))",lv,lv,rv);
                free(lv); free(rv); return out;
            }

            // -- String repeat: "ha" * 3 or 3 * "ha" --
            if ((L==JIT_TYPE_STRING&&R==JIT_TYPE_INT || L==JIT_TYPE_INT&&R==JIT_TYPE_STRING) && op==TOKEN_STAR) {
                char*sv=aot_expr(aot,L==JIT_TYPE_STRING?expr->as.binary.left:expr->as.binary.right,JIT_TYPE_UNKNOWN);
                char*iv=aot_expr(aot,L==JIT_TYPE_INT?expr->as.binary.left:expr->as.binary.right,JIT_TYPE_INT);
                // Box string var if raw
                int snb=(L==JIT_TYPE_STRING?expr->as.binary.left:expr->as.binary.right)->type==EXPR_VARIABLE;
                char*svb=snb?aot_box(JIT_TYPE_STRING,sv):sv;
                size_t sz=strlen(svb)+strlen(iv)+256;
                char*out=malloc(sz);
                snprintf(out,sz,"sage_rt_str_repeat(%s,%s)",svb,iv);
                if(svb!=sv)free(svb); free(sv); free(iv); return out;
            }
            // -- Generic fallback --
            // aot_expr(expr, UNKNOWN) returns SageValue for everything EXCEPT
            // EXPR_VARIABLE with an unboxed type (which returns the raw C name).
            // So: only box bare unboxed variables; all other exprs are already SageValue.
            {
                #define _NEEDS_BOX(e) ((e)->type == EXPR_VARIABLE && jit_is_unboxed(aot_infer_expr(aot,(e))))
                char* _lraw = aot_expr(aot, expr->as.binary.left,  JIT_TYPE_UNKNOWN);
                char* _rraw = aot_expr(aot, expr->as.binary.right, JIT_TYPE_UNKNOWN);
                char* lv = _NEEDS_BOX(expr->as.binary.left)
                    ? aot_box(aot_infer_expr(aot,expr->as.binary.left), _lraw) : _lraw;
                char* rv = _NEEDS_BOX(expr->as.binary.right)
                    ? aot_box(aot_infer_expr(aot,expr->as.binary.right), _rraw) : _rraw;
                if (lv != _lraw) free(_lraw);
                if (rv != _rraw) free(_rraw);
                #undef _NEEDS_BOX
                char* out=malloc(strlen(lv)+strlen(rv)+64);
                switch(op) {
                    case TOKEN_PLUS:    sprintf(out,"sage_rt_add(%s,%s)",lv,rv); break;
                    case TOKEN_MINUS:   sprintf(out,"sage_rt_sub(%s,%s)",lv,rv); break;
                    case TOKEN_STAR:    sprintf(out,"sage_rt_mul(%s,%s)",lv,rv); break;
                    case TOKEN_SLASH:   sprintf(out,"sage_rt_div(%s,%s)",lv,rv); break;
                    case TOKEN_PERCENT: sprintf(out,"sage_rt_mod(%s,%s)",lv,rv); break;
                    case TOKEN_EQ:      sprintf(out,"sage_rt_bool(sage_rt_equal(%s,%s))",lv,rv); break;
                    case TOKEN_NEQ:     sprintf(out,"sage_rt_bool(!sage_rt_equal(%s,%s))",lv,rv); break;
                    case TOKEN_GT:      sprintf(out,"sage_rt_bool(sage_rt_less(%s,%s))",rv,lv); break;
                    case TOKEN_LT:      sprintf(out,"sage_rt_bool(sage_rt_less(%s,%s))",lv,rv); break;
                    case TOKEN_GTE:     sprintf(out,"sage_rt_bool(!sage_rt_less(%s,%s))",lv,rv); break;
                    case TOKEN_LTE:     sprintf(out,"sage_rt_bool(!sage_rt_less(%s,%s))",rv,lv); break;
                    case TOKEN_AMP:     sprintf(out,"sage_rt_band(%s,%s)",lv,rv); break;
                    case TOKEN_PIPE:    sprintf(out,"sage_rt_bor(%s,%s)",lv,rv); break;
                    case TOKEN_CARET:   sprintf(out,"sage_rt_bxor(%s,%s)",lv,rv); break;
                    case TOKEN_LSHIFT:  sprintf(out,"sage_rt_shl(%s,%s)",lv,rv); break;
                    case TOKEN_RSHIFT:  sprintf(out,"sage_rt_shr(%s,%s)",lv,rv); break;
                    default:            sprintf(out,"sage_rt_add(%s,%s)",lv,rv); break;
                }
                free(lv); free(rv); return out;
            }
        }

        case EXPR_NULLCOAL: {
            // Must pass SageValues to sage_rt_nullcoal — box raw vars
            char* lv=aot_expr(aot,expr->as.nullcoal.left,JIT_TYPE_UNKNOWN);
            char* rv=aot_expr(aot,expr->as.nullcoal.right,JIT_TYPE_UNKNOWN);
            JitTypeTag lt=aot_infer_expr(aot,expr->as.nullcoal.left);
            JitTypeTag rt=aot_infer_expr(aot,expr->as.nullcoal.right);
            char* lbs=(expr->as.nullcoal.left->type==EXPR_VARIABLE&&jit_is_unboxed(lt))?aot_box(lt,lv):lv;
            char* rbs=(expr->as.nullcoal.right->type==EXPR_VARIABLE&&jit_is_unboxed(rt))?aot_box(rt,rv):rv;
            char* out=malloc(strlen(lbs)+strlen(rbs)+48);
            sprintf(out,"sage_rt_nullcoal(%s,%s)",lbs,rbs);
            if(lbs!=lv)free(lbs); if(rbs!=rv)free(rbs);
            free(lv); free(rv); return out;
        }

        case EXPR_RANGE: {
            char* lo=aot_expr(aot,expr->as.range.low,JIT_TYPE_INT);
            char* hi=aot_expr(aot,expr->as.range.high,JIT_TYPE_INT);
            char* out=malloc(strlen(lo)+strlen(hi)+80);
            if (expr->as.range.inclusive)
                sprintf(out,"sage_rt_range_inc(sage_rt_int(%s),sage_rt_int(%s))",lo,hi);
            else
                sprintf(out,"sage_rt_range(sage_rt_int(%s),sage_rt_int(%s))",lo,hi);
            free(lo); free(hi); return out;
        }

        case EXPR_FORCE_UNWRAP: {
            char* inner=aot_expr(aot,expr->as.unwrap.operand,JIT_TYPE_UNKNOWN);
            char* out=malloc(strlen(inner)+512);
            sprintf(out,"({SageValue _fu=(%s);"
                    "if(SAGE_IS_NIL(_fu))sage_rt_fatal(\"force-unwrap on nil\");"
                    "if(SAGE_IS_DICT(_fu)){SageValue _ft=sage_rt_dict_get(_fu,sage_rt_string(\"__type\"));"
                    "if(SAGE_IS_STRING(_ft)&&(strcmp(_ft.as.string,\"Some\")==0||strcmp(_ft.as.string,\"Ok\")==0))"
                    "{_fu=sage_rt_dict_get(_fu,sage_rt_string(\"value\"));}} _fu;})",inner);
            free(inner); return out;
        }
        case EXPR_PROPAGATE: {
            char* inner=aot_expr(aot,expr->as.unwrap.operand,JIT_TYPE_UNKNOWN);
            char* out=malloc(strlen(inner)+512);
            sprintf(out,"({SageValue _pp=(%s);"
                    "if(SAGE_IS_NIL(_pp))return sage_rt_nil();"
                    "if(SAGE_IS_DICT(_pp)){SageValue _pt=sage_rt_dict_get(_pp,sage_rt_string(\"__type\"));"
                    "if(SAGE_IS_STRING(_pt)&&strcmp(_pt.as.string,\"Err\")==0)return _pp;"
                    "if(SAGE_IS_STRING(_pt)&&strcmp(_pt.as.string,\"Ok\")==0)"
                    "{_pp=sage_rt_dict_get(_pp,sage_rt_string(\"value\"));}} _pp;})",inner);
            free(inner); return out;
        }

        case EXPR_ARRAY: {
            int n=expr->as.array.count;
            if (n==0) return strdup("sage_rt_array_new()");
            char* tmp=aot_temp(aot);
            size_t sz=strlen(tmp)+128;
            for(int i=0;i<n;i++){char*e=aot_expr(aot,expr->as.array.elements[i],JIT_TYPE_UNKNOWN);sz+=strlen(e)+128;free(e);}
            char* out=malloc(sz);
            int pos=sprintf(out,"({SageValue %s=sage_rt_array_new();",tmp);
            char* idx_tmp=aot_temp(aot);
            for(int i=0;i<n;i++){
                Expr* el=expr->as.array.elements[i];
                // Spread element: EXPR_BINARY(left=arr, op=TOKEN_DOTDOT, right=NULL)
                if (el->type==EXPR_BINARY && el->as.binary.op.type==TOKEN_DOTDOT && !el->as.binary.right) {
                    char*spread_arr=aot_expr(aot,el->as.binary.left,JIT_TYPE_UNKNOWN);
                    pos+=sprintf(out+pos,
                        "{ SageValue _sp=%s; for(int %s=0;%s<sage_rt_array_len(_sp);%s++) sage_rt_array_push(%s,sage_rt_array_get(_sp,sage_rt_int(%s))); }",
                        spread_arr,idx_tmp,idx_tmp,idx_tmp,tmp,idx_tmp);
                    free(spread_arr);
                } else {
                    char*e=aot_expr_boxed(aot,el);
                    pos+=sprintf(out+pos,"sage_rt_array_push(%s,%s);",tmp,e);
                    free(e);
                }
            }
            sprintf(out+pos,"%s;})",tmp);
            free(idx_tmp); free(tmp); return out;
        }

        case EXPR_DICT: {
            int n=expr->as.dict.count;
            if (n==0) return strdup("sage_rt_dict_new()");
            char* tmp=aot_temp(aot);
            size_t sz=strlen(tmp)+128;
            for(int i=0;i<n;i++){char*v=aot_expr(aot,expr->as.dict.values[i],JIT_TYPE_UNKNOWN);sz+=strlen(v)+strlen(expr->as.dict.keys[i])+80;free(v);}
            char* out=malloc(sz);
            int pos=sprintf(out,"({SageValue %s=sage_rt_dict_new();",tmp);
            for(int i=0;i<n;i++){
                char*ek=aot_escape(expr->as.dict.keys[i]);
                char*ev=aot_expr(aot,expr->as.dict.values[i],JIT_TYPE_UNKNOWN);
                pos+=sprintf(out+pos,"sage_rt_dict_set(%s,sage_rt_string(\"%s\"),%s);",tmp,ek,ev);
                free(ek);free(ev);
            }
            sprintf(out+pos,"%s;})",tmp); free(tmp); return out;
        }

        case EXPR_TUPLE: {
            int n=expr->as.tuple.count;
            size_t sz=64;
            for(int i=0;i<n;i++){char*e=aot_expr(aot,expr->as.tuple.elements[i],JIT_TYPE_UNKNOWN);sz+=strlen(e)+8;free(e);}
            char* out=malloc(sz);
            int pos=sprintf(out,"sage_rt_tuple_new(%d",n);
            for(int i=0;i<n;i++){char*e=aot_expr(aot,expr->as.tuple.elements[i],JIT_TYPE_UNKNOWN);pos+=sprintf(out+pos,",%s",e);free(e);}
            sprintf(out+pos,")"); return out;
        }

        case EXPR_INDEX: {
            JitTypeTag objt=aot_infer_expr(aot,expr->as.index.array);
            JitTypeTag idxt=aot_infer_expr(aot,expr->as.index.index);
            // For TUPLE, use integer index directly with tuple_get
            if (objt == JIT_TYPE_TUPLE) {
                char* obj=aot_expr(aot,expr->as.index.array,JIT_TYPE_UNKNOWN);
                char* idx=aot_expr(aot,expr->as.index.index,JIT_TYPE_INT);
                char* out=malloc(strlen(obj)+strlen(idx)+64);
                sprintf(out,"sage_rt_tuple_get(%s,(int)(%s))",obj,idx);
                free(obj);free(idx); return out;
            }
            // For STRING, use str_index (supports negative indexing)
            if (objt == JIT_TYPE_STRING) {
                // Box raw string variable before passing
                char* obj_raw=aot_expr(aot,expr->as.index.array,JIT_TYPE_UNKNOWN);
                int obj_is_raw=(expr->as.index.array->type==EXPR_VARIABLE);
                char* obj=obj_is_raw?aot_box(JIT_TYPE_STRING,obj_raw):obj_raw;
                char* idx=aot_expr(aot,expr->as.index.index,JIT_TYPE_INT);
                char* out=malloc(strlen(obj)+strlen(idx)+64);
                sprintf(out,"sage_rt_str_index(%s,(int64_t)(%s))",obj,idx);
                if(obj!=obj_raw)free(obj); free(obj_raw); free(idx); return out;
            }
            char* obj=aot_expr(aot,expr->as.index.array,JIT_TYPE_UNKNOWN);
            // Index must be SageValue for array_get/dict_get — box if unboxed raw scalar
            char* idx_raw=aot_expr(aot,expr->as.index.index,JIT_TYPE_UNKNOWN);
            char* idx;
            if (jit_is_unboxed(idxt) && strncmp(idx_raw,"sage_rt_",8)!=0) {
                // Raw scalar — box it
                idx = aot_box(idxt, idx_raw);
                free(idx_raw);
            } else {
                idx = idx_raw; // already boxed or SageValue
            }
            char* out_idx=malloc(strlen(obj)+strlen(idx)+64);
            if (objt==JIT_TYPE_DICT || idxt==JIT_TYPE_STRING)
                sprintf(out_idx,"sage_rt_dict_get(%s,%s)",obj,idx);
            else
                sprintf(out_idx,"sage_rt_array_get(%s,%s)",obj,idx);
            free(obj); free(idx); return out_idx;
        }
        case EXPR_INDEX_SET: {
            char* obj=aot_expr(aot,expr->as.index_set.array,JIT_TYPE_UNKNOWN);
            JitTypeTag _ot=aot_infer_expr(aot,expr->as.index_set.array);
            JitTypeTag _it=aot_infer_expr(aot,expr->as.index_set.index);
            char* idx_raw=aot_expr(aot,expr->as.index_set.index,JIT_TYPE_UNKNOWN);
            char* idx;
            if (jit_is_unboxed(_it) && strncmp(idx_raw,"sage_rt_",8)!=0)
                { idx=aot_box(_it,idx_raw); free(idx_raw); }
            else
                idx=idx_raw;
            char* val=aot_expr_boxed(aot,expr->as.index_set.value);
            // val appears twice in output
            char* out=malloc(strlen(obj)+strlen(idx)+strlen(val)*2+64);
            if (_ot==JIT_TYPE_DICT||_ot==JIT_TYPE_UNKNOWN)
                sprintf(out,"({sage_rt_dict_set(%s,%s,%s);%s;})",obj,idx,val,val);
            else
                sprintf(out,"({sage_rt_array_set(%s,%s,%s);%s;})",obj,idx,val,val);
            free(obj);free(idx);free(val); return out;
        }
        case EXPR_SLICE: {
            char* obj=aot_expr(aot,expr->as.slice.array,JIT_TYPE_UNKNOWN);
            char* s=expr->as.slice.start?aot_expr(aot,expr->as.slice.start,JIT_TYPE_INT):strdup("0");
            char* e=expr->as.slice.end?aot_expr(aot,expr->as.slice.end,JIT_TYPE_INT):strdup("-1");
            char* out=malloc(strlen(obj)+strlen(s)+strlen(e)+48);
            sprintf(out,"sage_rt_array_slice(%s,(int)(%s),(int)(%s))",obj,s,e);
            free(obj);free(s);free(e); return out;
        }
        case EXPR_GET: {
            char* obj=aot_expr(aot,expr->as.get.object,JIT_TYPE_UNKNOWN);
            char* prop=aot_escape(expr->as.get.property.start);
            prop[expr->as.get.property.length]='\0';
            // Dict field access: d.key → sage_rt_dict_get(d, "key")
            JitTypeTag _get_ot = aot_infer_expr(aot, expr->as.get.object);
            if (_get_ot == JIT_TYPE_DICT) {
                char* out = malloc(strlen(obj)+strlen(prop)+64);
                sprintf(out, "sage_rt_dict_get(%s,sage_rt_string(\"%s\"))", obj, prop);
                free(obj); free(prop); return out;
            }
            // Fast path for common properties
            if (!strcmp(prop,"length")||!strcmp(prop,"count")||!strcmp(prop,"size")) {
                JitTypeTag ot=aot_infer_expr(aot,expr->as.get.object);
                // Only apply length fast path for collection types, not instance fields named "count"
                if (ot==JIT_TYPE_STRING||ot==JIT_TYPE_ARRAY||ot==JIT_TYPE_TUPLE||ot==JIT_TYPE_DICT) {
                    int obj_is_raw=(expr->as.get.object->type==EXPR_VARIABLE&&jit_is_unboxed(ot));
                    char* obj_sv=obj_is_raw?aot_box(ot,obj):obj;
                    char* out=malloc(strlen(obj_sv)+64);
                    if (ot==JIT_TYPE_STRING) sprintf(out,"sage_rt_int(sage_rt_str_len(%s))",obj_sv);
                        else sprintf(out,"sage_rt_len(%s)",obj_sv);
                    if(obj_sv!=obj)free(obj_sv);
                    free(obj);free(prop); return out;
                }
                // For unknown/instance types, fall through to field_get
            }
            char* out=malloc(strlen(obj)+strlen(prop)+48);
            sprintf(out,"sage_rt_field_get(%s,\"%s\")",obj,prop);
            free(obj);free(prop); return out;
        }
        case EXPR_SET: {
            if (expr->as.set.object) {
                char* obj=aot_expr(aot,expr->as.set.object,JIT_TYPE_UNKNOWN);
                char* val=aot_expr(aot,expr->as.set.value,JIT_TYPE_UNKNOWN);
                char* prop=aot_escape(expr->as.set.property.start);
                prop[expr->as.set.property.length]='\0';
                // val appears twice in sprintf output
                char* out=malloc(strlen(obj)+strlen(prop)+strlen(val)*2+64);
                sprintf(out,"({sage_rt_field_set(%s,\"%s\",%s);%s;})",obj,prop,val,val);
                free(obj);free(val);free(prop); return out;
            }
            char* name=aot_cname(expr->as.set.property.start,expr->as.set.property.length);
            char* val=aot_expr(aot,expr->as.set.value,JIT_TYPE_UNKNOWN);
            char* out=malloc(strlen(name)+strlen(val)+16);
            sprintf(out,"(%s=%s)",name,val);
            free(name);free(val); return out;
        }

        case EXPR_CALL: {
            int argc=expr->as.call.arg_count;
            // super.method(args) — EXPR_SUPER is the direct callee
            // Parser: super.init(x) → EXPR_CALL(callee=EXPR_SUPER{method=Token}, args)
            if (expr->as.call.callee && expr->as.call.callee->type == EXPR_SUPER) {
                Token meth = expr->as.call.callee->as.super_expr.method;
                char mn[256]; int ml=meth.length<255?meth.length:255;
                memcpy(mn, meth.start, ml); mn[ml]='\0';
                size_t total = 512;
                for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);total+=strlen(a)+8;free(a);}
                char* out = malloc(total); int pos = 0;
                if (aot->current_parent_cname[0]) {
                    char* mn_cn = aot_cname(mn, strlen(mn));
                    if (argc > 0) {
                        char* ab = malloc(total); int ap = sprintf(ab,"(SageValue[]){");
                        for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);ap+=sprintf(ab+ap,"%s%s",i?",":"",a);free(a);}
                        sprintf(ab+ap,"}");
                        pos = sprintf(out,"%s_%s(_self,%d,%s)", aot->current_parent_cname, mn_cn, argc, ab);
                        free(ab);
                    } else {
                        pos = sprintf(out,"%s_%s(_self,0,NULL)", aot->current_parent_cname, mn_cn);
                    }
                    free(mn_cn);
                } else {
                    if (argc > 0) {
                        char* ab = malloc(total); int ap = sprintf(ab,"(SageValue[]){");
                        for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);ap+=sprintf(ab+ap,"%s%s",i?",":"",a);free(a);}
                        sprintf(ab+ap,"}");
                        pos = sprintf(out,"sage_rt_method_call_super(_self,\"%s\",%d,%s)",mn,argc,ab);
                        free(ab);
                    } else pos = sprintf(out,"sage_rt_method_call_super(_self,\"%s\",0,NULL)",mn);
                }
                (void)pos; return out;
            }
            if (expr->as.call.callee && expr->as.call.callee->type==EXPR_VARIABLE) {
                const char* raw=expr->as.call.callee->as.variable.name.start;
                int rawlen=expr->as.call.callee->as.variable.name.length;
                #define BM(s) (rawlen==(int)strlen(s)&&memcmp(raw,s,rawlen)==0)
                if (BM("print")&&argc==1){
                    JitTypeTag _at=aot_infer_expr(aot,expr->as.call.args[0]);
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    int _nb=(expr->as.call.args[0]->type==EXPR_VARIABLE&&jit_is_unboxed(_at));
                    char*_arg=_nb?aot_box(_at,a):a;
                    char*o=malloc(strlen(_arg)+64);
                    sprintf(o,"({sage_rt_print(%s);sage_rt_nil();})",_arg);
                    if(_arg!=a)free(_arg);free(a);return o;
                }
                if (BM("println")&&argc==1){
                    JitTypeTag _at=aot_infer_expr(aot,expr->as.call.args[0]);
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    int _nb=(expr->as.call.args[0]->type==EXPR_VARIABLE&&jit_is_unboxed(_at));
                    char*_arg=_nb?aot_box(_at,a):a;
                    char*o=malloc(strlen(_arg)+64);
                    sprintf(o,"({sage_rt_println(%s);sage_rt_nil();})",_arg);
                    if(_arg!=a)free(_arg);free(a);return o;
                }
                if (BM("len")&&argc==1){
                    JitTypeTag _lat=aot_infer_expr(aot,expr->as.call.args[0]);
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    char*o=malloc(strlen(a)+64);
                    // Tuples need special handling since sage_rt_len returns nil for them
                    if(_lat==JIT_TYPE_TUPLE)
                        snprintf(o,strlen(a)+64,"sage_rt_int(sage_rt_tuple_len(%s))",a);
                    else if(hint==JIT_TYPE_INT)
                        snprintf(o,strlen(a)+64,"sage_rt_len(%s).as.integer",a);
                    else
                        snprintf(o,strlen(a)+64,"sage_rt_len(%s)",a);
                    free(a);return o;
                }
                if (BM("int")&&argc==1){
                    JitTypeTag _at=aot_infer_expr(aot,expr->as.call.args[0]);
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    int _nb=(expr->as.call.args[0]->type==EXPR_VARIABLE&&jit_is_unboxed(_at));
                    char*_arg=_nb?aot_box(_at,a):a;
                    char*o=malloc(strlen(_arg)+64);
                    if(hint==JIT_TYPE_INT)sprintf(o,"sage_rt_int_cast(%s).as.integer",_arg);
                    else sprintf(o,"sage_rt_int_cast(%s)",_arg);
                    if(_arg!=a)free(_arg);free(a);return o;
                }
                if (BM("float")&&argc==1){
                    JitTypeTag _at=aot_infer_expr(aot,expr->as.call.args[0]);
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    int _nb=(expr->as.call.args[0]->type==EXPR_VARIABLE&&jit_is_unboxed(_at));
                    char*_arg=_nb?aot_box(_at,a):a;
                    char*o=malloc(strlen(_arg)+64);
                    if(hint==JIT_TYPE_FLOAT)sprintf(o,"sage_rt_float_cast(%s).as.number",_arg);
                    else sprintf(o,"sage_rt_float_cast(%s)",_arg);
                    if(_arg!=a)free(_arg);free(a);return o;
                }
                if (BM("str")&&argc==1){
                    JitTypeTag at=aot_infer_expr(aot,expr->as.call.args[0]);
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    // Only box bare unboxed variables — literals/expressions already return SageValue
                    int needs_box=(expr->as.call.args[0]->type==EXPR_VARIABLE&&jit_is_unboxed(at));
                    char*arg=needs_box?aot_box(at,a):a;
                    char*o=malloc(strlen(arg)+64);
                    if(hint==JIT_TYPE_STRING) sprintf(o,"sage_rt_str_cast(%s).as.string",arg);
                    else sprintf(o,"sage_rt_str_cast(%s)",arg);
                    if(arg!=a) { free(arg); } free(a); return o;
                }
                if (BM("typeof")&&argc==1){
                    JitTypeTag _at=aot_infer_expr(aot,expr->as.call.args[0]);
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    int _nb=(expr->as.call.args[0]->type==EXPR_VARIABLE&&jit_is_unboxed(_at));
                    char*_arg=_nb?aot_box(_at,a):a;
                    char*o=malloc(strlen(_arg)+32);sprintf(o,"sage_rt_typeof(%s)",_arg);
                    if(_arg!=a)free(_arg);free(a);return o;
                }
                if (BM("bool")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*o=malloc(strlen(a)+32);sprintf(o,"sage_rt_bool_cast(%s)",a);free(a);return o;}
                if (BM("range")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);char*o=malloc(strlen(a)+strlen(b)+32);sprintf(o,"sage_rt_range(%s,%s)",a,b);free(a);free(b);return o;}
                if (BM("range")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*o=malloc(strlen(a)+64);sprintf(o,"sage_rt_range(sage_rt_int(0),%s)",a);free(a);return o;}
                if (BM("range_inc")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);char*o=malloc(strlen(a)+strlen(b)+32);sprintf(o,"sage_rt_range_inc(%s,%s)",a,b);free(a);free(b);return o;}
                if (BM("clock")&&argc==0) return strdup("sage_rt_clock()");
                if (BM("input")){char*a=argc==1?aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN):strdup("sage_rt_nil()");char*o=malloc(strlen(a)+32);sprintf(o,"sage_rt_input(%s)",a);free(a);return o;}
                if (BM("gc_collect")&&argc==0) return strdup("({sage_rt_gc_collect();sage_rt_nil();})");
                if (BM("gc_disable")&&argc==0) return strdup("({sage_rt_gc_disable();sage_rt_nil();})");
                if (BM("gc_enable")&&argc==0)  return strdup("({sage_rt_gc_enable();sage_rt_nil();})");
                if (BM("Some")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*o=malloc(strlen(a)+48);sprintf(o,"sage_rt_some(%s)",a);free(a);return o;}
                if (BM("Ok")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*o=malloc(strlen(a)+48);sprintf(o,"sage_rt_ok(%s)",a);free(a);return o;}
                if (BM("Err")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*o=malloc(strlen(a)+48);sprintf(o,"sage_rt_err(%s)",a);free(a);return o;}
                // -- String/array builtins (bare function form) --
                // join() builtin
                // -- C struct layout builtins --
                if (BM("struct_def")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_struct_def(%s)",a);free(a);return o;}
                if (BM("struct_new")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_struct_new(%s)",a);free(a);return o;}
                if (BM("struct_get")&&argc==3){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);char*c=aot_expr(aot,expr->as.call.args[2],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+strlen(c)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_struct_get(%s,%s,%s)",a,b,c);free(a);free(b);free(c);return o;}
                if (BM("struct_set")&&argc==4){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);char*c=aot_expr(aot,expr->as.call.args[2],JIT_TYPE_UNKNOWN);char*d=aot_expr(aot,expr->as.call.args[3],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+strlen(c)+strlen(d)+64;char*o=malloc(sz);snprintf(o,sz,"({sage_rt_struct_set(%s,%s,%s,%s);sage_rt_nil();})",a,b,c,d);free(a);free(b);free(c);free(d);return o;}
                if (BM("struct_size")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_struct_size(%s)",a);free(a);return o;}
                // -- Manual memory builtins --
                if (BM("mem_alloc")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_mem_alloc(%s)",a);free(a);return o;}
                if (BM("mem_free")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"({sage_rt_mem_free(%s);sage_rt_nil();})",a);free(a);return o;}
                if (BM("mem_read")&&argc==3){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);char*c=aot_expr(aot,expr->as.call.args[2],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+strlen(c)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_mem_read(%s,%s,%s)",a,b,c);free(a);free(b);free(c);return o;}
                if (BM("mem_write")&&argc==4){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);char*c=aot_expr(aot,expr->as.call.args[2],JIT_TYPE_UNKNOWN);char*d=aot_expr(aot,expr->as.call.args[3],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+strlen(c)+strlen(d)+64;char*o=malloc(sz);snprintf(o,sz,"({sage_rt_mem_write(%s,%s,%s,%s);sage_rt_nil();})",a,b,c,d);free(a);free(b);free(c);free(d);return o;}
                if (BM("precision")&&argc==2){
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);
                    JitTypeTag bt=aot_infer_expr(aot,expr->as.call.args[1]);
                    // sage_rt_precision(SageValue, SageValue) - box int if needed
                    int bnb=(expr->as.call.args[1]->type==EXPR_VARIABLE&&jit_is_unboxed(bt));
                    char*bb=bnb?aot_box(bt,b):b;
                    size_t sz=strlen(a)+strlen(bb)+64;char*o=malloc(sz);
                    snprintf(o,sz,"sage_rt_precision(%s,%s)",a,bb);
                    if(bb!=b)free(bb);free(a);free(b);return o;
                }
                if (BM("tonumber")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_tonumber(%s)",a);free(a);return o;}
                if (BM("join")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_array_join(%s,%s)",a,b);free(a);free(b);return o;}
                if (BM("string_join")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_array_join(%s,%s)",a,b);free(a);free(b);return o;}
                // Dict builtins (bare function form)
                if (BM("dict_has")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr_boxed(aot,expr->as.call.args[1]);size_t sz=strlen(a)+strlen(b)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_bool(sage_rt_dict_has(%s,%s))",a,b);free(a);free(b);return o;}
                if (BM("dict_delete")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+64;char*o=malloc(sz);snprintf(o,sz,"({sage_rt_dict_remove(%s,%s);sage_rt_nil();})",a,b);free(a);free(b);return o;}
                if (BM("dict_get")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_dict_get(%s,%s)",a,b);free(a);free(b);return o;}
                if (BM("dict_keys")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_dict_keys(%s)",a);free(a);return o;}
                if (BM("dict_values")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_dict_values(%s)",a);free(a);return o;}
                if (BM("dict_len")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_dict_len(%s)",a);free(a);return o;}
                // Slice/range builtins
                if (BM("slice")&&argc==3){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_INT);char*d=aot_expr(aot,expr->as.call.args[2],JIT_TYPE_INT);size_t sz=strlen(a)+strlen(b)+strlen(d)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_array_slice(%s,(int)(%s),(int)(%s))",a,b,d);free(a);free(b);free(d);return o;}
                if (BM("upper")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_str_upper(%s)",a);free(a);return o;}
                if (BM("lower")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_str_lower(%s)",a);free(a);return o;}
                if (BM("strip")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_str_strip(%s)",a);free(a);return o;}
                if (BM("replace")&&argc==3){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);char*d=aot_expr(aot,expr->as.call.args[2],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+strlen(d)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_str_replace(%s,%s,%s)",a,b,d);free(a);free(b);free(d);return o;}
                if (BM("split")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_str_split(%s,%s)",a,b);free(a);free(b);return o;}
                if (BM("next")&&argc==1){
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    size_t sz=strlen(a)+128;char*o=malloc(sz);
                    snprintf(o,sz,"sage_rt_call_fn(%s,0,NULL)",a);
                    free(a);return o;
                }
                if (BM("push")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr_boxed(aot,expr->as.call.args[1]);size_t sz=strlen(a)+strlen(b)+64;char*o=malloc(sz);snprintf(o,sz,"({sage_rt_array_push(%s,%s);sage_rt_nil();})",a,b);free(a);free(b);return o;}
                if (BM("pop")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_array_pop(%s)",a);free(a);return o;}
                if (BM("abs")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)*3+128;char*o=malloc(sz);snprintf(o,sz,"(SAGE_IS_INT(%s)?sage_rt_int(llabs(%s.as.integer)):sage_rt_float(fabs(%s.as.number)))",a,a,a);free(a);return o;}
                if (BM("min")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+128;char*o=malloc(sz);snprintf(o,sz,"(sage_rt_less(%s,%s)?(%s):(%s))",a,b,a,b);free(a);free(b);return o;}
                if (BM("max")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+128;char*o=malloc(sz);snprintf(o,sz,"(sage_rt_less(%s,%s)?(%s):(%s))",a,b,b,a);free(a);free(b);return o;}
                if (BM("print")&&argc>=1){
                    // multi-arg print with spaces
                    size_t total=64; for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);total+=strlen(a)+32;free(a);}
                    char*o=malloc(total); int pos=sprintf(o,"({");
                    for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);pos+=sprintf(o+pos,"if(%d)fputs(" ",stdout);sage_rt_print(%s);",i,a);free(a);}
                    sprintf(o+pos,"sage_rt_nil();})"); return o;
                }
                #undef BM
                // Only use sage_rt_call_fn for SageValue function vars (not known C procs)
                if (!aot_is_known_proc(aot, raw, rawlen)) {
                    char fname_buf[256]; int flen2=rawlen<255?rawlen:255;
                    memcpy(fname_buf,raw,flen2); fname_buf[flen2]='\0';
                    JitTypeTag cv = aot_get_var_type(aot, fname_buf);
                    // If type is unknown and it's not a known proc, treat as SageValue fn
                    if (cv == JIT_TYPE_UNKNOWN || cv == JIT_TYPE_MIXED) {
                        char* cname = aot_cname(raw, rawlen);
                        size_t total = strlen(cname) + 256;
                        for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);total+=strlen(a)+8;free(a);}
                        char* out = malloc(total);
                        int pos = 0;
                        if(argc > 0){
                            char* argbuf = malloc(total);
                            int ap = sprintf(argbuf,"(SageValue[]){");
                            for(int i=0;i<argc;i++){
                                JitTypeTag at=aot_infer_expr(aot,expr->as.call.args[i]);
                                char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);
                                int nb=(expr->as.call.args[i]->type==EXPR_VARIABLE&&jit_is_unboxed(at));
                                char*ab=nb?aot_box(at,a):a;
                                ap+=sprintf(argbuf+ap,"%s%s",i?",":"",ab);
                                if(ab!=a)free(ab); free(a);
                            }
                            sprintf(argbuf+ap,"}");
                            pos=sprintf(out,"sage_rt_call_fn(%s,%d,%s)",cname,argc,argbuf);
                            free(argbuf);
                        } else {
                            pos=sprintf(out,"sage_rt_call_fn(%s,0,NULL)",cname);
                        }
                        (void)pos; free(cname); return out;
                    }
                }
                // User proc call — use inferred types BUT classes/structs always take SageValue
                // Detect if this is a class/struct constructor by checking known_procs source
                // (class/struct ctors always emit SageValue params, not raw typed ones)
                // Check if this is a class/struct constructor (always use SageValue params)
                int is_ctor = 0;
                {
                    char nbuf[256]; int nl=rawlen<255?rawlen:255;
                    memcpy(nbuf,raw,nl); nbuf[nl]='\0';
                    // Check both raw name ("Point") and cname ("sg_Point")
                    char cname_buf[260]="sg_"; strncat(cname_buf,nbuf,255);
                    for(int _ci=0;_ci<aot->known_ctor_count;_ci++){
                        if(strcmp(aot->known_ctors[_ci],nbuf)==0||
                           strcmp(aot->known_ctors[_ci],cname_buf)==0){ is_ctor=1; break; }
                    }
                }
                char* fname=aot_cname(raw,rawlen);
                // Inside module body: rewrite sg_procname to sg_MODULENAME_sg_procname
                // so recursive/cross-function calls within a module use the correct symbol
                if (aot->in_module_body && aot->current_module_prefix[0]) {
                    char prefixed[256]; snprintf(prefixed,sizeof(prefixed),"%s%s",aot->current_module_prefix,fname+3); // strip sg_ from fname, add full prefix
                    // Only rewrite if this is actually a module function (not a builtin)
                    for(int _ki=0;_ki<aot->known_proc_count;_ki++){
                        if(strcmp(aot->known_procs[_ki],prefixed)==0){
                            free(fname); fname=strdup(prefixed); break;
                        }
                    }
                }
                size_t total=strlen(fname)+32;
                // Count total params including defaulted ones for buffer sizing
                int emit_argc_pre = argc;
                for (int _di=0; _di<aot->proc_default_count; _di++) {
                    if (strcmp(aot->proc_defaults[_di].proc_cname, fname)==0 &&
                        aot->proc_defaults[_di].param_idx >= emit_argc_pre)
                        emit_argc_pre = aot->proc_defaults[_di].param_idx + 1;
                }
                for(int i=0;i<argc;i++){
                    JitTypeTag pt=is_ctor?JIT_TYPE_UNKNOWN:aot_param_type(aot,raw,rawlen,i);
                    JitTypeTag ah=jit_is_unboxed(pt)?pt:JIT_TYPE_UNKNOWN;
                    char*a=aot_expr(aot,expr->as.call.args[i],ah);
                    if(jit_is_unboxed(pt)){
                        JitTypeTag at=aot_infer_expr(aot,expr->as.call.args[i]);
                        if(!jit_is_unboxed(at)){char*bx=aot_box(pt,a);total+=strlen(bx);free(bx);}
                    }
                    total+=strlen(a)+4; free(a);
                }
                // Add space for default values
                for (int i=argc; i<emit_argc_pre; i++) total += 320;
                char* out=malloc(total);
                int pos=sprintf(out,"%s(",fname);
                // Determine total param count (provided + defaulted)
                int emit_argc = emit_argc_pre;
                for(int i=0;i<emit_argc;i++){
                    if (i < argc) {
                        JitTypeTag pt=is_ctor?JIT_TYPE_UNKNOWN:aot_param_type(aot,raw,rawlen,i);
                        JitTypeTag ah=(!is_ctor&&jit_is_unboxed(pt))?pt:JIT_TYPE_UNKNOWN;
                        char*a=aot_expr(aot,expr->as.call.args[i],ah);
                        // Box if passing SageValue to typed param
                        char*fa=a;
                        if(!is_ctor && jit_is_unboxed(pt)){
                            JitTypeTag at=aot_infer_expr(aot,expr->as.call.args[i]);
                            if(!jit_is_unboxed(at)){fa=aot_box(pt,a);}
                        }
                        pos+=sprintf(out+pos,"%s%s",i>0?", ":"",fa);
                        if(fa!=a)free(fa); free(a);
                    } else {
                        // Missing arg — compile default with correct param type hint
                        char* compiled_dflt = NULL;
                        for (int _di=0; _di<aot->proc_default_count; _di++) {
                            if (strcmp(aot->proc_defaults[_di].proc_cname, fname)==0 &&
                                aot->proc_defaults[_di].param_idx == i) {
                                if (aot->proc_defaults[_di].default_ast) {
                                    JitTypeTag pt = is_ctor ? JIT_TYPE_UNKNOWN
                                                           : aot_param_type(aot,raw,rawlen,i);
                                    JitTypeTag ah = jit_is_unboxed(pt)?pt:JIT_TYPE_UNKNOWN;
                                    compiled_dflt = aot_expr(aot, aot->proc_defaults[_di].default_ast, ah);
                                } else {
                                    compiled_dflt = strdup(aot->proc_defaults[_di].default_expr);
                                }
                                break;
                            }
                        }
                        const char* dflt = compiled_dflt ? compiled_dflt : "sage_rt_nil()";
                        pos+=sprintf(out+pos,"%s%s",i>0?", ":"",dflt);
                        if (compiled_dflt) free(compiled_dflt);
                    }
                }
                sprintf(out+pos,")");
                free(fname); return out;
            }
            // -- Method call: obj.method(args) --
            if (expr->as.call.callee && expr->as.call.callee->type == EXPR_GET) {
                Expr* ge = expr->as.call.callee;
                // Handle super.method() calls — parser emits EXPR_SUPER as the direct callee
                if (ge->as.get.object && ge->as.get.object->type == EXPR_SUPER) {
                    char mn[256]; int ml=ge->as.get.property.length<255?ge->as.get.property.length:255;
                    memcpy(mn,ge->as.get.property.start,ml); mn[ml]='\0';
                    size_t total=512;
                    for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);total+=strlen(a)+8;free(a);}
                    char* out=malloc(total); int pos=0;
                    // Static dispatch: call the parent's method directly by C name.
                    // This fixes multi-level inheritance: each level calls its own
                    // statically-resolved parent, so C(B(A)) works without MRO confusion.
                    if (aot->current_parent_cname[0]) {
                        char* mn_cn = aot_cname(mn, strlen(mn));
                        if(argc>0){
                            char* ab=malloc(total); int ap=sprintf(ab,"(SageValue[]){");
                            for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);ap+=sprintf(ab+ap,"%s%s",i?",":"",a);free(a);}
                            sprintf(ab+ap,"}");
                            pos=sprintf(out,"%s_%s(_self,%d,%s)",aot->current_parent_cname,mn_cn,argc,ab);
                            free(ab);
                        } else {
                            pos=sprintf(out,"%s_%s(_self,0,NULL)",aot->current_parent_cname,mn_cn);
                        }
                        free(mn_cn);
                    } else {
                        // No known parent — fall back to runtime lookup
                        if(argc>0){
                            char* ab=malloc(total); int ap=sprintf(ab,"(SageValue[]){");
                            for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);ap+=sprintf(ab+ap,"%s%s",i?",":"",a);free(a);}
                            sprintf(ab+ap,"}");
                            pos=sprintf(out,"sage_rt_method_call_super(_self,\"%s\",%d,%s)",mn,argc,ab);
                            free(ab);
                        } else pos=sprintf(out,"sage_rt_method_call_super(_self,\"%s\",0,NULL)",mn);
                    }
                    (void)pos; return out;
                }
                char* _mobj_raw = aot_expr(aot, ge->as.get.object, JIT_TYPE_UNKNOWN);
                JitTypeTag _mot = aot_infer_expr(aot, ge->as.get.object);
                // --- ADT constructor call: EnumName.VariantName(args) ---
                // Detect when object is a known enum namespace (DICT type from known_enums)
                if (_mot == JIT_TYPE_DICT && ge->as.get.object->type == EXPR_VARIABLE) {
                    char _en_raw[256]={0};
                    int _enl = ge->as.get.object->as.variable.name.length < 255
                             ? ge->as.get.object->as.variable.name.length : 255;
                    memcpy(_en_raw, ge->as.get.object->as.variable.name.start, _enl);
                    _en_raw[_enl] = '\0';
                    int _is_known_enum = 0;
                    for (int _ei=0; _ei<aot->known_enum_count; _ei++) {
                        if (strcmp(aot->known_enums[_ei], _en_raw)==0) { _is_known_enum=1; break; }
                    }
                    if (_is_known_enum) {
                        // Emit direct constructor call: sg_EnumName_sg_VariantName(arg0, arg1, ...)
                        char* _ecn = aot_cname(_en_raw, strlen(_en_raw));
                        char* _vcn = aot_cname(ge->as.get.property.start, ge->as.get.property.length);
                        size_t _adt_sz = strlen(_ecn)+strlen(_vcn)+64;
                        for(int _ai=0;_ai<argc;_ai++){char*_ta=aot_expr(aot,expr->as.call.args[_ai],JIT_TYPE_UNKNOWN);_adt_sz+=strlen(_ta)+8;free(_ta);}
                        char* _adt_out = malloc(_adt_sz);
                        int _adt_pos = snprintf(_adt_out, _adt_sz, "%s_%s(", _ecn, _vcn);
                        for(int _ai=0;_ai<argc;_ai++){
                            char*_ta=aot_expr(aot,expr->as.call.args[_ai],JIT_TYPE_UNKNOWN);
                            _adt_pos+=sprintf(_adt_out+_adt_pos,"%s%s",_ai?", ":"",_ta);
                            free(_ta);
                        }
                        if (argc==0) sprintf(_adt_out+_adt_pos,")");
                        else sprintf(_adt_out+_adt_pos,")");
                        free(_ecn); free(_vcn); free(_mobj_raw);
                        return _adt_out;
                    }
                }
                // ── Module direct call: module.proc(args) → sg_MODULE_sg_PROC(args) ─
                if (_mot == JIT_TYPE_DICT && ge->as.get.object->type == EXPR_VARIABLE) {
                    char _mod_raw[256]={0};
                    int _modl = ge->as.get.object->as.variable.name.length < 255
                              ? ge->as.get.object->as.variable.name.length : 255;
                    memcpy(_mod_raw, ge->as.get.object->as.variable.name.start, _modl);
                    _mod_raw[_modl] = '\0';
                    // Check if this is a known imported module
                    int _is_module = 0;
                    for (int _mi=0; _mi<aot->imported_module_count; _mi++) {
                        if (strcmp(aot->imported_modules[_mi], _mod_raw)==0) { _is_module=1; break; }
                    }
                    if (_is_module) {
                        char* _fn_c = aot_cname(ge->as.get.property.start, ge->as.get.property.length);
                        size_t _msz = strlen(_mod_raw)+strlen(_fn_c)+64;
                        for(int _ai=0;_ai<argc;_ai++){char*_ta=aot_expr(aot,expr->as.call.args[_ai],JIT_TYPE_UNKNOWN);_msz+=strlen(_ta)+8;free(_ta);}
                        char* _mout = malloc(_msz);
                        // Direct C call: sg_MOD_sg_FUNC(arg0, arg1, ...)
                        int _mp = snprintf(_mout, _msz, "sg_%s_%s(", _mod_raw, _fn_c);
                        for(int _ai=0;_ai<argc;_ai++){
                            char*_ta=aot_expr(aot,expr->as.call.args[_ai],JIT_TYPE_UNKNOWN);
                            _mp+=sprintf(_mout+_mp, "%s%s", _ai?",":"", _ta);
                            free(_ta);
                        }
                        sprintf(_mout+_mp, ")");
                        free(_fn_c); free(_mobj_raw);
                        return _mout;
                    }
                }
                // Box raw string/int/float/bool variables — methods always expect SageValue
                char* _mobj = (ge->as.get.object->type == EXPR_VARIABLE && jit_is_unboxed(_mot))
                    ? aot_box(_mot, _mobj_raw) : _mobj_raw;
                char* _mn  = aot_escape(ge->as.get.property.start);
                _mn[ge->as.get.property.length] = '\0';
                /* Use safe buffer: strlen of all parts + 256 headroom */
                #define _MOUT(...) ({ size_t _sz = strlen(_mobj) + 256; __VA_ARGS__; char* _o = malloc(_sz); _o; })
                #define _MO(fmt,...) do { size_t _sz=strlen(_mobj)+256; if(argc>0){char*_ta=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);_sz+=strlen(_ta);free(_ta);} char*_o=malloc(_sz); sprintf(_o, fmt, ## __VA_ARGS__); free(_mobj);free(_mn); return _o; } while(0)
                /* Build a size-safe output buffer for method call results */
                size_t _mbufsz = strlen(_mobj) + 512;
                for(int _mi=0;_mi<argc;_mi++){char*_ta=aot_expr(aot,expr->as.call.args[_mi],JIT_TYPE_UNKNOWN);_mbufsz+=strlen(_ta)+8;free(_ta);}
                #undef _MO
                #define _MO1(fn) do{char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*o=malloc(strlen(_mobj)+strlen(a)+256);sprintf(o,fn"(%s,%s)",_mobj,a);free(_mobj);free(_mn);free(a);return o;}while(0)
                #define _MO0(fn) do{char*o=malloc(strlen(_mobj)+256);sprintf(o,fn"(%s)",_mobj);free(_mobj);free(_mn);return o;}while(0)
                if (!strcmp(_mn,"upper")&&argc==0)      _MO0("sage_rt_str_upper");
                if (!strcmp(_mn,"lower")&&argc==0)      _MO0("sage_rt_str_lower");
                if ((!strcmp(_mn,"strip")||!strcmp(_mn,"trim"))&&argc==0) _MO0("sage_rt_str_strip");
                if (!strcmp(_mn,"split")&&argc==1)      _MO1("sage_rt_str_split");
                if ((!strcmp(_mn,"startswith")||!strcmp(_mn,"starts_with"))&&argc==1) _MO1("sage_rt_str_startswith");
                if ((!strcmp(_mn,"endswith")||!strcmp(_mn,"ends_with"))&&argc==1) _MO1("sage_rt_str_endswith");
                if (!strcmp(_mn,"find")&&argc==1)       _MO1("sage_rt_str_find");
                if (!strcmp(_mn,"pop")&&argc==0)        _MO0("sage_rt_array_pop");
                if (!strcmp(_mn,"join")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t _cs=strlen(_mobj)+strlen(a)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_array_join(%s,%s)",_mobj,a);free(_mobj);free(_mn);free(a);return o;}
                if ((!strcmp(_mn,"index_of")||!strcmp(_mn,"indexOf"))&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t _cs=strlen(_mobj)+strlen(a)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_array_index_of(%s,%s)",_mobj,a);free(_mobj);free(_mn);free(a);return o;}
                if (!strcmp(_mn,"reverse")&&argc==0){size_t _cs=strlen(_mobj)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_array_reverse(%s)",_mobj);free(_mobj);free(_mn);return o;}
                if (!strcmp(_mn,"sort")&&argc==0){size_t _cs=strlen(_mobj)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_array_sort(%s)",_mobj);free(_mobj);free(_mn);return o;}
                if (!strcmp(_mn,"slice")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_INT);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_INT);size_t _cs=strlen(_mobj)+strlen(a)+strlen(b)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_array_slice(%s,(int)(%s),(int)(%s))",_mobj,a,b);free(_mobj);free(_mn);free(a);free(b);return o;}
                if ((!strcmp(_mn,"get_or")||!strcmp(_mn,"get"))&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t _cs=strlen(_mobj)+strlen(a)+strlen(b)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_dict_get_or(%s,%s,%s)",_mobj,a,b);free(_mobj);free(_mn);free(a);free(b);return o;}
                if (!strcmp(_mn,"keys")&&argc==0)       _MO0("sage_rt_dict_keys");
                if (!strcmp(_mn,"values")&&argc==0)     _MO0("sage_rt_dict_values");
                if (!strcmp(_mn,"contains_key")&&argc==1){ char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t _cs=strlen(_mobj)+strlen(a)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_bool(sage_rt_dict_has(%s,%s))",_mobj,a);free(_mobj);free(_mn);free(a);return o;}
                if (!strcmp(_mn,"remove")&&argc==1){ char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t _cs=strlen(_mobj)+strlen(a)+256;char*o=malloc(_cs);snprintf(o,_cs,"({sage_rt_dict_remove(%s,%s);sage_rt_nil();})",_mobj,a);free(_mobj);free(_mn);free(a);return o;}
                if (!strcmp(_mn,"get")&&argc==2){ char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t _cs=strlen(_mobj)+strlen(a)+strlen(b)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_dict_get_or(%s,%s,%s)",_mobj,a,b);free(_mobj);free(_mn);free(a);free(b);return o;}
                if (!strcmp(_mn,"get")&&argc==1){ char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t _cs=strlen(_mobj)+strlen(a)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_dict_get(%s,%s)",_mobj,a);free(_mobj);free(_mn);free(a);return o;}
                if (!strcmp(_mn,"contains")&&argc==1){
                    JitTypeTag _cot=aot_infer_expr(aot,ge->as.get.object);
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    size_t _csz=strlen(_mobj)+strlen(a)+256;
                    char*o=malloc(_csz);
                    if(_cot==JIT_TYPE_ARRAY)
                        snprintf(o,_csz,"sage_rt_array_contains(%s,%s)",_mobj,a);
                    else
                        snprintf(o,_csz,"sage_rt_bool(sage_rt_str_find(%s,%s).as.integer>=0)",_mobj,a);
                    free(_mobj);free(_mn);free(a);return o;
                }
                if (!strcmp(_mn,"replace")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t _cs=strlen(_mobj)+strlen(a)+strlen(b)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_str_replace(%s,%s,%s)",_mobj,a,b);free(_mobj);free(_mn);free(a);free(b);return o;}
                if (!strcmp(_mn,"push")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t _cs=strlen(_mobj)+strlen(a)+256;char*o=malloc(_cs);snprintf(o,_cs,"({sage_rt_array_push(%s,%s);sage_rt_nil();})",_mobj,a);free(_mobj);free(_mn);free(a);return o;}
                if (!strcmp(_mn,"length")&&argc==0){size_t _cs=strlen(_mobj)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_int(sage_rt_str_len(%s))",_mobj);free(_mobj);free(_mn);return o;}
                #undef _MO0
                #undef _MO1
                #undef _MOUT
                /* _mbufsz is a local var, not a macro */
                // Generic method call via runtime
                size_t total=strlen(_mobj)+strlen(_mn)+128;
                for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);total+=strlen(a)+4;free(a);}
                char* out=malloc(total);
                int pos=0;
                if(argc>0){
                    char** aa=malloc(argc*sizeof(char*));
                    size_t al=32;
                    for(int i=0;i<argc;i++){aa[i]=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);al+=strlen(aa[i])+4;}
                    char*argbuf=malloc(al);
                    int ap=sprintf(argbuf,"(SageValue[]){");
                    for(int i=0;i<argc;i++) ap+=sprintf(argbuf+ap,"%s%s",i?",":"",aa[i]);
                    sprintf(argbuf+ap,"}");
                    for(int i=0;i<argc;i++) free(aa[i]); free(aa);
                    pos=sprintf(out,"sage_rt_method_call(%s,\"%s\",%d,%s)",_mobj,_mn,argc,argbuf);
                    free(argbuf);
                } else {
                    pos=sprintf(out,"sage_rt_method_call(%s,\"%s\",0,NULL)",_mobj,_mn);
                }
                (void)pos;
                if(_mobj!=_mobj_raw)free(_mobj_raw); free(_mobj); free(_mn); return out;
            }
            char* callee=aot_expr(aot,expr->as.call.callee,JIT_TYPE_UNKNOWN);
            char* out=malloc(strlen(callee)+64);
            sprintf(out,"sage_rt_nil()/*dyn-call*/");
            free(callee); return out;
        }

        case EXPR_INTERP: {
            // Compile-time string interpolation: scan template, parse each {expr},
            // emit: sage_rt_string_concat(sage_rt_string_concat("literal", tostring(expr)), ...)
            const char* tmpl = expr->as.interp.template_str ? expr->as.interp.template_str : "";
            int tlen = expr->as.interp.template_len;
            if (tlen <= 0) tlen = (int)strlen(tmpl);

            // Build a list of parts: (is_literal, text/expr_src)
            // Then fold them into nested sage_rt_string_concat calls
            #define MAX_PARTS 64
            typedef struct { int is_lit; char* s; } InterpPart;
            InterpPart parts[MAX_PARTS]; int nparts = 0;

            int i = 0;
            while (i < tlen && nparts < MAX_PARTS) {
                if (tmpl[i] == '{' && (i == 0 || tmpl[i-1] != '\\')) {
                    i++; // skip {
                    char expr_src[4096]; int elen = 0; int depth = 1;
                    while (i < tlen && depth > 0) {
                        if (tmpl[i] == '{') depth++;
                        else if (tmpl[i] == '}') { depth--; if (depth == 0) break; }
                        if (elen < (int)sizeof(expr_src)-2) expr_src[elen++] = tmpl[i];
                        i++;
                    }
                    expr_src[elen] = '\0'; i++; // skip }
                    if (elen > 0) {
                        parts[nparts].is_lit = 0;
                        parts[nparts].s = strdup(expr_src);
                        nparts++;
                    }
                } else {
                    // Collect literal segment
                    char lit[4096]; int llen = 0;
                    while (i < tlen && !(tmpl[i] == '{' && (i == 0 || tmpl[i-1] != '\\'))) {
                        if (llen < (int)sizeof(lit)-2) lit[llen++] = tmpl[i];
                        i++;
                    }
                    if (llen > 0) {
                        lit[llen] = '\0';
                        parts[nparts].is_lit = 1;
                        parts[nparts].s = strdup(lit);
                        nparts++;
                    }
                }
            }

            if (nparts == 0) { return strdup("sage_rt_string(\"\")"); }

            // Compile each part to a C expression returning SageValue string
            char* compiled[MAX_PARTS];
            for (int pi = 0; pi < nparts; pi++) {
                if (parts[pi].is_lit) {
                    // Escape the literal and wrap in sage_rt_string
                    char* esc = aot_escape(parts[pi].s);
                    compiled[pi] = malloc(strlen(esc) + 32);
                    sprintf(compiled[pi], "sage_rt_string(\"%s\")", esc);
                    free(esc);
                } else {
                    // Parse the sub-expression and compile it
                    char* snip = malloc(strlen(parts[pi].s) + 4);
                    sprintf(snip, "%s\n", parts[pi].s);
                    LexerState  sl = lexer_get_state();
                    ParserState sp = parser_get_state();
                    init_lexer(snip, "<interp>");
                    parser_init();
                    Expr* sub = parse_expression_public();
                    lexer_set_state(sl);
                    parser_set_state(sp);
                    if (sub) {
                        // Always get a boxed SageValue for the sub-expression.
                        // aot_expr with UNKNOWN hint returns raw scalars for unboxed vars,
                        // so check inferred type and box explicitly if needed.
                        JitTypeTag st = aot_infer_expr(aot, sub);
                        char* sub_c;
                        if (jit_is_unboxed(st)) {
                            // Get raw scalar, then box it
                            char* raw_c = aot_expr(aot, sub, st);
                            sub_c = aot_box(st, raw_c);
                            free(raw_c);
                        } else {
                            sub_c = aot_expr(aot, sub, JIT_TYPE_UNKNOWN);
                        }
                        compiled[pi] = malloc(strlen(sub_c) + 32);
                        sprintf(compiled[pi], "sage_rt_str_cast(%s)", sub_c);
                        free(sub_c);
                    } else {
                        // Parse failed — emit as literal
                        char* esc = aot_escape(parts[pi].s);
                        compiled[pi] = malloc(strlen(esc) + 40);
                        sprintf(compiled[pi], "sage_rt_string(\"{%s}\")", esc);
                        free(esc);
                    }
                    free(snip);
                }
                free(parts[pi].s);
            }

            // Fold: concat(concat(part0, part1), part2) ...
            char* acc = compiled[0];
            for (int pi = 1; pi < nparts; pi++) {
                char* newacc = malloc(strlen(acc) + strlen(compiled[pi]) + 48);
                sprintf(newacc, "sage_rt_string_concat(%s, %s)", acc, compiled[pi]);
                free(acc); free(compiled[pi]);
                acc = newacc;
            }
            return acc;
        }

        case EXPR_AWAIT: {
            // await always produces a SageValue (box unboxed types)
            JitTypeTag at = aot_infer_expr(aot, expr->as.await.expression);
            char* av = aot_expr(aot, expr->as.await.expression, at);
            if (jit_is_unboxed(at)) {
                char* bv = aot_box(at, av); free(av); return bv;
            }
            return av;
        }
        case EXPR_COMPTIME:
            return aot_expr(aot,expr->as.await.expression,hint);
        case EXPR_SUPER:
            return strdup("_self"); // super → current instance in compiled mode
            // Note: super.method() calls are handled in the method call dispatch
        default:
            return strdup("sage_rt_nil()");
    }
}

char* aot_compile_expr(AotCompiler* aot, Expr* expr) {
    return aot_expr(aot, expr, JIT_TYPE_UNKNOWN);
}

static int is_range_for(Stmt* stmt, Expr** lo, Expr** hi, int* inclusive) {
    Expr* iter=stmt->as.for_stmt.iterable;
    if (!iter) return 0;
    if (iter->type==EXPR_RANGE){*lo=iter->as.range.low;*hi=iter->as.range.high;*inclusive=iter->as.range.inclusive;return 1;}
    if (iter->type==EXPR_CALL && iter->as.call.callee && iter->as.call.callee->type==EXPR_VARIABLE) {
        const char* n=iter->as.call.callee->as.variable.name.start;
        int nl=iter->as.call.callee->as.variable.name.length;
        if (nl==5&&memcmp(n,"range",5)==0) {
            if (iter->as.call.arg_count==2){*lo=iter->as.call.args[0];*hi=iter->as.call.args[1];*inclusive=0;return 1;}
            if (iter->as.call.arg_count==1){*lo=NULL;*hi=iter->as.call.args[0];*inclusive=0;return 1;}
        }
        if (nl==8&&memcmp(n,"range_inc",9)==0&&iter->as.call.arg_count==2){*lo=iter->as.call.args[0];*hi=iter->as.call.args[1];*inclusive=1;return 1;}
    }
    return 0;
}

// Returns a C expression guaranteed to be a scalar int (0 or 1) for use in if/while.
// Never returns a SageValue struct — always wraps in sage_rt_truthy when needed.
static char* compile_cond(AotCompiler* aot, Expr* expr) {
    if (!expr) return strdup("0");
    // Native comparison: both operands are unboxed scalars → result is already C int
    if (expr->type == EXPR_BINARY) {
        int op = expr->as.binary.op.type;
        int is_cmp = (op==TOKEN_EQ||op==TOKEN_NEQ||op==TOKEN_GT||op==TOKEN_LT||
                      op==TOKEN_GTE||op==TOKEN_LTE);
        if (is_cmp) {
            JitTypeTag L = aot_infer_expr(aot, expr->as.binary.left);
            JitTypeTag R = aot_infer_expr(aot, expr->as.binary.right);
            if ((L==JIT_TYPE_INT||L==JIT_TYPE_FLOAT||L==JIT_TYPE_BOOL) &&
                (R==JIT_TYPE_INT||R==JIT_TYPE_FLOAT||R==JIT_TYPE_BOOL)) {
                // Both unboxed — binary path returns a native C int
                return aot_expr(aot, expr, JIT_TYPE_BOOL);
            }
        }
    }
    // Bool literal → native
    if (expr->type == EXPR_BOOL) return strdup(expr->as.boolean.value ? "1" : "0");
    // Known bool variable → native
    if (expr->type == EXPR_VARIABLE) {
        char name[256];
        int len = expr->as.variable.name.length<255?expr->as.variable.name.length:255;
        memcpy(name,expr->as.variable.name.start,len); name[len]='\0';
        JitTypeTag vt = aot_get_var_type(aot, name);
        if (vt==JIT_TYPE_BOOL||vt==JIT_TYPE_INT) return aot_expr(aot, expr, vt);
    }
    // Everything else: evaluate as SageValue, test with sage_rt_truthy
    char* raw = aot_expr(aot, expr, JIT_TYPE_UNKNOWN);
    char* out = malloc(strlen(raw)+32);
    sprintf(out, "sage_rt_truthy(%s)", raw);
    free(raw); return out;
}

void aot_compile_stmt(AotCompiler* aot, Stmt* stmt) {
    if (!stmt) return;
    switch (stmt->type) {
        case STMT_PRINT: {
            // aot_expr(..., UNKNOWN) returns a raw C scalar only for bare EXPR_VARIABLE
            // with a known unboxed type. Everything else already comes back as SageValue.
            Expr* _pe = stmt->as.print.expression;
            JitTypeTag pt = aot_infer_expr(aot, _pe);
            char* val = aot_expr(aot, _pe, JIT_TYPE_UNKNOWN);
            if (jit_is_unboxed(pt) && _pe && _pe->type == EXPR_VARIABLE) {
                char* boxed = aot_box(pt, val);
                aot_emit(aot, "sage_rt_println(%s);", boxed);
                free(boxed);
            } else {
                aot_emit(aot, "sage_rt_println(%s);", val);
            }
            free(val); break;
        }
        case STMT_LET: {
            char* name=aot_cname_tok(stmt->as.let.name);
            if (stmt->as.let.initializer) {
                // Inside a coroutine body all variables must be SageValue — params arrive
                // from _co->argv[] as SageValue, so specialisation would cause type mismatches.
                JitTypeTag t = aot->in_coro_body ? JIT_TYPE_UNKNOWN
                                                 : aot_infer_expr(aot, stmt->as.let.initializer);
                if (jit_is_unboxed(t)) {
                    char* val=aot_expr(aot,stmt->as.let.initializer,t);
                    aot_emit(aot,"%s %s = %s;",jit_ctype(t),name,val); free(val);
                } else if (t == JIT_TYPE_STRUCT &&
                           stmt->as.let.initializer->type == EXPR_VARIABLE) {
                    // Struct copy on assign — value semantics
                    char* val=aot_expr(aot,stmt->as.let.initializer,JIT_TYPE_UNKNOWN);
                    aot_emit(aot,"SageValue %s = sage_rt_struct_copy(%s);",name,val); free(val);
                } else {
                    char* val=aot_expr(aot,stmt->as.let.initializer,JIT_TYPE_UNKNOWN);
                    aot_emit(aot,"SageValue %s = %s;",name,val); free(val);
                }
            } else aot_emit(aot,"SageValue %s = sage_rt_nil();",name);
            free(name); break;
        }
        case STMT_EXPRESSION: {
            Expr* e = stmt->as.expression;
            // Optimise: var = expr where var is a known unboxed type
            // (suppressed inside coroutine bodies — all vars are SageValue there)
            if (!aot->in_coro_body && e && e->type == EXPR_SET && e->as.set.object == NULL) {
                char name[256];
                int len = e->as.set.property.length < 255 ? e->as.set.property.length : 255;
                memcpy(name, e->as.set.property.start, len); name[len] = '\0';
                JitTypeTag vt = aot_get_var_type(aot, name);
                if (jit_is_unboxed(vt)) {
                    char* lhs = aot_cname(e->as.set.property.start, e->as.set.property.length);
                    JitTypeTag rhs_t = aot_infer_expr(aot, e->as.set.value);
                    char* rhs = aot_expr(aot, e->as.set.value, vt);
                    // If RHS is a method call or complex expr returning SageValue,
                    // extract the raw C value from it
                    // rhs_is_raw: aot_expr returned a raw C scalar (not SageValue)
                    // This is true when:
                    //   - unboxed variable (sg_x where x is int64_t/double/etc.)
                    //   - literal with matching typed hint (returns raw C value)
                    //   - binary op between same unboxed types (returns raw C value)
                    int rhs_is_raw = (
                        (e->as.set.value->type == EXPR_VARIABLE && jit_is_unboxed(rhs_t)) ||
                        (e->as.set.value->type == EXPR_INT   && vt == JIT_TYPE_INT)   ||
                        (e->as.set.value->type == EXPR_NUMBER&& vt == JIT_TYPE_FLOAT) ||
                        (e->as.set.value->type == EXPR_BOOL  && vt == JIT_TYPE_BOOL)  ||
                        (e->as.set.value->type == EXPR_STRING&& vt == JIT_TYPE_STRING)||
                        (e->as.set.value->type == EXPR_BINARY && jit_is_unboxed(rhs_t) && rhs_t == vt)
                    );
                    if (rhs_is_raw) {
                        aot_emit(aot, "%s = %s;", lhs, rhs);
                    } else {
                        // RHS returns SageValue — extract the right field
                        char* extract = malloc(strlen(rhs) + 64);
                        switch(vt) {
                            case JIT_TYPE_INT:
                                sprintf(extract, "(%s).as.integer", rhs); break;
                            case JIT_TYPE_FLOAT:
                                sprintf(extract, "(%s).as.number", rhs); break;
                            case JIT_TYPE_BOOL:
                                sprintf(extract, "(%s).as.boolean", rhs); break;
                            case JIT_TYPE_STRING:
                                sprintf(extract, "(%s).as.string", rhs); break;
                            default:
                                sprintf(extract, "%s", rhs); break;
                        }
                        aot_emit(aot, "%s = %s;", lhs, extract);
                        free(extract);
                    }
                    free(lhs); free(rhs); break;
                }
            }
            // Also handle += -= *= /= on unboxed vars
            char* val=aot_expr(aot,stmt->as.expression,JIT_TYPE_UNKNOWN);
            aot_emit(aot,"(void)(%s);",val); free(val); break;
        }
        case STMT_IF: {
            char* cond = compile_cond(aot, stmt->as.if_stmt.condition);
            aot_emit(aot,"if (%s) {",cond); free(cond);
            aot->indent++;
            for(Stmt*s=stmt->as.if_stmt.then_branch;s;s=s->next) aot_compile_stmt(aot,s);
            aot->indent--;
            if (stmt->as.if_stmt.else_branch) {
                aot_emit(aot,"} else {"); aot->indent++;
                for(Stmt*s=stmt->as.if_stmt.else_branch;s;s=s->next) aot_compile_stmt(aot,s);
                aot->indent--;
            }
            aot_emit(aot,"}"); break;
        }
        case STMT_WHILE: {
            char* cond = compile_cond(aot, stmt->as.while_stmt.condition);
            aot_emit(aot,"while (%s) {",cond); free(cond);
            aot->indent++;
            for(Stmt*s=stmt->as.while_stmt.body;s;s=s->next) aot_compile_stmt(aot,s);
            aot->indent--;
            aot_emit(aot,"}"); break;
        }
        case STMT_FOR: {
            char* var=aot_cname_tok(stmt->as.for_stmt.variable);
            Expr*lo=NULL,*hi=NULL; int inc=0;
            if (is_range_for(stmt,&lo,&hi,&inc)) {
                // Register loop var as INT so body can use it unboxed
                { char name[256]; int len=stmt->as.for_stmt.variable.length<255?stmt->as.for_stmt.variable.length:255;
                  memcpy(name,stmt->as.for_stmt.variable.start,len);name[len]='\0';
                  aot_set_var_type(aot,name,JIT_TYPE_INT); }
                char* lo_c=lo?aot_expr(aot,lo,JIT_TYPE_INT):strdup("INT64_C(0)");
                char* hi_c=aot_expr(aot,hi,JIT_TYPE_INT);
                const char* cmp=inc?"<=":"<";
                aot_emit(aot,"for (int64_t %s = %s; %s %s %s; %s++) {",var,lo_c,var,cmp,hi_c,var);
                free(lo_c); free(hi_c);
                aot->indent++;
                for(Stmt*s=stmt->as.for_stmt.body;s;s=s->next) aot_compile_stmt(aot,s);
                aot->indent--;
                aot_emit(aot,"}");
            } else {
                char* iter=aot_temp(aot); char* idx=aot_temp(aot);
                char* itv=aot_expr(aot,stmt->as.for_stmt.iterable,JIT_TYPE_UNKNOWN);
                aot_emit(aot,"{"); aot->indent++;
                aot_emit(aot,"SageValue %s = %s;",iter,itv);
                aot_emit(aot,"for (int64_t %s = 0; %s < sage_rt_array_len(%s); %s++) {",idx,idx,iter,idx);
                aot->indent++;
                aot_emit(aot,"SageValue %s = sage_rt_array_get(%s, sage_rt_int(%s));",var,iter,idx);
                for(Stmt*s=stmt->as.for_stmt.body;s;s=s->next) aot_compile_stmt(aot,s);
                aot->indent--;
                aot_emit(aot,"}"); aot->indent--;
                aot_emit(aot,"}");
                free(itv); free(iter); free(idx);
            }
            free(var); break;
        }
        case STMT_RETURN: {
            // Emit defers (LIFO) before returning
            for(int _di=aot->defer_count-1;_di>=0;_di--){
                aot_emit(aot,"{ /* defer */"); aot->indent++;
                for(Stmt*_ds=aot->defer_stack[_di]->as.defer.statement;_ds;_ds=_ds->next)
                    aot_compile_stmt(aot,_ds);
                aot->indent--; aot_emit(aot,"}");
            }
            if (stmt->as.ret.value) {
                JitTypeTag rt = aot_infer_expr(aot, stmt->as.ret.value);
                char* val = aot_expr(aot, stmt->as.ret.value, JIT_TYPE_UNKNOWN);
                if (jit_is_unboxed(rt) && stmt->as.ret.value->type == EXPR_VARIABLE) {
                    char* boxed = aot_box(rt, val);
                    aot_emit(aot, "return %s;", boxed);
                    free(boxed);
                } else {
                    aot_emit(aot, "return %s;", val);
                }
                free(val);
            } else aot_emit(aot,"return sage_rt_nil();");
            break;
        }
        case STMT_BREAK:    aot_emit(aot,"break;");    break;
        case STMT_CONTINUE: aot_emit(aot,"continue;"); break;
        case STMT_BLOCK:
            aot_emit(aot,"{"); aot->indent++;
            for(Stmt*s=stmt->as.block.statements;s;s=s->next) aot_compile_stmt(aot,s);
            aot->indent--; aot_emit(aot,"}"); break;
        case STMT_ANNOTATED_BLOCK: {
            BlockAnnotation ann=stmt->as.annotated_block.annotation;
            if (ann==BLOCK_ANNOT_MANUAL||ann==BLOCK_ANNOT_TRUSTED)
                aot_emit(aot,"sage_rt_gc_pause(); /* @%s */",ann==BLOCK_ANNOT_MANUAL?"manual":"trusted");
            aot_emit(aot,"{"); aot->indent++;
            for(Stmt*s=stmt->as.annotated_block.statements;s;s=s->next) aot_compile_stmt(aot,s);
            aot->indent--; aot_emit(aot,"}");
            if (ann==BLOCK_ANNOT_MANUAL||ann==BLOCK_ANNOT_TRUSTED) aot_emit(aot,"sage_rt_gc_resume();");
            break;
        }
        case STMT_MATCH: {
            JitTypeTag vt=aot_infer_expr(aot,stmt->as.match_stmt.value);
            /* Evaluate match value */
            // For INT match, use INT hint so literals come back as raw int64_t
            char* _mval_raw=aot_expr(aot,stmt->as.match_stmt.value,
                vt==JIT_TYPE_INT?JIT_TYPE_INT:JIT_TYPE_UNKNOWN);
            char* val;
            if (stmt->as.match_stmt.value->type==EXPR_VARIABLE&&jit_is_unboxed(vt)&&vt!=JIT_TYPE_INT)
                val=aot_box(vt,_mval_raw);
            else
                val=_mval_raw;
            char* tmp=aot_temp(aot);
            aot_emit(aot,"{ /* match */"); aot->indent++;
            if (vt==JIT_TYPE_INT) {
                aot_emit(aot,"int64_t %s = %s;",tmp,_mval_raw);
                // Check if any case is a range or wildcard — if so, use if-else not switch
                int has_range=0;
                for(int i=0;i<stmt->as.match_stmt.case_count&&!has_range;i++){
                    CaseClause*mc=stmt->as.match_stmt.cases[i];
                    if(!mc->pattern) has_range=1;
                    else if(mc->pattern->type==EXPR_RANGE) has_range=1;
                    else if(mc->pattern->type==EXPR_VARIABLE) has_range=1; // wildcard
                }
                if(!has_range){
                    // Pure integer switch
                    aot_emit(aot,"switch (%s) {",tmp); aot->indent++;
                    for(int i=0;i<stmt->as.match_stmt.case_count;i++){
                        CaseClause*mc=stmt->as.match_stmt.cases[i];
                        char*pat=aot_expr(aot,mc->pattern,JIT_TYPE_INT);
                        aot_emit(aot,"case %s: {",pat); free(pat);
                        aot->indent++;
                        for(Stmt*s=mc->body;s;s=s->next) aot_compile_stmt(aot,s);
                        aot_emit(aot,"break;"); aot->indent--; aot_emit(aot,"}");
                    }
                    if(stmt->as.match_stmt.default_case){
                        aot_emit(aot,"default: {"); aot->indent++;
                        for(Stmt*s=stmt->as.match_stmt.default_case;s;s=s->next) aot_compile_stmt(aot,s);
                        aot_emit(aot,"break;"); aot->indent--; aot_emit(aot,"}");
                    }
                    aot->indent--; aot_emit(aot,"}"); // close switch
                } else {
                    // If-else chain (handles ranges, wildcards, mixed, guards)
                    for(int i=0;i<stmt->as.match_stmt.case_count;i++){
                        CaseClause*mc=stmt->as.match_stmt.cases[i];
                        const char*kw=(i==0)?"if":"} else if";
                        // Check for guard expression (case n if cond =>)
                        if(mc->guard){
                            // Bind the pattern variable if it's a name pattern
                            char*gcond=aot_expr(aot,mc->guard,JIT_TYPE_UNKNOWN);
                            aot_emit(aot,"%s (sage_rt_truthy(%s)) {",kw,gcond);
                            free(gcond); aot->indent++;
                            // Bind pattern variable to match target
                            if(mc->pattern && mc->pattern->type==EXPR_VARIABLE){
                                char*pn=aot_cname_tok(mc->pattern->as.variable.name);
                                aot_emit(aot,"int64_t %s=%s;",pn,tmp); free(pn);
                            }
                        } else if(!mc->pattern || (mc->pattern->type==EXPR_VARIABLE &&
                                mc->pattern->as.variable.name.length==1 &&
                                mc->pattern->as.variable.name.start[0]=='_')){
                            aot_emit(aot,"%s",i==0?"{":"} else {"); aot->indent++;
                        } else if(mc->pattern->type==EXPR_RANGE){
                            char*ls=aot_expr(aot,mc->pattern->as.range.low,JIT_TYPE_INT);
                            char*hs=aot_expr(aot,mc->pattern->as.range.high,JIT_TYPE_INT);
                            aot_emit(aot,"%s (%s>=%s&&%s<=%s) {",kw,tmp,ls,tmp,hs);
                            free(ls);free(hs); aot->indent++;
                        } else if(mc->pattern->type==EXPR_VARIABLE){
                            // Bare variable pattern: bind and match all
                            char*pn=aot_cname_tok(mc->pattern->as.variable.name);
                            aot_emit(aot,"%s (1) { int64_t %s=%s;",kw,pn,tmp);
                            free(pn); aot->indent++;
                        } else {
                            char*pat=aot_expr(aot,mc->pattern,JIT_TYPE_INT);
                            aot_emit(aot,"%s (%s==%s) {",kw,tmp,pat);
                            free(pat); aot->indent++;
                        }
                        for(Stmt*s=mc->body;s;s=s->next) aot_compile_stmt(aot,s);
                        aot->indent--;
                    }
                    if(stmt->as.match_stmt.default_case){
                        aot_emit(aot,"%s",stmt->as.match_stmt.case_count>0?"} else {":"{"); aot->indent++;
                        for(Stmt*s=stmt->as.match_stmt.default_case;s;s=s->next) aot_compile_stmt(aot,s);
                        aot->indent--;
                    }
                    if(stmt->as.match_stmt.case_count>0||stmt->as.match_stmt.default_case)
                        aot_emit(aot,"}");
                }
            } else {
                aot_emit(aot,"SageValue %s = %s;",tmp,val);
                for(int i=0;i<stmt->as.match_stmt.case_count;i++){
                    CaseClause*c=stmt->as.match_stmt.cases[i];
                    if (!c->pattern) {
                        // Default/wildcard: treat as else
                        aot_emit(aot,"%s",i==0?"{":"} else {"); aot->indent++;
                        for(Stmt*s=c->body;s;s=s->next) aot_compile_stmt(aot,s);
                        aot->indent--;
                        continue;
                    }
                    if (!c->pattern) {
                        // skip: handled below
                        const char*kw2=i==0?"{":"} else {";
                        aot_emit(aot,"%s",kw2); aot->indent++;
                        for(Stmt*s=c->body;s;s=s->next) aot_compile_stmt(aot,s);
                        aot->indent--; continue;
                    }
                    if(c->guard){
                        char*gcond2=aot_expr(aot,c->guard,JIT_TYPE_UNKNOWN);
                        const char*kw2=i==0?"if":"} else if";
                        aot_emit(aot,"%s (sage_rt_truthy(%s)) {",kw2,gcond2);
                        free(gcond2); aot->indent++;
                        for(Stmt*s=c->body;s;s=s->next) aot_compile_stmt(aot,s);
                        aot->indent--; continue;
                    }
                    // ── ADT pattern: Some(v), Ok(v), Err(e) ─────────────────────────────
                    if (c->pattern && c->pattern->type == EXPR_CALL &&
                        c->pattern->as.call.callee &&
                        c->pattern->as.call.callee->type == EXPR_VARIABLE &&
                        c->pattern->as.call.arg_count >= 1) {
                        const char* _fn = c->pattern->as.call.callee->as.variable.name.start;
                        int _fl = c->pattern->as.call.callee->as.variable.name.length;
                        #define _OPTM(s) (_fl==(int)strlen(s)&&memcmp(_fn,s,_fl)==0)
                        if (_OPTM("Some")||_OPTM("Ok")||_OPTM("Err")) {
                            const char* _type_name = _OPTM("Some")?"Some":_OPTM("Ok")?"Ok":"Err";
                            const char* kw = i==0?"if":"} else if";
                            aot_emit(aot,"%s (SAGE_IS_DICT(%s) && sage_rt_equal(sage_rt_dict_get(%s,sage_rt_string(\"__type\")),sage_rt_string(\"%s\"))) {",
                                     kw, tmp, tmp, _type_name);
                            aot->indent++;
                            // Bind the inner value to each binding variable
                            for (int _bi=0; _bi<c->pattern->as.call.arg_count; _bi++) {
                                Expr* _barg = c->pattern->as.call.args[_bi];
                                if (_barg && _barg->type == EXPR_VARIABLE) {
                                    char* _bname = aot_cname_tok(_barg->as.variable.name);
                                    aot_emit(aot,"SageValue %s = sage_rt_dict_get(%s, sage_rt_string(\"value\"));",
                                             _bname, tmp);
                                    free(_bname);
                                }
                            }
                            for(Stmt*s=c->body;s;s=s->next) aot_compile_stmt(aot,s);
                            aot->indent--; continue;
                        }
                        #undef _OPTM
                    }
                    // ── ADT pattern: Enum.Variant(field1, field2, ...) ──────────────────
                    if (c->pattern && c->pattern->type == EXPR_CALL &&
                        c->pattern->as.call.callee &&
                        c->pattern->as.call.callee->type == EXPR_GET) {
                        Expr* _ge2 = c->pattern->as.call.callee;
                        if (_ge2->as.get.object && _ge2->as.get.object->type == EXPR_VARIABLE) {
                            char _en2[64]={0};
                            int _enl2 = _ge2->as.get.object->as.variable.name.length<63
                                      ? _ge2->as.get.object->as.variable.name.length:63;
                            memcpy(_en2, _ge2->as.get.object->as.variable.name.start, _enl2);
                            _en2[_enl2]='\0';
                            char _vn2[64]={0};
                            int _vnl2 = _ge2->as.get.property.length<63
                                      ? _ge2->as.get.property.length:63;
                            memcpy(_vn2, _ge2->as.get.property.start, _vnl2);
                            _vn2[_vnl2]='\0';
                            // Look up field names from registry
                            int _slot = -1;
                            for (int _ri=0; _ri<aot->adt_variant_count; _ri++) {
                                if (strcmp(aot->adt_variants[_ri].enum_raw,_en2)==0 &&
                                    strcmp(aot->adt_variants[_ri].variant_raw,_vn2)==0) {
                                    _slot=_ri; break;
                                }
                            }
                            const char* kw = i==0?"if":"} else if";
                            aot_emit(aot,"%s (SAGE_IS_DICT(%s) && sage_rt_equal(sage_rt_dict_get(%s,sage_rt_string(\"__tag\")),sage_rt_string(\"%s\"))) {",
                                     kw, tmp, tmp, _vn2);
                            aot->indent++;
                            // Bind each pattern arg to the corresponding field
                            int _nbinds = c->pattern->as.call.arg_count;
                            for (int _bi=0; _bi<_nbinds; _bi++) {
                                Expr* _barg = c->pattern->as.call.args[_bi];
                                if (!_barg || _barg->type != EXPR_VARIABLE) continue;
                                char* _bname = aot_cname_tok(_barg->as.variable.name);
                                const char* _fname = (_slot>=0 && _bi<aot->adt_variants[_slot].field_count)
                                    ? aot->adt_variants[_slot].field_names[_bi] : NULL;
                                if (_fname && _fname[0]) {
                                    aot_emit(aot,"SageValue %s = sage_rt_dict_get(%s, sage_rt_string(\"%s\"));",
                                             _bname, tmp, _fname);
                                } else {
                                    // Fallback: positional lookup not possible without registry
                                    aot_emit(aot,"SageValue %s = sage_rt_nil(); /* field %d not in registry */",
                                             _bname, _bi);
                                }
                                free(_bname);
                            }
                            for(Stmt*s=c->body;s;s=s->next) aot_compile_stmt(aot,s);
                            aot->indent--; continue;
                        }
                    }
                    // ── ADT unit variant: Enum.Variant (no args, EXPR_GET pattern) ──────
                    if (c->pattern && c->pattern->type == EXPR_GET &&
                        c->pattern->as.get.object &&
                        c->pattern->as.get.object->type == EXPR_VARIABLE) {
                        char _en3[64]={0};
                        int _enl3=c->pattern->as.get.object->as.variable.name.length<63
                                 ?c->pattern->as.get.object->as.variable.name.length:63;
                        memcpy(_en3,c->pattern->as.get.object->as.variable.name.start,_enl3);
                        _en3[_enl3]='\0';
                        int _is_enum3=0;
                        for(int _ei=0;_ei<aot->known_enum_count;_ei++){
                            if(strcmp(aot->known_enums[_ei],_en3)==0){_is_enum3=1;break;}
                        }
                        if (_is_enum3) {
                            char _vn3[64]={0};
                            int _vnl3=c->pattern->as.get.property.length<63
                                     ?c->pattern->as.get.property.length:63;
                            memcpy(_vn3,c->pattern->as.get.property.start,_vnl3);
                            _vn3[_vnl3]='\0';
                            const char* kw = i==0?"if":"} else if";
                            aot_emit(aot,"%s (SAGE_IS_DICT(%s) && sage_rt_equal(sage_rt_dict_get(%s,sage_rt_string(\"__tag\")),sage_rt_string(\"%s\"))) {",
                                     kw, tmp, tmp, _vn3);
                            aot->indent++;
                            for(Stmt*s=c->body;s;s=s->next) aot_compile_stmt(aot,s);
                            aot->indent--; continue;
                        }
                    }
                    char*pat=aot_expr(aot,c->pattern,JIT_TYPE_UNKNOWN);
                    const char*kw=i==0?"if":"} else if";
                    // Wildcard variable "_" → always-true condition
                    if (c->pattern->type==EXPR_VARIABLE &&
                        c->pattern->as.variable.name.length==1 &&
                        c->pattern->as.variable.name.start[0]=='_') {
                        aot_emit(aot,"%s (1) {",kw); free(pat); aot->indent++;
                        for(Stmt*s=c->body;s;s=s->next) aot_compile_stmt(aot,s);
                        aot->indent--;
                        continue;
                    }
                    if (c->pattern&&c->pattern->type==EXPR_STRING){
                        char*esc=aot_escape(c->pattern->as.string.value);
                        aot_emit(aot,"%s (SAGE_IS_STRING(%s)&&strcmp(%s.as.string,\"%s\")==0) {",kw,tmp,tmp,esc);
                        free(esc);
                    } else aot_emit(aot,"%s (sage_rt_equal(%s,%s)) {",kw,tmp,pat);
                    free(pat); aot->indent++;
                    for(Stmt*s=c->body;s;s=s->next) aot_compile_stmt(aot,s);
                    aot->indent--;
                }
                if (stmt->as.match_stmt.default_case){
                    aot_emit(aot,"%s",stmt->as.match_stmt.case_count>0?"} else {":"{");
                    aot->indent++;
                    for(Stmt*s=stmt->as.match_stmt.default_case;s;s=s->next) aot_compile_stmt(aot,s);
                    aot->indent--;
                }
                if (stmt->as.match_stmt.case_count>0||stmt->as.match_stmt.default_case) aot_emit(aot,"}");
            }
            aot->indent--; aot_emit(aot,"} /* end match */");
            if(val!=_mval_raw) free(_mval_raw); free(val); free(tmp); break;
        }
        case STMT_TRY: {
            char* frame=aot_temp(aot);
            aot_emit(aot,"{ SageExcFrame %s;",frame); aot->indent++;
            aot_emit(aot,"%s.prev=sage_rt_exc_top; %s.active=1; sage_rt_exc_top=&%s;",frame,frame,frame);
            aot_emit(aot,"if (setjmp(%s.jb)==0) {",frame); aot->indent++;
            for(Stmt*s=stmt->as.try_stmt.try_block;s;s=s->next) aot_compile_stmt(aot,s);
            aot->indent--; aot_emit(aot,"}");
            // Catch block runs when exception was raised (active==0 after raise)
            if (stmt->as.try_stmt.catch_count>0&&stmt->as.try_stmt.catches){
                CatchClause*cc=stmt->as.try_stmt.catches[0];
                char*excvar=aot_cname_tok(cc->exception_var);
                aot_emit(aot,"if (!%s.active) {",frame); aot->indent++;
                aot_emit(aot,"sage_rt_exc_top=%s.prev;",frame);
                // Store exc in a block-scoped var to survive optimization
                aot_emit(aot,"{ SageValue %s=%s.exc;",excvar,frame);
                for(Stmt*s=cc->body;s;s=s->next) aot_compile_stmt(aot,s);
                aot_emit(aot,"}");
                aot->indent--; aot_emit(aot,"} else { sage_rt_exc_top=%s.prev; }",frame);
                free(excvar);
            } else {
                aot_emit(aot,"sage_rt_exc_top=%s.prev;",frame);
            }
            // Finally block always runs after catch
            if (stmt->as.try_stmt.finally_block){
                aot_emit(aot,"{ /* finally */"); aot->indent++;
                for(Stmt*s=stmt->as.try_stmt.finally_block;s;s=s->next) aot_compile_stmt(aot,s);
                aot->indent--; aot_emit(aot,"}");
            }
            aot->indent--; aot_emit(aot,"}"); free(frame); break;
        }
        case STMT_RAISE: {
            char* val=aot_expr(aot,stmt->as.raise.exception,JIT_TYPE_UNKNOWN);
            aot_emit(aot,"sage_rt_raise(%s);",val); free(val); break;
        }
        case STMT_DEFER:
            // Push onto defer stack (LIFO — emitted before each return and at proc end)
            if (aot->defer_count < 64)
                aot->defer_stack[aot->defer_count++] = stmt;
            break;
        case STMT_YIELD:
            if (stmt->as.yield_stmt.value){
                JitTypeTag yt=aot_infer_expr(aot,stmt->as.yield_stmt.value);
                char*v=aot_expr(aot,stmt->as.yield_stmt.value,JIT_TYPE_UNKNOWN);
                int nb=(stmt->as.yield_stmt.value->type==EXPR_VARIABLE&&jit_is_unboxed(yt));
                char*bv=nb?aot_box(yt,v):v;
                if (aot->in_coro_body)
                    aot_emit(aot,"sage_rt_coro_yield(%s, %s);",aot->coro_var,bv);
                else
                    aot_emit(aot,"return %s; /* yield */",bv);
                if(bv!=v)free(bv); free(v);
            } else {
                if (aot->in_coro_body)
                    aot_emit(aot,"sage_rt_coro_yield(%s, sage_rt_nil());",aot->coro_var);
                else
                    aot_emit(aot,"return sage_rt_nil(); /* yield */");
            }
            break;
        case STMT_STRUCT: {
            StructStmt*ss=&stmt->as.struct_stmt;
            char* sname=aot_cname_tok(ss->name);
            char rawname[256]; int nl=ss->name.length<255?ss->name.length:255;
            memcpy(rawname,ss->name.start,nl); rawname[nl]='\0';
            // Global classval so impl blocks can find it
            aot_emit(aot,"static SageValue _%s_classval;",sname);
            // Constructor
            aot_emit(aot,"static SageValue %s(",sname);
            aot->indent++;
            for(int i=0;i<ss->field_count;i++){char*fn=aot_cname_tok(ss->field_names[i]);aot_emit(aot,"SageValue %s%s",fn,i<ss->field_count-1?",":"");free(fn);}
            if(ss->field_count==0) aot_emit(aot,"void");
            aot->indent--;
            aot_emit(aot,") {"); aot->indent++;
            aot_emit(aot,"SageValue _inst = sage_rt_instance_new(_%s_classval);",sname);
            for(int i=0;i<ss->field_count;i++){char*fn=aot_cname_tok(ss->field_names[i]);char esc[64];int el=ss->field_names[i].length<63?ss->field_names[i].length:63;memcpy(esc,ss->field_names[i].start,el);esc[el]='\0';aot_emit(aot,"sage_rt_field_set(_inst,\"%s\",%s);",esc,fn);free(fn);}
            aot_emit(aot,"return _inst;");
            aot->indent--; aot_emit(aot,"}"); aot_blank(aot); free(sname); break;
        }
        case STMT_ENUM: {
            EnumStmt*es=&stmt->as.enum_stmt;
            char*ename=aot_cname_tok(es->name);
            aot_emit(aot,"/* enum %.*s */",es->name.length,es->name.start);
            for(int i=0;i<es->variant_count;i++){
                char*vname=aot_cname_tok(es->variant_names[i]);
                int has_fields=(es->variant_field_counts&&es->variant_field_counts[i]>0);
                if (!has_fields){
                    aot_emit(aot,"static inline SageValue %s_%s(void){",ename,vname);
                    aot->indent++;
                    aot_emit(aot,"static SageValue _v={.type=SAGE_VAL_NIL};static int _init=0;");
                    aot_emit(aot,"if(!_init){_v=sage_rt_dict_new();sage_rt_dict_set(_v,sage_rt_string(\"__tag\"),sage_rt_string(\"%.*s\"));_init=1;}",es->variant_names[i].length,es->variant_names[i].start);
                    aot_emit(aot,"return _v;");
                    aot->indent--; aot_emit(aot,"}");
                } else {
                    int nf=es->variant_field_counts[i];
                    aot_emit(aot,"static SageValue %s_%s(",ename,vname);
                    aot->indent++;
                    for(int f=0;f<nf;f++){char*fn=aot_cname_tok(es->variant_fields[i][f]);aot_emit(aot,"SageValue %s%s",fn,f<nf-1?",":"");free(fn);}
                    aot->indent--;
                    aot_emit(aot,"){"); aot->indent++;
                    aot_emit(aot,"SageValue _v=sage_rt_dict_new();");
                    aot_emit(aot,"sage_rt_dict_set(_v,sage_rt_string(\"__tag\"),sage_rt_string(\"%.*s\"));",es->variant_names[i].length,es->variant_names[i].start);
                    for(int f=0;f<nf;f++){char*fn=aot_cname_tok(es->variant_fields[i][f]);char*esc=aot_escape(es->variant_fields[i][f].start);esc[es->variant_fields[i][f].length]='\0';aot_emit(aot,"sage_rt_dict_set(_v,sage_rt_string(\"%s\"),%s);",esc,fn);free(fn);free(esc);}
                    aot_emit(aot,"return _v;"); aot->indent--; aot_emit(aot,"}");
                }
                free(vname);
            }
            // Emit enum namespace variable: sg_Color = dict{"Red": sg_Color_sg_Red(), ...}
            aot_emit(aot,"static SageValue %s;",ename);
            aot_blank(aot); free(ename); break;
        }
        case STMT_CLASS: {
            ClassStmt*cs=&stmt->as.class_stmt;
            char*cname=aot_cname_tok(cs->name);
            // Count methods and fields
            int mcount=0;
            for(Stmt*m=cs->methods;m;m=m->next) if(m->type==STMT_PROC) mcount++;
            // Set super-dispatch context so method bodies can resolve super calls statically
            snprintf(aot->current_class_cname, sizeof(aot->current_class_cname), "%s", cname);
            if (cs->has_parent && cs->parent.length > 0) {
                char* pcn = aot_cname_tok(cs->parent);
                snprintf(aot->current_parent_cname, sizeof(aot->current_parent_cname), "%s", pcn);
                free(pcn);
            } else {
                aot->current_parent_cname[0] = '\0';
            }
            // Emit method implementations
            for(Stmt*m=cs->methods;m;m=m->next){
                if(m->type!=STMT_PROC) continue;
                char*mname=aot_cname_tok(m->as.proc.name);
                aot_emit(aot,"static SageValue %s_%s(SageInst* _self, int _argc, SageValue* _argv) {",cname,mname);
                aot->indent++;
                // Bind self
                aot_emit(aot,"SageValue sg_self; sg_self.type=SAGE_VAL_INSTANCE; sg_self.as.instance=_self;");
                // Bind params (skip 'self')
                int pi=0;
                for(int i=0;i<m->as.proc.param_count;i++){
                    if(m->as.proc.params[i].length==4&&memcmp(m->as.proc.params[i].start,"self",4)==0) continue;
                    char*pn=aot_cname_tok(m->as.proc.params[i]);
                    aot_emit(aot,"SageValue %s=(_argc>%d)?_argv[%d]:sage_rt_nil();",pn,pi,pi);
                    free(pn); pi++;
                }
                aot_infer_body(aot,m->as.proc.body);
                for(Stmt*bs=m->as.proc.body;bs;bs=bs->next) aot_compile_stmt(aot,bs);
                aot_emit(aot,"return sage_rt_nil();"); aot->indent--; aot_emit(aot,"}");
                free(mname);
            }
            // Emit method table
            aot_emit(aot,"static SageMethod _%s_methods[] = {",cname);
            aot->indent++;
            for(Stmt*m=cs->methods;m;m=m->next){
                if(m->type!=STMT_PROC) continue;
                // Get the raw method name (not cname-mangled for the string key)
                char mraw[256]; int ml=m->as.proc.name.length<255?m->as.proc.name.length:255;
                memcpy(mraw,m->as.proc.name.start,ml); mraw[ml]='\0';
                char*mname=aot_cname_tok(m->as.proc.name);
                aot_emit(aot,"{\"%s\", %s_%s},",mraw,cname,mname);
                free(mname);
            }
            aot->indent--; aot_emit(aot,"};");
            // Emit class definition (registered at startup)
            char rawname[256]; int nl=cs->name.length<255?cs->name.length:255;
            memcpy(rawname,cs->name.start,nl); rawname[nl]='\0';
            aot_emit(aot,"static SageClass _%s_class = { \"%s\", NULL, _%s_methods, %d, NULL, 0, 0 };",
                     cname,rawname,cname,mcount);
            aot_emit(aot,"static SageValue _%s_classval;",cname);
            // Count init params (excluding 'self') and find if init exists
            int init_params = 0;
            int has_init = 0;
            char init_cname[128]="";
            for(Stmt*m=cs->methods;m;m=m->next){
                if(m->type==STMT_PROC && m->as.proc.name.length==4 &&
                   memcmp(m->as.proc.name.start,"init",4)==0){
                    has_init=1;
                    char*mn=aot_cname_tok(m->as.proc.name);
                    snprintf(init_cname,sizeof(init_cname),"%s_%s",cname,mn);
                    free(mn);
                    for(int i=0;i<m->as.proc.param_count;i++){
                        if(m->as.proc.params[i].length==4&&memcmp(m->as.proc.params[i].start,"self",4)==0) continue;
                        init_params++;
                    }
                    break;
                }
            }
            // Count call-site args to get max params needed (covers inherited inits)
            int call_max = init_params;
            {   // scan all call sites for this class constructor
                char craw[256]; int crl=cs->name.length<255?cs->name.length:255;
                memcpy(craw,cs->name.start,crl); craw[crl]='\0';
                // check aot_param_type at indices beyond init_params
                for(int ci=init_params;ci<8;ci++){
                    JitTypeTag pt=aot_param_type(aot,cs->name.start,cs->name.length,ci);
                    if(pt==JIT_TYPE_UNKNOWN&&ci>call_max) break;
                    if(pt!=JIT_TYPE_UNKNOWN) call_max=ci+1;
                }
            }
            int ctor_params = call_max > init_params ? call_max : init_params;
            // Emit constructor
            aot_emit(aot,"static SageValue %s(",cname);
            for(int i=0;i<ctor_params;i++){
                char pbuf[32]; snprintf(pbuf,sizeof(pbuf),"SageValue _a%d%s",i,i<ctor_params-1?",":"");
                aot_emit_raw(aot, pbuf);
            }
            if(ctor_params==0) aot_emit_raw(aot,"void");
            aot_emit_raw(aot,") {\n");
            aot->indent++;
            aot_emit(aot,"SageValue _inst = sage_rt_instance_new(_%s_classval);",cname);
            if(has_init){
                if(ctor_params>0){
                    char call_buf[512]; int cpos=0;
                    cpos+=snprintf(call_buf+cpos,sizeof(call_buf)-cpos,
                        "{ SageValue _iargs[%d]={",ctor_params);
                    for(int i=0;i<ctor_params;i++)
                        cpos+=snprintf(call_buf+cpos,sizeof(call_buf)-cpos,"%s_a%d",i?",":"",i);
                    cpos+=snprintf(call_buf+cpos,sizeof(call_buf)-cpos,
                        "}; %s(_inst.as.instance,%d,_iargs); }",init_cname,ctor_params);
                    aot_emit(aot,"%s",call_buf);
                } else {
                    aot_emit(aot,"%s(_inst.as.instance,0,NULL);",init_cname);
                }
            } else if(ctor_params>0 && cs->has_parent && cs->parent.length>0){
                // No local init but has parent — call parent init with args
                char parent_cname[128];
                char* pcn = aot_cname_tok(cs->parent);
                snprintf(parent_cname, sizeof(parent_cname), "%s_sg_init", pcn);
                free(pcn);
                char call_buf[512]; int cpos=0;
                cpos+=snprintf(call_buf+cpos,sizeof(call_buf)-cpos,
                    "if (%s != NULL) { SageValue _piargs[%d]={",parent_cname,ctor_params);
                for(int i=0;i<ctor_params;i++)
                    cpos+=snprintf(call_buf+cpos,sizeof(call_buf)-cpos,"%s_a%d",i?",":"",i);
                cpos+=snprintf(call_buf+cpos,sizeof(call_buf)-cpos,
                    "}; %s(_inst.as.instance,%d,_piargs); }",parent_cname,ctor_params);
                // Use a simpler approach: just call it unconditionally
                char call_buf2[512];
                snprintf(call_buf2,sizeof(call_buf2),
                    "{ SageValue _piargs[%d]={",ctor_params);
                char* p2 = call_buf2+strlen(call_buf2);
                for(int i=0;i<ctor_params;i++)
                    p2+=sprintf(p2,"%s_a%d",i?",":"",i);
                p2+=sprintf(p2,"}; %s(_inst.as.instance,%d,_piargs); }",
                    parent_cname,ctor_params);
                aot_emit(aot,"%s",call_buf2);
            }
            aot_emit(aot,"return _inst;");
            aot->indent--; aot_emit(aot,"}");
            aot_blank(aot);
            // Clear super-dispatch context
            aot->current_class_cname[0] = '\0';
            aot->current_parent_cname[0] = '\0';
            free(cname); break;
        }
        case STMT_IMPL: {
            ImplStmt*is=&stmt->as.impl_stmt;
            char*tname=aot_cname_tok(is->target);
            // Count methods
            int impl_mc=0;
            for(Stmt*m=is->methods;m;m=m->next) if(m->type==STMT_PROC) impl_mc++;
            for(Stmt*m=is->methods;m;m=m->next){
                if(m->type!=STMT_PROC) continue;
                char*mname=aot_cname_tok(m->as.proc.name);
                aot_emit(aot,"static SageValue %s_%s(SageInst* _self, int _argc, SageValue* _argv) {",tname,mname);
                aot->indent++;
                aot_emit(aot,"SageValue sg_self; sg_self.type=SAGE_VAL_INSTANCE; sg_self.as.instance=_self;");
                int _mpi=0;
                for(int i=0;i<m->as.proc.param_count;i++){
                    if(m->as.proc.params[i].length==4&&memcmp(m->as.proc.params[i].start,"self",4)==0) continue;
                    char*pn=aot_cname_tok(m->as.proc.params[i]);
                    aot_emit(aot,"SageValue %s=(_argc>%d)?_argv[%d]:sage_rt_nil();",pn,_mpi,_mpi);
                    free(pn); _mpi++;
                }
                aot_infer_body(aot,m->as.proc.body);
                for(Stmt*bs=m->as.proc.body;bs;bs=bs->next) aot_compile_stmt(aot,bs);
                aot_emit(aot,"return sage_rt_nil();"); aot->indent--; aot_emit(aot,"}"); free(mname);
            }
            // Emit method table for impl
            if(impl_mc>0){
                aot_emit(aot,"static SageMethod _%s_impl_methods[] = {",tname);
                aot->indent++;
                for(Stmt*m=is->methods;m;m=m->next){
                    if(m->type!=STMT_PROC) continue;
                    char mraw[256]; int ml=m->as.proc.name.length<255?m->as.proc.name.length:255;
                    memcpy(mraw,m->as.proc.name.start,ml); mraw[ml]='\0';
                    char*mname=aot_cname_tok(m->as.proc.name);
                    aot_emit(aot,"{\"%s\",%s_%s},",mraw,tname,mname);
                    free(mname);
                }
                aot->indent--; aot_emit(aot,"};");
            }
            free(tname); aot_blank(aot); break;
        }
        case STMT_IMPORT: {
            const char* mname = stmt->as.import.module_name;
            if (!mname) break;
            // Do not recursively process imports inside a module body
            if (aot->in_module_body) break;
            // Skip if already processed (imports are pre-compiled at file scope)
            int already2 = 0;
            for (int i=0; i<aot->imported_module_count; i++)
                if (strcmp(aot->imported_modules[i], mname)==0) { already2=1; break; }
            if (already2) break;
            // Already imported?
            int already = 0;
            for (int i=0; i<aot->imported_module_count; i++)
                if (strcmp(aot->imported_modules[i], mname)==0) { already=1; break; }
            if (already) break;
            // Record
            if (aot->imported_module_count < 64)
                snprintf(aot->imported_modules[aot->imported_module_count++], 128, "%s", mname);

            // Resolve module path via global_module_cache
            char* path = resolve_module_path(global_module_cache, mname);
            if (!path) {
                aot_emit(aot, "/* import %s: not found, skipping */", mname);
                break;
            }
            char* source = read_file(path);
            free(path);
            if (!source) { aot_emit(aot,"/* import %s: read failed */",mname); break; }

            // Parse module source
            LexerState sl = lexer_get_state();
            ParserState sp = parser_get_state();
            init_lexer(source, mname);
            parser_init();
            // parse_program parses ALL statements and links them — parse() only returns one
            extern Stmt* parse_program(const char* source, const char* input_path);
            Stmt* mod_ast = parse_program(source, mname);
            lexer_set_state(sl);
            parser_set_state(sp);
            // NOTE: do NOT free(source) here — token .start pointers reference it!
            // It will be freed after all emission is complete.
            if (!mod_ast) { free(source); aot_emit(aot,"/* import %s: parse failed */",mname); break; }

            // Set module prefix for name-mangling
            char saved_prefix[128];
            snprintf(saved_prefix, sizeof(saved_prefix), "%s", aot->current_module_prefix);
            // Build prefix: "arrays" -> "sg_arrays_"
            char mod_prefix[128]; snprintf(mod_prefix, sizeof(mod_prefix), "sg_%s_", mname);
            snprintf(aot->current_module_prefix, sizeof(aot->current_module_prefix), "%s", mod_prefix);
            aot->in_module_body = 1;

            // Type-infer module body in an isolated type env snapshot
            // (prevent outer scope variable types from contaminating module functions)
            int saved_type_count_mod = aot->type_env.count;
            aot_infer_types(aot, mod_ast);

            // Emit a section comment
            aot_emit(aot, "/* ── module %s ─────────────────────────── */", mname);

            // Emit all top-level procs, classes, structs, enums from the module
            // Each gets the module prefix prepended to its C name
            for (Stmt* ms = mod_ast; ms; ms = ms->next) {
                if (ms->type == STMT_PROC || ms->type == STMT_ASYNC_PROC) {
                    // Reset type env to pre-module state before each proc
                    // so one proc's variable types don't contaminate the next
                    int saved_for_proc = aot->type_env.count;
                    aot->type_env.count = saved_type_count_mod;
                    // Only emit as module proc (global-scoped)
                    aot_emit_proc(aot, ms);
                    aot->type_env.count = saved_for_proc;
                    // Also emit a SageNativeFn wrapper for dict registration
                    ProcStmt* ps = (ms->type==STMT_PROC)?&ms->as.proc:&ms->as.async_proc;
                    char* pn_c   = aot_cname_tok(ps->name);
                    int np = ps->param_count;
                    char mcn2[256]; snprintf(mcn2,sizeof(mcn2),"sg_%s",mname);
                    char wrap_name[256]; snprintf(wrap_name,sizeof(wrap_name),"_mwrap_%s_%s",mcn2,pn_c);
                    aot_emit(aot,"static SageValue %s(int _argc, SageValue* _argv, void* _env) {",wrap_name);
                    aot->indent++;
                    aot_emit(aot,"(void)_env;");
                    // Call the prefixed function with args
                    char arglist[512]; int ap=0;
                    for (int pi=0; pi<np; pi++)
                        ap+=snprintf(arglist+ap, sizeof(arglist)-ap, "%s(_argc>%d?_argv[%d]:sage_rt_nil())",
                                     pi?",":"", pi, pi);
                    arglist[ap]='\0';
                    aot_emit(aot,"return %s_%s(%s);", mcn2, pn_c, arglist);
                    aot->indent--; aot_emit(aot,"}");
                    // Register the unregistered C name (sg_flatten) AND the prefixed name
                    // (sg_arrays_sg_flatten) so recursive self-calls inside the module work
                    aot_register_proc(aot, pn_c);                    // sg_flatten
                    char full_pfx[256]; snprintf(full_pfx,sizeof(full_pfx),"%s_%s",mcn2,pn_c);
                    aot_register_proc(aot, full_pfx);                // sg_arrays_sg_flatten
                    // Register in mod_procs for main() dict population
                    if (aot->mod_proc_count < 512) {
                        char pn_raw[64]; int prl=ps->name.length<63?ps->name.length:63;
                        memcpy(pn_raw,ps->name.start,prl); pn_raw[prl]='\0';
                        snprintf(aot->mod_procs[aot->mod_proc_count].mod_cname, 64, "%s", mcn2);
                        snprintf(aot->mod_procs[aot->mod_proc_count].proc_raw,  64, "%s", pn_raw);
                        snprintf(aot->mod_procs[aot->mod_proc_count].wrap_cname,128, "%s", wrap_name);
                        aot->mod_proc_count++;
                    }
                    free(pn_c);
                } else if (ms->type == STMT_CLASS || ms->type == STMT_STRUCT ||
                           ms->type == STMT_ENUM  || ms->type == STMT_IMPL) {
                    // Skip nested imports inside module body — they are interpreter-only
                    aot_compile_stmt(aot, ms);
                } else if (ms->type == STMT_LET) {
                    // Module-level variable — export into dict via inline expression in main()
                    if (aot->mod_proc_count < 512) {
                        char vn[64]; int prl=ms->as.let.name.length<63?ms->as.let.name.length:63;
                        memcpy(vn,ms->as.let.name.start,prl); vn[prl]='\0';
                        char mcn2[64]; snprintf(mcn2,sizeof(mcn2),"sg_%s",mname);
                        char* val = ms->as.let.initializer
                            ? aot_expr(aot, ms->as.let.initializer, JIT_TYPE_UNKNOWN)
                            : strdup("sage_rt_nil()");
                        // wrap_cname = "@@" + c_expression (eval'd in main())
                        snprintf(aot->mod_procs[aot->mod_proc_count].mod_cname, 64, "%s", mcn2);
                        snprintf(aot->mod_procs[aot->mod_proc_count].proc_raw,  64, "%s", vn);
                        snprintf(aot->mod_procs[aot->mod_proc_count].wrap_cname,256, "@@%s", val);
                        aot->mod_proc_count++;
                        free(val);
                    }
                }
                // Skip everything else (STMT_IMPORT, bare statements, etc.)
            }

            // Declare the module namespace variable
            char* mcn = aot_cname(mname, strlen(mname));
            aot_emit(aot, "static SageValue %s; /* module %s namespace */", mcn, mname);
            aot_blank(aot);

            // Register module var type as DICT (for EXPR_GET d.key dispatch)
            aot_set_var_type(aot, mname, JIT_TYPE_DICT);
            // Also register the module var as known (for direct-call detection)
            // We save the type before restoring count so it persists in outer scope
            int mod_var_idx = -1;
            for (int _mi=0; _mi<aot->type_env.count; _mi++) {
                if (strcmp(aot->type_env.vars[_mi].name, mname)==0) { mod_var_idx=_mi; break; }
            }

            free(mcn);
            // Now safe to free the module source — all tokens have been processed
            free(source);
            // Restore type env to pre-module state (module vars don't pollute outer scope)
            aot->type_env.count = saved_type_count_mod;
            // Re-register the module namespace variable as DICT in the outer scope
            aot_set_var_type(aot, mname, JIT_TYPE_DICT);
            // Restore prefix and module-body flag
            snprintf(aot->current_module_prefix, sizeof(aot->current_module_prefix), "%s", saved_prefix);
            aot->in_module_body = 0;
            break;
        }
        case STMT_SPAWN:  aot_emit(aot,"/* spawn — needs libpthread */"); break;
        case STMT_COMPTIME: for(Stmt*s=stmt->as.comptime.body;s;s=s->next) aot_compile_stmt(aot,s); break;
        case STMT_PROC: case STMT_ASYNC_PROC: {
            // Nested proc: emit capture struct + make_fn
            ProcStmt* ps=(stmt->type==STMT_PROC)?&stmt->as.proc:&stmt->as.async_proc;
            char* pname=aot_cname_tok(ps->name);
            char wname[128]; snprintf(wname,sizeof(wname),"_sw_%s",pname);
            char sname[140]; snprintf(sname,sizeof(sname),"_cap_%s",pname);
            // Collect captures for this nested proc
            const char* caps[32]; int ncaps=0;
            _collect_free_vars_stmt(ps->body, caps, &ncaps, 32, ps);
            if(ncaps>0){
                // Allocate capture struct and fill it
                aot_emit(aot,"%s* _env_%s = (%s*)malloc(sizeof(%s));",sname,pname,sname,sname);
                for(int i=0;i<ncaps;i++){
                    // caps[i] is a null-terminated name
                    int cl=(int)strlen(caps[i]);
                    char* cn=aot_cname(caps[i],cl);
                    JitTypeTag ct=aot_get_var_type(aot,(char*)caps[i]);
                    if(jit_is_unboxed(ct)){
                        char* bx=aot_box(ct,cn);
                        aot_emit(aot,"_env_%s->fields[%d]=%s;",pname,i,bx);
                        free(bx);
                    } else {
                        aot_emit(aot,"_env_%s->fields[%d]=%s;",pname,i,cn);
                    }
                    free(cn);
                }
                aot_emit(aot,"SageValue %s=sage_rt_make_fn((SageNativeFn)%s,_env_%s,\"%.*s\");",
                    pname,wname,pname,ps->name.length,ps->name.start);
            } else {
                aot_emit(aot,"SageValue %s=sage_rt_make_fn((SageNativeFn)%s,NULL,\"%.*s\");",
                    pname,wname,ps->name.length,ps->name.start);
            }
            free(pname);
            break;
        }
        case STMT_TRAIT: case STMT_MACRO_DEF: break;
        default: aot_emit(aot,"/* unhandled stmt %d */",stmt->type); break;
    }
}


// ── Call-site type analysis ───────────────────────────────────────────────────
// Scans AST for all calls to a named function, collects arg types.
// Stores inferred param types as "procname#paramN" in type_env.

// Scan expr tree for calls to fname and record arg types
static void aot_scan_expr_calls(AotCompiler* aot, const char* fname, int flen, Expr* e) {
    if (!e) return;
    if (e->type == EXPR_CALL) {
        if (e->as.call.callee && e->as.call.callee->type == EXPR_VARIABLE) {
            const char* cn = e->as.call.callee->as.variable.name.start;
            int cl = e->as.call.callee->as.variable.name.length;
            if (cl == flen && memcmp(cn, fname, flen) == 0) {
                for (int i = 0; i < e->as.call.arg_count; i++) {
                    JitTypeTag t = aot_infer_expr(aot, e->as.call.args[i]);
                    if (t != JIT_TYPE_UNKNOWN) {
                        char key[280];
                        snprintf(key, sizeof(key), "%.*s#%d", flen, fname, i);
                        // Only set if not already set (first call wins)
                        if (aot_get_var_type(aot, key) == JIT_TYPE_UNKNOWN)
                            aot_set_var_type(aot, key, t);
                    }
                }
            }
        }
        // Scan args recursively
        for (int i = 0; i < e->as.call.arg_count; i++)
            aot_scan_expr_calls(aot, fname, flen, e->as.call.args[i]);
        aot_scan_expr_calls(aot, fname, flen, e->as.call.callee);
    } else {
        // Recurse into sub-expressions
        switch (e->type) {
            case EXPR_BINARY:
                aot_scan_expr_calls(aot, fname, flen, e->as.binary.left);
                aot_scan_expr_calls(aot, fname, flen, e->as.binary.right);
                break;
            case EXPR_GET:
                aot_scan_expr_calls(aot, fname, flen, e->as.get.object);
                break;
            case EXPR_SET:
                aot_scan_expr_calls(aot, fname, flen, e->as.set.object);
                aot_scan_expr_calls(aot, fname, flen, e->as.set.value);
                break;
            case EXPR_INDEX:
                aot_scan_expr_calls(aot, fname, flen, e->as.index.array);
                aot_scan_expr_calls(aot, fname, flen, e->as.index.index);
                break;
            default: break;
        }
    }
}

static void aot_scan_stmt_calls(AotCompiler* aot, const char* fname, int flen, Stmt* s);

static void aot_collect_calls(AotCompiler* aot, const char* fname, int flen,
                               Stmt* program) {
    aot_scan_stmt_calls(aot, fname, flen, program);
}

static void aot_scan_stmt_calls(AotCompiler* aot, const char* fname, int flen, Stmt* s) {
    for (; s; s = s->next) {
        Expr* e = NULL;
        switch (s->type) {
            case STMT_EXPRESSION: e = s->as.expression; break;
            case STMT_LET:        e = s->as.let.initializer; break;
            case STMT_PRINT:      e = s->as.print.expression; break;
            case STMT_RETURN:     e = s->as.ret.value; break;
            case STMT_IF:
                aot_scan_expr_calls(aot, fname, flen, s->as.if_stmt.condition);
                aot_scan_stmt_calls(aot, fname, flen, s->as.if_stmt.then_branch);
                aot_scan_stmt_calls(aot, fname, flen, s->as.if_stmt.else_branch);
                break;
            case STMT_WHILE:
                aot_scan_expr_calls(aot, fname, flen, s->as.while_stmt.condition);
                aot_scan_stmt_calls(aot, fname, flen, s->as.while_stmt.body);
                break;
            case STMT_FOR:
                aot_scan_stmt_calls(aot, fname, flen, s->as.for_stmt.body);
                break;
            case STMT_BLOCK:
                aot_scan_stmt_calls(aot, fname, flen, s->as.block.statements);
                break;
            case STMT_PROC: case STMT_ASYNC_PROC: {
                ProcStmt* ps=(s->type==STMT_PROC)?&s->as.proc:&s->as.async_proc;
                aot_scan_stmt_calls(aot, fname, flen, ps->body);
                break;
            }
            default: break;
        }
        if (e) aot_scan_expr_calls(aot, fname, flen, e);
    }
}
static JitTypeTag aot_param_type(AotCompiler* aot, const char* fname, int flen, int idx) {
    char key[280];
    snprintf(key, sizeof(key), "%.*s#%d", flen, fname, idx);
    return aot_get_var_type(aot, key);
}


static void aot_emit_nested_procs(AotCompiler* aot, Stmt* body);

// (forward decl)
// (forward decl moved to top of section)
static void _collect_free_vars(Expr* e, const char** caps, int* ncaps, int maxcaps, ProcStmt* ps) {
    if (!e) return;
    if (e->type == EXPR_VARIABLE) {
        int is_param = 0;
        for(int i=0;i<ps->param_count;i++){
            if(e->as.variable.name.length==ps->params[i].length &&
               memcmp(e->as.variable.name.start,ps->params[i].start,ps->params[i].length)==0){
                is_param=1; break;
            }
        }
        if(!is_param && *ncaps < maxcaps){
            int nl=(int)e->as.variable.name.length;
            char* namecopy=(char*)malloc(nl+1);
            memcpy(namecopy,e->as.variable.name.start,nl); namecopy[nl]='\0';
            int dup=0;
            for(int i=0;i<*ncaps;i++){
                if(strcmp(caps[i],namecopy)==0){ dup=1; break; }
            }
            if(!dup) caps[(*ncaps)++] = namecopy;
            else free(namecopy);
        }
        return;
    }
    switch(e->type){
        case EXPR_BINARY:
            _collect_free_vars(e->as.binary.left,caps,ncaps,maxcaps,ps);
            _collect_free_vars(e->as.binary.right,caps,ncaps,maxcaps,ps); break;
        case EXPR_CALL:
            _collect_free_vars(e->as.call.callee,caps,ncaps,maxcaps,ps);
            for(int i=0;i<e->as.call.arg_count;i++)
                _collect_free_vars(e->as.call.args[i],caps,ncaps,maxcaps,ps);
            break;
        case EXPR_GET: _collect_free_vars(e->as.get.object,caps,ncaps,maxcaps,ps); break;
        case EXPR_SET:
            _collect_free_vars(e->as.set.object,caps,ncaps,maxcaps,ps);
            _collect_free_vars(e->as.set.value,caps,ncaps,maxcaps,ps); break;
        default: break;
    }
}

static void _collect_free_vars_stmt(Stmt* s, const char** caps, int* ncaps, int maxcaps, ProcStmt* ps) {
    for(;s;s=s->next){
        Expr* e=NULL;
        switch(s->type){
            case STMT_EXPRESSION: e=s->as.expression; break;
            case STMT_LET: e=s->as.let.initializer; break;
            case STMT_RETURN: e=s->as.ret.value; break;
            case STMT_IF:
                _collect_free_vars(s->as.if_stmt.condition,caps,ncaps,maxcaps,ps);
                _collect_free_vars_stmt(s->as.if_stmt.then_branch,caps,ncaps,maxcaps,ps);
                _collect_free_vars_stmt(s->as.if_stmt.else_branch,caps,ncaps,maxcaps,ps);
                break;
            case STMT_WHILE:
                _collect_free_vars(s->as.while_stmt.condition,caps,ncaps,maxcaps,ps);
                _collect_free_vars_stmt(s->as.while_stmt.body,caps,ncaps,maxcaps,ps);
                break;
            case STMT_BLOCK:
                _collect_free_vars_stmt(s->as.block.statements,caps,ncaps,maxcaps,ps);
                break;
            case STMT_FOR:
                _collect_free_vars_stmt(s->as.for_stmt.body,caps,ncaps,maxcaps,ps);
                break;
            default: break;
        }
        if(e) _collect_free_vars(e,caps,ncaps,maxcaps,ps);
    }
}

static void aot_emit_one_nested_proc(AotCompiler* aot, Stmt* s) {
    ProcStmt* ps = (s->type==STMT_PROC)?&s->as.proc:&s->as.async_proc;
    char* pname = aot_cname_tok(ps->name);
    char wname[128]; snprintf(wname, sizeof(wname), "_sw_%s", pname);
    // Emit all nested procs within this proc first (recursive)
    aot_emit_nested_procs(aot, ps->body);
    // Collect free variables (captures)
    const char* caps[32]; int ncaps=0;
    _collect_free_vars_stmt(ps->body, caps, &ncaps, 32, ps);
    // Emit capture struct type
    char sname[140]; snprintf(sname,sizeof(sname),"_cap_%s",pname);
    if(ncaps>0){
        aot_emit(aot,"typedef struct { SageValue fields[%d]; } %s;",ncaps,sname);
        // Emit capture field name comments for debugging
        for(int i=0;i<ncaps;i++){
            char capname[256]; int cl=(int)strnlen(caps[i],255);
            memcpy(capname,caps[i],cl); capname[cl]='\0';
            aot_emit(aot,"// cap[%d] = %s",i,capname);
        }
    }
    // Emit this proc as a file-scope SageNativeFn wrapper
    aot_emit(aot, "static SageValue %s(int _argc, SageValue* _argv, void* _env) {", wname);
    aot->indent++;
    // Bind params
    for(int i = 0; i < ps->param_count; i++) {
        char* pn = aot_cname_tok(ps->params[i]);
        aot_emit(aot, "SageValue %s = (_argc > %d) ? _argv[%d] : sage_rt_nil();", pn, i, i);
        free(pn);
    }
    // Bind captures: use direct struct field access (always non-null when ncaps>0)
    if(ncaps>0){
        aot_emit(aot,"%s* _caps = (%s*)_env;",sname,sname);
        for(int i=0;i<ncaps;i++){
            int cl=(int)strlen(caps[i]);
            char* cn=aot_cname(caps[i],cl);
            // #define maps the captured name directly to the struct field
            // This makes assignments write-through to the shared struct
            aot_emit(aot,"#define %s (_caps->fields[%d])",cn,i);
            free(cn);
        }
    }
    aot_infer_body(aot, ps->body);
    for(Stmt* bs = ps->body; bs; bs = bs->next) aot_compile_stmt(aot, bs);
    // No explicit writeback needed - #define makes assignments write directly to struct
    // Undefine the macros to avoid polluting global scope
    if(ncaps>0){
        for(int i=0;i<ncaps;i++){
            int cl=(int)strlen(caps[i]);
            char* cn=aot_cname(caps[i],cl);
            aot_emit(aot,"#undef %s",cn);
            free(cn);
        }
    }
    aot_emit(aot, "return sage_rt_nil();");
    aot->indent--; aot_emit(aot, "}");
    free(pname);
}

static void aot_emit_nested_procs(AotCompiler* aot, Stmt* body) {
    for (Stmt* s = body; s; s = s->next) {
        if (s->type == STMT_PROC || s->type == STMT_ASYNC_PROC)
            aot_emit_one_nested_proc(aot, s);
        else if (s->type == STMT_BLOCK)
            aot_emit_nested_procs(aot, s->as.block.statements);
        else if (s->type == STMT_IF) {
            aot_emit_nested_procs(aot, s->as.if_stmt.then_branch);
            if (s->as.if_stmt.else_branch) aot_emit_nested_procs(aot, s->as.if_stmt.else_branch);
        }
        else if (s->type == STMT_WHILE) aot_emit_nested_procs(aot, s->as.while_stmt.body);
        else if (s->type == STMT_FOR)   aot_emit_nested_procs(aot, s->as.for_stmt.body);
    }
}


// Check if a body contains any yield statements (generator detection)
static int _has_yield(Stmt* body) {
    for (Stmt* s = body; s; s = s->next) {
        if (s->type == STMT_YIELD) return 1;
        if (s->type == STMT_BLOCK && _has_yield(s->as.block.statements)) return 1;
        if (s->type == STMT_IF) {
            if (_has_yield(s->as.if_stmt.then_branch)) return 1;
            if (_has_yield(s->as.if_stmt.else_branch)) return 1;
        }
        if (s->type == STMT_WHILE && _has_yield(s->as.while_stmt.body)) return 1;
        if (s->type == STMT_FOR && _has_yield(s->as.for_stmt.body)) return 1;
    }
    return 0;
}

// Collect yield values from body (flat list, in order) — includes nested blocks/loops
static int _collect_yields(Stmt* body, Expr** yields, int max) {
    int count = 0;
    for (Stmt* s = body; s && count < max; s = s->next) {
        if (s->type == STMT_YIELD) {
            if (s->as.yield_stmt.value) yields[count++] = s->as.yield_stmt.value;
        } else if (s->type == STMT_BLOCK)
            count += _collect_yields(s->as.block.statements, yields+count, max-count);
        else if (s->type == STMT_WHILE)
            count += _collect_yields(s->as.while_stmt.body, yields+count, max-count);
        else if (s->type == STMT_FOR)
            count += _collect_yields(s->as.for_stmt.body, yields+count, max-count);
        else if (s->type == STMT_IF) {
            count += _collect_yields(s->as.if_stmt.then_branch, yields+count, max-count);
            if (s->as.if_stmt.else_branch)
                count += _collect_yields(s->as.if_stmt.else_branch, yields+count, max-count);
        }
    }
    return count;
}

// Check if all yields are at the top level (no yields inside loops)
static int _yields_are_sequential(Stmt* body) {
    for (Stmt* s = body; s; s = s->next) {
        if (s->type == STMT_WHILE || s->type == STMT_FOR) {
            // Any yield inside a loop = needs coroutine
            Stmt* lbody = (s->type==STMT_WHILE) ? s->as.while_stmt.body : s->as.for_stmt.body;
            if (_has_yield(lbody)) return 0;
        }
        if (s->type == STMT_IF) {
            if (_has_yield(s->as.if_stmt.then_branch)) return 0;
            if (_has_yield(s->as.if_stmt.else_branch)) return 0;
        }
        // Recurse into blocks — a block wrapping a loop with yield is also non-sequential
        if (s->type == STMT_BLOCK) {
            if (!_yields_are_sequential(s->as.block.statements)) return 0;
        }
    }
    return 1;
}










static void aot_emit_proc(AotCompiler* aot, Stmt* s) {
    ProcStmt* ps = (s->type==STMT_PROC)?&s->as.proc:&s->as.async_proc;
    char* base_fname = aot_cname_tok(ps->name);
    char* fname;
    // When compiling a module, prefix all proc names with the module prefix
    if (aot->current_module_prefix[0]) {
        fname = malloc(strlen(aot->current_module_prefix) + strlen(base_fname) + 1);
        sprintf(fname, "%s%s", aot->current_module_prefix, base_fname);
        free(base_fname);
    } else {
        fname = base_fname;
    }

    // Save type env state to restore after proc (proc params are local)
    int saved_type_count = aot->type_env.count;
    // Set param types (override any existing entry to avoid cross-proc pollution)
    for (int i = 0; i < ps->param_count; i++) {
        JitTypeTag pt = aot_param_type(aot, ps->name.start, ps->name.length, i);
        if (pt != JIT_TYPE_UNKNOWN) {
            char pname[256];
            int len = ps->params[i].length<255?ps->params[i].length:255;
            memcpy(pname, ps->params[i].start, len); pname[len]='\0';
            // Find and update existing entry, or add new
            int found_param = 0;
            for (int j = 0; j < aot->type_env.count; j++) {
                if (strcmp(aot->type_env.vars[j].name, pname) == 0) {
                    aot->type_env.vars[j].inferred_type = pt;
                    found_param = 1; break;
                }
            }
            if (!found_param) aot_set_var_type(aot, pname, pt);
        }
    }
    aot_infer_body(aot, ps->body);


    // Check if generator — emit state machine or coroutine
    if (ps->body && _has_yield(ps->body)) {
        if (!_yields_are_sequential(ps->body)) {
            // Complex generator (yields inside loops): use ucontext coroutine
            char gname[128]; snprintf(gname, sizeof(gname), "_gen_%s", fname);
            int np = ps->param_count;
            // Coroutine body function
            aot_emit(aot, "static void %s_body(SageCoroutine* _co) {", gname);
            aot->indent++;
            for (int pi = 0; pi < np; pi++) {
                char* pn = aot_cname_tok(ps->params[pi]);
                aot_emit(aot, "SageValue %s = _co->argv[%d];", pn, pi); free(pn);
            }
            aot_infer_body(aot, ps->body);
            // Set coroutine context so STMT_YIELD → sage_rt_coro_yield(_co, val)
            aot->in_coro_body = 1;
            strcpy(aot->coro_var, "_co");
            // Temporarily suppress type specialization — all vars are SageValue inside
            // the coroutine body because params come from _co->argv (always boxed)
            int coro_saved_count = aot->type_env.count;
            aot->type_env.count = 0;
            for (Stmt* bs = ps->body; bs; bs = bs->next) aot_compile_stmt(aot, bs);
            aot->in_coro_body = 0;
            aot->type_env.count = coro_saved_count;
            aot_emit(aot, "_co->done = 1;");
            aot->indent--; aot_emit(aot, "}");
            // Constructor
            aot_emit(aot, "static SageValue %s(", fname);
            aot->indent++;
            for (int pi = 0; pi < np; pi++) {
                char* pn = aot_cname_tok(ps->params[pi]);
                aot_emit(aot, "SageValue %s%s", pn, pi<np-1?",":""); free(pn);
            }
            if (np==0) aot_emit(aot,"void");
            aot->indent--; aot_emit(aot, ") {"); aot->indent++;
            if (np > 0) {
                aot_emit(aot, "SageValue* _argv = (SageValue*)malloc(%d*sizeof(SageValue));", np);
                for (int pi = 0; pi < np; pi++) {
                    char* pn = aot_cname_tok(ps->params[pi]);
                    aot_emit(aot, "_argv[%d] = %s;", pi, pn); free(pn);
                }
                aot_emit(aot, "SageCoroutine* _co = sage_rt_coro_new((SageCoroutineBody)%s_body,%d,_argv);",gname,np);
            } else {
                aot_emit(aot,"SageCoroutine* _co = sage_rt_coro_new((SageCoroutineBody)%s_body,0,NULL);",gname);
            }
            aot_emit(aot, "return sage_rt_make_fn((SageNativeFn)_coro_next_fn, _co, \"%s\");", fname);
            aot->indent--; aot_emit(aot, "}");
            aot_blank(aot);
            aot->type_env.count = saved_type_count;
            free(fname); return;
        }
        // Simple sequential generator: use state machine
        {
        char gname[128]; snprintf(gname, sizeof(gname), "_gen_%s", fname);
        Expr* yields[64]; int nyields = _collect_yields(ps->body, yields, 64);
        int np = ps->param_count;  // number of params (incl. self if any)
        // State struct: state index + params as SageValue fields
        aot_emit(aot, "typedef struct { int _state; SageValue params[%d]; } %s;", np > 0 ? np : 1, gname);
        // Next function
        aot_emit(aot, "static SageValue %s_next(int _argc, SageValue* _argv, void* _env) {", gname);
        aot->indent++;
        aot_emit(aot, "%s* _g = (%s*)_env;", gname, gname);
        aot_emit(aot, "if (!_g) return sage_rt_nil();");
        // Bind params from struct
        for (int pi = 0; pi < np; pi++) {
            char* pn = aot_cname_tok(ps->params[pi]);
            aot_emit(aot, "SageValue %s = _g->params[%d];", pn, pi);
            free(pn);
        }
        // Also set up local vars that were LETs in the body
        aot_infer_body(aot, ps->body);
        // Simple generator: sequential yields
        aot_emit(aot, "switch (_g->_state) {"); aot->indent++;
        for (int yi = 0; yi < nyields; yi++) {
            char* yv = aot_expr(aot, yields[yi], JIT_TYPE_UNKNOWN);
            JitTypeTag yt = aot_infer_expr(aot, yields[yi]);
            int ynb = (yields[yi]->type == EXPR_VARIABLE && jit_is_unboxed(yt));
            char* yvb = ynb ? aot_box(yt, yv) : yv;
            aot_emit(aot, "case %d: _g->_state = %d; return %s;", yi, yi+1, yvb);
            if (yvb != yv) free(yvb); free(yv);
        }
        aot_emit(aot, "default: return sage_rt_nil();");
        aot->indent--; aot_emit(aot, "}");
        aot_emit(aot, "return sage_rt_nil();");
        aot->indent--; aot_emit(aot, "}");
        // Constructor
        aot_emit(aot, "static SageValue %s(", fname);
        aot->indent++;
        for (int pi = 0; pi < np; pi++) {
            char* pn = aot_cname_tok(ps->params[pi]);
            aot_emit(aot, "SageValue %s%s", pn, pi < np-1 ? "," : "");
            free(pn);
        }
        if (np == 0) aot_emit(aot, "void");
        aot->indent--;
        aot_emit(aot, ") {"); aot->indent++;
        aot_emit(aot, "%s* _gs = (%s*)malloc(sizeof(%s));", gname, gname, gname);
        aot_emit(aot, "_gs->_state = 0;");
        for (int pi = 0; pi < np; pi++) {
            char* pn = aot_cname_tok(ps->params[pi]);
            aot_emit(aot, "_gs->params[%d] = %s;", pi, pn);
            free(pn);
        }
        aot_emit(aot, "return sage_rt_make_fn((SageNativeFn)%s_next, _gs, \"%s\");", gname, fname);
        aot->indent--; aot_emit(aot, "}");
        aot_blank(aot);
        aot->type_env.count = saved_type_count;
        free(fname); return;
    }
    }  // end if (_has_yield)

    // Regular proc — determine return type
    // For now emit SageValue return — future pass can specialise this
    aot_emit(aot, "static SageValue %s(", fname);
    aot->indent++;
    for (int i = 0; i < ps->param_count; i++) {
        JitTypeTag pt = aot_param_type(aot, ps->name.start, ps->name.length, i);
        char* pn = aot_cname_tok(ps->params[i]);
        if (jit_is_unboxed(pt)) {
            aot_emit(aot, "%s %s%s", jit_ctype(pt), pn, i<ps->param_count-1?",":"");
        } else {
            aot_emit(aot, "SageValue %s%s", pn, i<ps->param_count-1?",":"");
        }
        free(pn);
    }
    aot->indent--;
    // (generator handled above)
    aot_emit(aot, ") {"); aot->indent++;
    int _saved_defer = aot->defer_count;
    aot->defer_count = 0;  // Reset defer stack for this function
    for(Stmt* bs = ps->body; bs; bs = bs->next) aot_compile_stmt(aot, bs);
    // Emit pending defers at function end (LIFO)
    for(int _di=aot->defer_count-1;_di>=0;_di--){
        aot_emit(aot,"{ /* defer */"); aot->indent++;
        for(Stmt*_ds=aot->defer_stack[_di]->as.defer.statement;_ds;_ds=_ds->next)
            aot_compile_stmt(aot,_ds);
        aot->indent--; aot_emit(aot,"}");
    }
    aot->defer_count = _saved_defer;
    aot_emit(aot, "return sage_rt_nil();");
    aot->indent--;
    aot_emit(aot, "}"); aot_blank(aot); free(fname);
    // Restore type env to pre-proc state (proc-local types don't persist)
    aot->type_env.count = saved_type_count;
}



char* aot_compile_program(AotCompiler* aot, Stmt* program) {
    aot_infer_types(aot,program);
    aot_emit(aot,"/* Auto-generated by sage --aot */");
    aot_emit(aot,"#define _POSIX_C_SOURCE 200809L");
    aot_emit(aot,"#define _GNU_SOURCE");
    aot_emit(aot,"#include <stdint.h>");
    aot_emit(aot,"#include <stdio.h>");
    aot_emit(aot,"#include <stdlib.h>");
    aot_emit(aot,"#include <string.h>");
    aot_emit(aot,"#include <math.h>");
    aot_emit(aot,"#include <setjmp.h>");
    aot_emit(aot,"#include \"sage_runtime.h\"");
    aot_blank(aot);
    // Coroutine next-function stub — used by complex generators (yields inside loops)
    aot_emit(aot,"static SageValue _coro_next_fn(int _argc, SageValue* _argv, void* _env) {");
    aot_emit(aot,"    (void)_argc; (void)_argv;");
    aot_emit(aot,"    return sage_rt_coro_next((SageCoroutine*)_env);");
    aot_emit(aot,"}");
    aot_blank(aot);
    // ── Call-site type analysis first ──────────────────────────────────────
    for (Stmt* s = program; s; s = s->next) {
        if (s->type == STMT_PROC || s->type == STMT_ASYNC_PROC) {
            ProcStmt* ps = (s->type==STMT_PROC)?&s->as.proc:&s->as.async_proc;
            aot_collect_calls(aot, ps->name.start, ps->name.length, program);
        }
        // Also collect call-site info for class constructors
        if (s->type == STMT_CLASS) {
            aot_collect_calls(aot, s->as.class_stmt.name.start, s->as.class_stmt.name.length, program);
        }
        if (s->type == STMT_STRUCT) {
            aot_collect_calls(aot, s->as.struct_stmt.name.start, s->as.struct_stmt.name.length, program);
        }
    }

    // ── Forward-declare procs with typed signatures ──────────────────────────
    for (Stmt* s = program; s; s = s->next) {
        if (s->type == STMT_PROC || s->type == STMT_ASYNC_PROC) {
            ProcStmt* ps = (s->type==STMT_PROC)?&s->as.proc:&s->as.async_proc;
            // Skip generators — they emit their own constructor in aot_emit_proc
            if (ps->body && _has_yield(ps->body)) continue;
            char* name = aot_cname_tok(ps->name);
            aot_emit(aot, "static SageValue %s(", name);
            aot->indent++;
            for (int i = 0; i < ps->param_count; i++) {
                JitTypeTag pt = aot_param_type(aot, ps->name.start, ps->name.length, i);
                aot_emit(aot, "%s%s",
                    jit_is_unboxed(pt) ? jit_ctype(pt) : "SageValue",
                    i < ps->param_count-1 ? "," : "");
            }
            aot->indent--;
            aot_emit(aot, ");");
            free(name);
        }
    }
    aot_blank(aot);
    // ── Process imports at file scope (before main) ────────────────────────
    // This ensures module procs/classes are emitted as file-scope statics
    for(Stmt*s=program;s;s=s->next)
        if(s->type==STMT_IMPORT)
            aot_compile_stmt(aot,s);
    aot_blank(aot);
    // Structs/enums/classes/impls
    for(Stmt*s=program;s;s=s->next)
        if(s->type==STMT_STRUCT||s->type==STMT_ENUM||s->type==STMT_CLASS||s->type==STMT_IMPL)
            aot_compile_stmt(aot,s);
    aot_blank(aot);
    // Nested proc wrappers (hoisted file-scope functions for closures)
    for(Stmt*s=program;s;s=s->next)
        if(s->type==STMT_PROC||s->type==STMT_ASYNC_PROC){
            ProcStmt*ps=(s->type==STMT_PROC)?&s->as.proc:&s->as.async_proc;
            aot_emit_nested_procs(aot,ps->body);
        }
    aot_blank(aot);
    // Procs
    for(Stmt*s=program;s;s=s->next)
        if(s->type==STMT_PROC||s->type==STMT_ASYNC_PROC)
            aot_emit_proc(aot,s);
    // Infer types for top-level code before emitting main
    aot_infer_body(aot, program);
    // main
    aot_emit(aot,"int main(int argc, char** argv) {");
    aot->indent++;
    aot_emit(aot,"(void)argc; (void)argv;");
    aot_emit(aot,"sage_rt_init();");
    // ── Initialize imported module namespace dicts ────────────────────────
    {
        const char* cur_mod = "";
        for (int mi = 0; mi < aot->mod_proc_count; mi++) {
            const char* mcn = aot->mod_procs[mi].mod_cname;
            if (strcmp(mcn, cur_mod) != 0) {
                aot_emit(aot, "%s = sage_rt_dict_new();", mcn);
                cur_mod = aot->mod_procs[mi].mod_cname;
            }
            const char* wrap = aot->mod_procs[mi].wrap_cname;
            if (wrap[0] == '@' && wrap[1] == '@') {
                // Module variable — value expression follows @@
                const char* expr_str = wrap + 2;
                aot_emit(aot, "sage_rt_dict_set(%s, sage_rt_string(\"%s\"), %s);",
                         mcn, aot->mod_procs[mi].proc_raw, expr_str);
            } else if (wrap[0] == '@') {
                // Legacy single-@ variable reference
                const char* cvar = wrap + 1;
                aot_emit(aot, "sage_rt_dict_set(%s, sage_rt_string(\"%s\"), %s);",
                         mcn, aot->mod_procs[mi].proc_raw, cvar);
            } else {
                aot_emit(aot, "sage_rt_dict_set(%s, sage_rt_string(\"%s\"), sage_rt_make_fn((SageNativeFn)%s, NULL, \"%s\"));",
                         mcn, aot->mod_procs[mi].proc_raw, wrap,
                         aot->mod_procs[mi].proc_raw);
            }
        }
    }
    // Register enums as namespace dicts
    for(Stmt*s=program;s;s=s->next){
        if(s->type==STMT_ENUM){
            EnumStmt*es=&s->as.enum_stmt;
            char*en=aot_cname_tok(es->name);
            char en_raw[256]; int enrl=es->name.length<255?es->name.length:255;
            memcpy(en_raw,es->name.start,enrl); en_raw[enrl]='\0';
            aot_emit(aot,"%s=sage_rt_dict_new();",en);
            // Store enum name so typeof() works
            aot_emit(aot,"sage_rt_dict_set(%s,sage_rt_string(\"__name__\"),sage_rt_string(\"%s\"));",en,en_raw);
            for(int i=0;i<es->variant_count;i++){
                char*vn=aot_cname_tok(es->variant_names[i]);
                char vraw[256]; int vl=es->variant_names[i].length<255?es->variant_names[i].length:255;
                memcpy(vraw,es->variant_names[i].start,vl); vraw[vl]='\0';
                int has_fields=(es->variant_field_counts&&es->variant_field_counts[i]>0);
                if(!has_fields){
                    // Simple/unit variant: store tag dict directly
                    aot_emit(aot,"sage_rt_dict_set(%s,sage_rt_string(\"%s\"),%s_%s());",en,vraw,en,vn);
                }
                // ADT variants with fields are called directly as sg_Enum_sg_Variant(args)
                // in the EXPR_CALL handler — no need to store in dict for AOT code
                free(vn);
            }
            free(en);
        }
    }
    // Register all class and struct definitions
    for(Stmt*s=program;s;s=s->next){
        if(s->type==STMT_CLASS){
            char*cn=aot_cname_tok(s->as.class_stmt.name);
            int mc=0;
            for(Stmt*m=s->as.class_stmt.methods;m;m=m->next) if(m->type==STMT_PROC) mc++;
            char rawname[256]; int nl=s->as.class_stmt.name.length<255?s->as.class_stmt.name.length:255;
            memcpy(rawname,s->as.class_stmt.name.start,nl); rawname[nl]='\0';
            // Set parent class if this class inherits
            if(s->as.class_stmt.has_parent && s->as.class_stmt.parent.length>0){
                char* pn=aot_cname_tok(s->as.class_stmt.parent);
                aot_emit(aot,"_%s_classval = sage_rt_class_new(\"%s\",_%s_classval.as.class_def,_%s_methods,%d,NULL,0,0);",
                     cn,rawname,pn,cn,mc);
                free(pn);
            } else {
                aot_emit(aot,"_%s_classval = sage_rt_class_new(\"%s\",NULL,_%s_methods,%d,NULL,0,0);",
                     cn,rawname,cn,mc);
            }
            free(cn);
        }
        if(s->type==STMT_STRUCT){
            StructStmt*ssr=&s->as.struct_stmt;
            char*cn=aot_cname_tok(ssr->name);
            char rawname[256]; int nl=ssr->name.length<255?ssr->name.length:255;
            memcpy(rawname,ssr->name.start,nl); rawname[nl]='\0';
            // Emit inline field name array for this registration
            if(ssr->field_count>0){
                aot_emit(aot,"{ static const char* _sfn[] = {");
                aot->indent++;
                for(int i=0;i<ssr->field_count;i++){
                    char esc[64]; int el=ssr->field_names[i].length<63?ssr->field_names[i].length:63;
                    memcpy(esc,ssr->field_names[i].start,el); esc[el]='\0';
                    aot_emit(aot,"\"%s\"%s",esc,i<ssr->field_count-1?",":"");
                }
                aot->indent--;
                aot_emit(aot,"};");
                aot_emit(aot,"_%s_classval=sage_rt_class_new(\"%s\",NULL,NULL,0,_sfn,%d,1); }",
                         cn,rawname,ssr->field_count);
            } else {
                aot_emit(aot,"_%s_classval=sage_rt_class_new(\"%s\",NULL,NULL,0,NULL,0,1);",cn,rawname);
            }
            free(cn);
        }
    }

    // Register impl methods (after class/struct classvals are created)
    for(Stmt*s=program;s;s=s->next){
        if(s->type==STMT_IMPL){
            ImplStmt*is=&s->as.impl_stmt;
            char*tn=aot_cname_tok(is->target);
            int mc=0; for(Stmt*m=is->methods;m;m=m->next) if(m->type==STMT_PROC) mc++;
            if(mc>0)
                aot_emit(aot,"_%s_classval = sage_rt_add_methods(_%s_classval,_%s_impl_methods,%d);",tn,tn,tn,mc);
            free(tn);
        }
    }
    aot_blank(aot);
    for(Stmt*s=program;s;s=s->next){
        if(s->type==STMT_PROC||s->type==STMT_ASYNC_PROC||
           s->type==STMT_STRUCT||s->type==STMT_ENUM||
           s->type==STMT_CLASS||s->type==STMT_IMPL||
           s->type==STMT_TRAIT||s->type==STMT_MACRO_DEF) continue;
        aot_compile_stmt(aot,s);
    }
    aot_blank(aot);
    aot_emit(aot,"sage_rt_shutdown();");
    aot_emit(aot,"return 0;");
    aot->indent--;
    aot_emit(aot,"}");
    // Join
    size_t total=0;
    for(int i=0;i<aot->line_count;i++) total+=strlen(aot->lines[i])+1;
    char* result=malloc(total+1);
    char* p=result;
    for(int i=0;i<aot->line_count;i++){size_t n=strlen(aot->lines[i]);memcpy(p,aot->lines[i],n);p+=n;*p++='\n';}
    *p='\0';
    return result;
}

int aot_write_c_file(AotCompiler* aot, const char* path) {
    FILE* f=fopen(path,"w");
    if(!f) return 0;
    for(int i=0;i<aot->line_count;i++){fputs(aot->lines[i],f);fputc('\n',f);}
    fclose(f); return 1;
}


int aot_compile_to_binary(AotCompiler* aot, const char* c_path, const char* bin_path) {
    (void)aot;
    // Find runtime dir
    const char* rt_dir = getenv("SAGE_RUNTIME_DIR");
    if (!rt_dir) rt_dir = "runtime"; // fallback for dev builds

    char rt_obj[512], rt_inc[512];
    snprintf(rt_obj,sizeof(rt_obj),"%s/libsage_runtime.a",rt_dir);
    snprintf(rt_inc,sizeof(rt_inc),"-I%s",rt_dir);

    const char* cc=getenv("CC")?getenv("CC"):"cc";
    pid_t pid=fork();
    if(pid<0) return 0;
    if(pid==0){
        execlp(cc,cc,"-std=c11","-O3","-march=native",
               "-fomit-frame-pointer","-funroll-loops","-ffast-math",
               rt_inc,c_path,rt_obj,"-o",bin_path,"-lm",(char*)NULL);
        _exit(127);
    }
    int status; waitpid(pid,&status,0);
    return WIFEXITED(status)&&WEXITSTATUS(status)==0;
}

char* aot_emit_add_int(AotCompiler* aot, const char* left, const char* right) {
    (void)aot;
    char* out=malloc(strlen(left)+strlen(right)+16);
    sprintf(out,"((%s)+(%s))",left,right); return out;
}
char* aot_emit_add_string(AotCompiler* aot, const char* left, const char* right) {
    (void)aot;
    char* out=malloc(strlen(left)+strlen(right)+80);
    sprintf(out,"sage_rt_string_concat(sage_rt_string(%s),sage_rt_string(%s)).as.string",left,right); return out;
}
char* aot_emit_add_generic(AotCompiler* aot, const char* left, const char* right) {
    (void)aot;
    char* out=malloc(strlen(left)+strlen(right)+32);
    sprintf(out,"sage_rt_add(%s,%s)",left,right); return out;
}
