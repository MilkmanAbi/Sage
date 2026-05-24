#define _GNU_SOURCE   // for mkstemps
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <ctype.h>    // isalnum
#include <stdint.h>   // uintptr_t
#include <inttypes.h> // PRId64 (P1)
#include <math.h>     // isinf, isnan
#include <unistd.h>   // getpid, unlink
#include <sys/stat.h> // stat, S_ISDIR, S_ISREG
#include "sage_thread.h"  // Phase 11: async/await thread joining
#ifndef SAGE_NO_FFI
#include <dlfcn.h>
#ifdef SAGE_HAS_FFI
#include <ffi.h>
#endif
#include "parser.h"   // for ParserState
#include "lilybox.h"
#include "firefly.h"  // LilyKnight sandbox, parse_expression_public    // Phase 9: FFI (dlopen, dlsym, dlclose)
#include <ffi.h>      // Phase 4: libffi for extern proc calls
#endif
#include "interpreter.h"
#include "token.h"
#include "env.h"
#include "value.h"
#include "gc.h"
#include "ast.h"
#include "module.h"  // Phase 8: Module system
#include "repl.h"    // Phase 12: REPL error recovery

// Helper macro for creating normal expression results
#define EVAL_RESULT(v) ((ExecResult){ (v), 0, 0, 0, 0, sage_nil, 0, NULL, g_gas_used, g_gas_limit })
#define EVAL_EXCEPTION(exc) ((ExecResult){ sage_nil, 0, 0, 0, 1, (exc), 0, NULL, g_gas_used, g_gas_limit })

// P1: Comparison helpers (interpreter-only, not needed in VM)

// P1: Runtime type checking — validates a Value matches a TypeAnnotation
// Returns 1 if valid, 0 if mismatch. Sets err_msg on failure.
static int sage_typecheck(Value val, TypeAnnotation* ann, const char** err_expected) {
    if (!ann) return 1;  // no annotation = any type OK
    
    const char* t = ann->name.start;
    int tlen = ann->name.length;
    
    // Handle optional types: T? accepts nil
    if (ann->is_optional && IS_NIL(val)) return 1;
    
    // Match against known type names
    if (tlen == 3 && strncmp(t, "int", 3) == 0) {
        if (IS_INT(val)) return 1;
        *err_expected = "int"; return 0;
    }
    if (tlen == 5 && strncmp(t, "float", 5) == 0) {
        if (IS_NUMBER(val)) return 1;
        *err_expected = "float"; return 0;
    }
    if (tlen == 6 && strncmp(t, "double", 6) == 0) {
        if (IS_NUMBER(val)) return 1;
        *err_expected = "double"; return 0;
    }
    if (tlen == 3 && strncmp(t, "num", 3) == 0) {
        if (IS_NUMERIC(val)) return 1;
        *err_expected = "num"; return 0;
    }
    if (tlen == 3 && strncmp(t, "str", 3) == 0) {
        if (IS_STRING(val)) return 1;
        *err_expected = "str"; return 0;
    }
    if (tlen == 4 && strncmp(t, "bool", 4) == 0) {
        if (IS_BOOL(val)) return 1;
        *err_expected = "bool"; return 0;
    }
    if (tlen == 5 && strncmp(t, "Array", 5) == 0) {
        if (IS_ARRAY(val)) return 1;
        *err_expected = "Array"; return 0;
    }
    if (tlen == 4 && strncmp(t, "Dict", 4) == 0) {
        if (IS_DICT(val)) return 1;
        *err_expected = "Dict"; return 0;
    }
    if (tlen == 5 && strncmp(t, "Tuple", 5) == 0) {
        if (IS_TUPLE(val)) return 1;
        *err_expected = "Tuple"; return 0;
    }
    if (tlen == 3 && strncmp(t, "any", 3) == 0) {
        return 1;  // any type OK
    }
    if (tlen == 3 && strncmp(t, "nil", 3) == 0) {
        if (IS_NIL(val)) return 1;
        *err_expected = "nil"; return 0;
    }
    // Unknown type annotation — don't error (could be a user class, gradual typing)
    return 1;
}

const char* sage_typeof_str(Value val) {
    switch (val.type) {
        case VAL_INT: return "int";
        case VAL_NUMBER: return "float";
        case VAL_BOOL: return "bool";
        case VAL_NIL: return "nil";
        case VAL_STRING: return "str";
        case VAL_ARRAY: return "Array";
        case VAL_DICT: return "Dict";
        case VAL_TUPLE: return "Tuple";
        case VAL_FUNCTION: case VAL_NATIVE: return "function";
        default: return "unknown";
    }
}

// P2: Builtin method dispatch for string, array, dict
// Returns 1 if handled (result written to *out), 0 if not a known method
int builtin_method_call(Value object, const char* name, int name_len,
                               int argc, Value* args, Value* out) {
    // String methods
    if (IS_STRING(object)) {
        const char* s = AS_STRING(object);
        if (name_len == 5 && strncmp(name, "upper", 5) == 0) {
            char* r = string_upper(s); *out = val_string_take(r); return 1;
        }
        if (name_len == 5 && strncmp(name, "lower", 5) == 0) {
            char* r = string_lower(s); *out = val_string_take(r); return 1;
        }
        if (name_len == 4 && strncmp(name, "trim", 4) == 0) {
            const char* start = s;
            while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') start++;
            const char* end = s + strlen(s) - 1;
            while (end > start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;
            size_t len = (size_t)(end - start + 1);
            char* r = SAGE_ALLOC(len + 1);
            memcpy(r, start, len);
            r[len] = '\0';
            *out = val_string_take(r); return 1;
        }
        if (name_len == 5 && strncmp(name, "split", 5) == 0) {
            if (argc >= 1 && IS_STRING(args[0])) {
                *out = string_split(s, AS_STRING(args[0])); return 1;
            }
            *out = string_split(s, " "); return 1;
        }
        if (name_len == 7 && strncmp(name, "replace", 7) == 0) {
            if (argc >= 2 && IS_STRING(args[0]) && IS_STRING(args[1])) {
                char* r = string_replace(s, AS_STRING(args[0]), AS_STRING(args[1]));
                *out = val_string_take(r); return 1;
            }
            *out = val_nil(); return 1;
        }
        if (name_len == 8 && strncmp(name, "contains", 8) == 0) {
            if (argc >= 1 && IS_STRING(args[0])) {
                *out = val_bool(strstr(s, AS_STRING(args[0])) != NULL); return 1;
            }
            *out = val_bool(0); return 1;
        }
        if (name_len == 11 && strncmp(name, "starts_with", 11) == 0) {
            if (argc >= 1 && IS_STRING(args[0])) {
                const char* prefix = AS_STRING(args[0]);
                *out = val_bool(strncmp(s, prefix, strlen(prefix)) == 0); return 1;
            }
            *out = val_bool(0); return 1;
        }
        if (name_len == 9 && strncmp(name, "ends_with", 9) == 0) {
            if (argc >= 1 && IS_STRING(args[0])) {
                const char* suffix = AS_STRING(args[0]);
                size_t slen = strlen(s), sufflen = strlen(suffix);
                *out = val_bool(slen >= sufflen && strcmp(s + slen - sufflen, suffix) == 0);
                return 1;
            }
            *out = val_bool(0); return 1;
        }
        // P15: .encode() for Python FFI compat -- returns bytes-tagged string
        if (name_len == 6 && strncmp(name, "encode", 6) == 0) {
            *out = object;  // Sage strings are already UTF-8
            return 1;
        }
        return 0;
    }

    // Array methods
    if (IS_ARRAY(object)) {
        if (name_len == 4 && strncmp(name, "push", 4) == 0) {
            if (argc >= 1) { array_push(&object, args[0]); }
            *out = val_nil(); return 1;
        }
        if (name_len == 6 && strncmp(name, "append", 6) == 0) {
            if (argc >= 1) { array_push(&object, args[0]); }
            *out = val_nil(); return 1;
        }
        if (name_len == 3 && strncmp(name, "pop", 3) == 0) {
            ArrayValue* a = object.as.array;
            if (a->count == 0) { *out = val_nil(); return 1; }
            *out = a->elements[a->count - 1];
            a->count--;
            return 1;
        }
        if (name_len == 3 && strncmp(name, "len", 3) == 0) {
            *out = val_int(object.as.array->count); return 1;
        }
        if (name_len == 8 && strncmp(name, "contains", 8) == 0) {
            if (argc >= 1) {
                for (int i = 0; i < object.as.array->count; i++) {
                    if (values_equal(object.as.array->elements[i], args[0])) {
                        *out = val_bool(1); return 1;
                    }
                }
            }
            *out = val_bool(0); return 1;
        }
        if (name_len == 4 && strncmp(name, "join", 4) == 0) {
            const char* sep = argc >= 1 && IS_STRING(args[0]) ? AS_STRING(args[0]) : ",";
            *out = string_join(&object, sep); return 1;
        }
        // P6: .reverse() — in-place
        if (name_len == 7 && strncmp(name, "reverse", 7) == 0) {
            ArrayValue* a = object.as.array;
            for (int i = 0; i < a->count / 2; i++) {
                Value tmp = a->elements[i];
                a->elements[i] = a->elements[a->count - 1 - i];
                a->elements[a->count - 1 - i] = tmp;
            }
            *out = val_nil(); return 1;
        }
        // P6: .clear()
        if (name_len == 5 && strncmp(name, "clear", 5) == 0) {
            object.as.array->count = 0;
            *out = val_nil(); return 1;
        }
        // P6: .slice(start, end)
        if (name_len == 5 && strncmp(name, "slice", 5) == 0) {
            ArrayValue* a = object.as.array;
            int start = argc >= 1 && IS_NUMERIC(args[0]) ? (int)NUMERIC_AS_INT(args[0]) : 0;
            int end = argc >= 2 && IS_NUMERIC(args[1]) ? (int)NUMERIC_AS_INT(args[1]) : a->count;
            if (start < 0) start += a->count;
            if (end < 0) end += a->count;
            if (start < 0) start = 0;
            if (end > a->count) end = a->count;
            Value result = val_array();
            for (int i = start; i < end; i++) {
                array_push(&result, a->elements[i]);
            }
            *out = result; return 1;
        }
        // P6: .indexOf(value)
        if (name_len == 7 && strncmp(name, "indexOf", 7) == 0) {
            if (argc >= 1) {
                ArrayValue* a = object.as.array;
                for (int i = 0; i < a->count; i++) {
                    if (values_equal(a->elements[i], args[0])) {
                        *out = val_int(i); return 1;
                    }
                }
            }
            *out = val_int(-1); return 1;
        }
        return 0;
    }

    // Dict methods
    if (IS_DICT(object)) {
        if (name_len == 4 && strncmp(name, "keys", 4) == 0) {
            *out = dict_keys(&object); return 1;
        }
        if (name_len == 6 && strncmp(name, "values", 6) == 0) {
            *out = dict_values(&object); return 1;
        }
        if (name_len == 3 && strncmp(name, "len", 3) == 0) {
            *out = val_int(object.as.dict->count); return 1;
        }
        if (name_len == 12 && strncmp(name, "contains_key", 12) == 0) {
            if (argc >= 1 && IS_STRING(args[0])) {
                Value v = dict_get(&object, AS_STRING(args[0]));
                *out = val_bool(!IS_NIL(v)); return 1;
            }
            *out = val_bool(0); return 1;
        }
        // P6: .get(key, default) — return default if key missing
        if (name_len == 3 && strncmp(name, "get", 3) == 0) {
            if (argc >= 1 && IS_STRING(args[0])) {
                Value v = dict_get(&object, AS_STRING(args[0]));
                if (!IS_NIL(v)) { *out = v; return 1; }
            }
            *out = argc >= 2 ? args[1] : val_nil(); return 1;
        }
        // P6: .delete(key)
        if (name_len == 6 && strncmp(name, "delete", 6) == 0) {
            if (argc >= 1 && IS_STRING(args[0])) {
                dict_delete(&object, AS_STRING(args[0]));
            }
            *out = val_nil(); return 1;
        }
        return 0;
    }

    return 0;
}

static inline int sage_cmp_lt(Value a, Value b)  {
    if (IS_INT(a) && IS_INT(b)) return AS_INT(a) < AS_INT(b);
    return NUMERIC_AS_DOUBLE(a) < NUMERIC_AS_DOUBLE(b);
}
static inline int sage_cmp_gt(Value a, Value b)  {
    if (IS_INT(a) && IS_INT(b)) return AS_INT(a) > AS_INT(b);
    return NUMERIC_AS_DOUBLE(a) > NUMERIC_AS_DOUBLE(b);
}
static inline int sage_cmp_lte(Value a, Value b) {
    if (IS_INT(a) && IS_INT(b)) return AS_INT(a) <= AS_INT(b);
    return NUMERIC_AS_DOUBLE(a) <= NUMERIC_AS_DOUBLE(b);
}
static inline int sage_cmp_gte(Value a, Value b) {
    if (IS_INT(a) && IS_INT(b)) return AS_INT(a) >= AS_INT(b);
    return NUMERIC_AS_DOUBLE(a) >= NUMERIC_AS_DOUBLE(b);
}
#define RESULT_NORMAL(v) ((ExecResult){ (v), 0, 0, 0, 0, sage_nil, 0, NULL, g_gas_used, g_gas_limit })

Environment* g_global_env = NULL;
__thread EnvRootNode* g_gc_root_stack = NULL;

#define AST_GC_TEMP_MAX 1024
__thread Value g_ast_gc_temps[AST_GC_TEMP_MAX];
__thread int g_ast_gc_temp_count = 0;
#define AST_GC_PUSH(v) do { \
    ThreadState* ts = gc_get_thread_state(); \
    if (ts) { \
        if (ts->ast_gc_temp_count < AST_GC_TEMP_MAX) ts->ast_gc_temps[ts->ast_gc_temp_count++] = (v); \
    } else { \
        if (g_ast_gc_temp_count < AST_GC_TEMP_MAX) g_ast_gc_temps[g_ast_gc_temp_count++] = (v); \
    } \
} while(0)
#define AST_GC_POP() do { \
    ThreadState* ts = gc_get_thread_state(); \
    if (ts) { if (ts->ast_gc_temp_count > 0) ts->ast_gc_temp_count--; } \
    else { if (g_ast_gc_temp_count > 0) g_ast_gc_temp_count--; } \
} while(0)
#define AST_GC_POP_N(n) do { \
    ThreadState* ts = gc_get_thread_state(); \
    if (ts) { ts->ast_gc_temp_count -= (n); if (ts->ast_gc_temp_count < 0) ts->ast_gc_temp_count = 0; } \
    else { g_ast_gc_temp_count -= (n); if (g_ast_gc_temp_count < 0) g_ast_gc_temp_count = 0; } \
} while(0)

#define AST_GC_ENV_TEMP_MAX 256
__thread Env* g_ast_gc_env_temps[AST_GC_ENV_TEMP_MAX];
__thread int g_ast_gc_env_temp_count = 0;
#define AST_GC_PUSH_ENV(e) do { \
    ThreadState* ts = gc_get_thread_state(); \
    if (ts) { \
        if (ts->ast_gc_env_temp_count < AST_GC_ENV_TEMP_MAX) ts->ast_gc_env_temps[ts->ast_gc_env_temp_count++] = (e); \
    } else { \
        if (g_ast_gc_env_temp_count < AST_GC_ENV_TEMP_MAX) g_ast_gc_env_temps[g_ast_gc_env_temp_count++] = (e); \
    } \
} while(0)
#define AST_GC_POP_ENV() do { \
    ThreadState* ts = gc_get_thread_state(); \
    if (ts) { if (ts->ast_gc_env_temp_count > 0) ts->ast_gc_env_temp_count--; } \
    else { if (g_ast_gc_env_temp_count > 0) g_ast_gc_env_temp_count--; } \
} while(0)

static Stmt* g_generator_resume_target = NULL;

// Phase 2: Gas tracking globals
static __thread long g_gas_limit = -1; // -1 means unlimited
static __thread long g_gas_used = 0;

static ExecResult gas_error(void) {
    return EVAL_EXCEPTION(val_exception("Out of gas"));
}

static int consume_gas(long amount) {
    if (g_gas_limit < 0) return 1;
    g_gas_used += amount;
    if (g_gas_used > g_gas_limit) return 0;
    return 1;
}

static Value vm_set_gas_limit_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_nil();
    g_gas_limit = (long)NUMERIC_AS_INT(args[0]);
    g_gas_used = 0;
    return val_nil();
}

static Value vm_get_gas_used_native(int argCount, Value* args) {
    (void)argCount; (void)args;
    return val_int((int64_t)g_gas_used);
}

static Value vm_get_gas_limit_native(int argCount, Value* args) {
    (void)argCount; (void)args;
    return val_int((int64_t)g_gas_limit);
}

// JIT state — global, initialized by --jit mode
#include "jit.h"
static JitState* g_jit = NULL;
void interpreter_set_jit(JitState* jit) {
#ifdef SAGE_EXPERIMENTAL_JIT
    g_jit = jit;
#else
    (void)jit;
    g_jit = NULL;
#endif
}
JitState* interpreter_get_jit(void) { return g_jit; }

// Recursion depth tracking to prevent stack overflow
#define MAX_RECURSION_DEPTH 1000

// Check if a statement has a specific pragma decorator (@nojit, @noaot, etc.)
static int stmt_has_pragma(Stmt* stmt, const char* name) {
    if (!stmt || !stmt->pragmas) return 0;
    for (Pragma* p = stmt->pragmas; p; p = p->next) {
        if (strcmp(p->name, name) == 0) return 1;
    }
    return 0;
}
// Maximum loop iterations to prevent hangs and stack exhaustion
#define MAX_LOOP_ITERATIONS 10000000
static __thread int g_recursion_depth = 0;

static int stmt_contains_target(Stmt* stmt, Stmt* target) {
    if (stmt == NULL || target == NULL) return 0;
    if (stmt == target) return 1;

    switch (stmt->type) {
        case STMT_BLOCK:
            for (Stmt* current = stmt->as.block.statements; current != NULL; current = current->next) {
                if (stmt_contains_target(current, target)) return 1;
            }
            return 0;
        case STMT_IF:
            return stmt_contains_target(stmt->as.if_stmt.then_branch, target) ||
                   stmt_contains_target(stmt->as.if_stmt.else_branch, target);
        case STMT_WHILE:
            return stmt_contains_target(stmt->as.while_stmt.body, target);
        case STMT_FOR:
            return stmt_contains_target(stmt->as.for_stmt.body, target);
        case STMT_TRY: {
            if (stmt_contains_target(stmt->as.try_stmt.try_block, target) ||
                stmt_contains_target(stmt->as.try_stmt.finally_block, target)) {
                return 1;
            }
            for (int i = 0; i < stmt->as.try_stmt.catch_count; i++) {
                if (stmt_contains_target(stmt->as.try_stmt.catches[i]->body, target)) {
                    return 1;
                }
            }
            return 0;
        }
        default:
            return 0;
    }
}

// --- Native Functions ---

static Value clock_native(int argCount, Value* args) {
    (void)argCount; (void)args;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return val_number((double)tv.tv_sec + (double)tv.tv_usec / 1000000.0);
}

static Value input_native(int argCount, Value* args) {
    char buffer[1024];
    if (fgets(buffer, sizeof(buffer), stdin) != NULL) {
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len-1] == '\n') buffer[len-1] = '\0';

        char* str = SAGE_ALLOC(len + 1);
        memcpy(str, buffer, len + 1);
        return val_string_take(str);
    }
    return val_nil();
}

static Value tonumber_native(int argCount, Value* args) {
    if (argCount != 1) return val_nil();
    if (IS_NUMERIC(args[0])) return args[0];
    if (IS_STRING(args[0])) {
        return val_number(strtod(AS_STRING(args[0]), NULL));
    }
    return val_nil();
}

// PHASE 7: int() function for number-to-int conversion
static Value int_native(int argCount, Value* args) {
    if (argCount != 1) return val_nil();
    if (IS_INT(args[0])) return args[0];  // already int
    if (IS_NUMERIC(args[0])) {
        return val_int((int64_t)NUMERIC_AS_INT(args[0]));
    }
    if (IS_STRING(args[0])) {
        char* end;
        long long v = strtoll(AS_STRING(args[0]), &end, 10);
        if (end != AS_STRING(args[0])) return val_int((int64_t)v);
        return val_nil();
    }
    if (IS_BOOL(args[0])) return val_int(AS_BOOL(args[0]) ? 1 : 0);
    return val_nil();
}

// PHASE 7: str() function for number-to-string conversion
// println(val) — print value followed by newline (alias for print statement)
static Value println_native(int argCount, Value* args) {
    if (argCount == 0) {
        printf("\n");
        return val_nil();
    }
    for (int i = 0; i < argCount; i++) {
        if (i > 0) printf(" ");
        print_value(args[i]);
    }
    printf("\n");
    return val_nil();
}

// eprint(val) — print to stderr
static Value eprint_native(int argCount, Value* args) {
    for (int i = 0; i < argCount; i++) {
        if (i > 0) fprintf(stderr, " ");
        // Simple stderr print
        if (IS_STRING(args[i]))      fprintf(stderr, "%s", AS_STRING(args[i]));
        else if (IS_INT(args[i]))    fprintf(stderr, "%" PRId64, AS_INT(args[i]));
        else if (IS_NUMERIC(args[i])) {
            double d = NUMERIC_AS_DOUBLE(args[i]);
            if (d == (long long)d) fprintf(stderr, "%lld", (long long)d);
            else fprintf(stderr, "%g", d);
        }
        else if (IS_BOOL(args[i]))   fprintf(stderr, "%s", AS_BOOL(args[i]) ? "true" : "false");
        else if (IS_NIL(args[i]))    fprintf(stderr, "nil");
    }
    fprintf(stderr, "\n");
    return val_nil();
}

// P1: precision(value, n) — format a number to n decimal places, returns string
static Value precision_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_NUMERIC(args[0]) || !IS_NUMERIC(args[1])) return val_nil();
    double val = NUMERIC_AS_DOUBLE(args[0]);
    int dp = (int)NUMERIC_AS_INT(args[1]);
    if (dp < 0) dp = 0;
    if (dp > 20) dp = 20;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", dp, val);
    size_t slen = strlen(buf);
    char* str = SAGE_ALLOC(slen + 1);
    memcpy(str, buf, slen + 1);
    return val_string_take(str);
}

// P1: typeof() builtin — returns type as string
static Value typeof_native(int argCount, Value* args) {
    if (argCount != 1) return val_string("nil");
    switch (args[0].type) {
        case VAL_INT:       return val_string("int");
        case VAL_NUMBER:    return val_string("float");
        case VAL_BOOL:      return val_string("bool");
        case VAL_NIL:       return val_string("nil");
        case VAL_STRING:    return val_string("str");
        case VAL_FUNCTION:  return val_string("function");
        case VAL_NATIVE:    return val_string("function");
        case VAL_ARRAY:     return val_string("Array");
        case VAL_DICT:      return val_string("Dict");
        case VAL_TUPLE:     return val_string("Tuple");
        case VAL_CLASS:     return val_string("Class");
        case VAL_INSTANCE:
            if (args[0].as.instance && args[0].as.instance->class_def && args[0].as.instance->class_def->name)
                return val_string(args[0].as.instance->class_def->name);
            return val_string("instance");
        case VAL_MODULE:    return val_string("Module");
        case VAL_EXCEPTION: return val_string("Exception");
        case VAL_GENERATOR: return val_string("Generator");
        case VAL_CLIB:      return val_string("CLib");
        case VAL_POINTER:   return val_string("Pointer");
        case VAL_THREAD:    return val_string("Thread");
        case VAL_MUTEX:     return val_string("Mutex");
        case VAL_BYTES:     return val_string("bytes");
        default:            return val_string("unknown");
    }
}

// P1: float() builtin — convert to float
static Value float_native(int argCount, Value* args) {
    if (argCount != 1) return val_nil();
    if (IS_NUMBER(args[0])) return args[0];  // already float
    if (IS_INT(args[0])) return val_number((double)AS_INT(args[0]));
    if (IS_STRING(args[0])) {
        char* end;
        double v = strtod(AS_STRING(args[0]), &end);
        if (end != AS_STRING(args[0])) return val_number(v);
        return val_nil();
    }
    if (IS_BOOL(args[0])) return val_number(AS_BOOL(args[0]) ? 1.0 : 0.0);
    return val_nil();
}

static Value str_native(int argCount, Value* args) {
    if (argCount != 1) return val_nil();
    
    char buffer[256];
    if (IS_INT(args[0])) {
        snprintf(buffer, sizeof(buffer), "%" PRId64, AS_INT(args[0]));
        size_t slen = strlen(buffer);
        char* str = SAGE_ALLOC(slen + 1);
        memcpy(str, buffer, slen + 1);
        return val_string_take(str);
    }
    if (IS_NUMERIC(args[0])) {
        double n = NUMERIC_AS_DOUBLE(args[0]);
        // P1: floats always show decimal point in str() to distinguish from int
        if (n == (long long)n && n >= -9007199254740992.0 && n <= 9007199254740992.0) {
            snprintf(buffer, sizeof(buffer), "%lld.0", (long long)n);
        } else {
            snprintf(buffer, sizeof(buffer), "%g", n);
        }
        size_t slen = strlen(buffer);
        char* str = SAGE_ALLOC(slen + 1);
        memcpy(str, buffer, slen + 1);
        return val_string_take(str);
    }
    if (IS_STRING(args[0])) {
        return args[0];
    }
    // Handle tagged dicts: Option/Result/range pretty printing
    if (IS_DICT(args[0])) {
        Value tag = dict_get(&args[0], "__type");
        if (IS_STRING(tag)) {
            const char* t = AS_STRING(tag);
            if (strcmp(t, "option.some") == 0) {
                Value inner = dict_get(&args[0], "__val");
                Value inner_str = str_native(1, &inner);
                if (!IS_STRING(inner_str)) return val_string("Some(?)");
                char buf[1024];
                snprintf(buf, sizeof(buf), "Some(%s)", AS_STRING(inner_str));
                return val_string(buf);
            } else if (strcmp(t, "result.ok") == 0) {
                Value inner = dict_get(&args[0], "__val");
                Value inner_str = str_native(1, &inner);
                char buf[1024];
                snprintf(buf, sizeof(buf), "Ok(%s)", IS_STRING(inner_str) ? AS_STRING(inner_str) : "?");
                return val_string(buf);
            } else if (strcmp(t, "result.err") == 0) {
                Value inner = dict_get(&args[0], "__val");
                Value inner_str = str_native(1, &inner);
                char buf[1024];
                snprintf(buf, sizeof(buf), "Err(%s)", IS_STRING(inner_str) ? AS_STRING(inner_str) : "?");
                return val_string(buf);
            }
        }
    }
    if (IS_NIL(args[0])) return val_string("nil");
    if (IS_BOOL(args[0])) {
        char* str = AS_BOOL(args[0]) ? "true" : "false";
        size_t slen = strlen(str);
        char* result = SAGE_ALLOC(slen + 1);
        memcpy(result, str, slen + 1);
        return val_string_take(result);
    }
    if (args[0].type == VAL_ARRAY) {
        ArrayValue* arr = args[0].as.array;
        // Estimate size: "[" + elements + "]"
        size_t buf_size = 1024;
        char* buf = SAGE_ALLOC(buf_size);
        size_t pos = 0;
        buf[pos++] = '[';
        for (int i = 0; i < arr->count && pos < buf_size - 32; i++) {
            if (i > 0) { buf[pos++] = ','; buf[pos++] = ' '; }
            Value elem = arr->elements[i];
            if (IS_INT(elem)) {
                pos += snprintf(buf + pos, buf_size - pos, "%" PRId64, AS_INT(elem));
            } else if (IS_NUMERIC(elem)) {
                double en = NUMERIC_AS_DOUBLE(elem);
                if (en == (long long)en && en >= -9007199254740992.0 && en <= 9007199254740992.0) {
                    pos += snprintf(buf + pos, buf_size - pos, "%lld.0", (long long)en);
                } else {
                    pos += snprintf(buf + pos, buf_size - pos, "%g", en);
                }
            } else if (IS_STRING(elem)) {
                pos += snprintf(buf + pos, buf_size - pos, "%s", AS_STRING(elem));
            } else if (IS_BOOL(elem)) {
                pos += snprintf(buf + pos, buf_size - pos, "%s", AS_BOOL(elem) ? "true" : "false");
            } else if (elem.type == VAL_NIL) {
                pos += snprintf(buf + pos, buf_size - pos, "nil");
            } else {
                pos += snprintf(buf + pos, buf_size - pos, "<%d>", elem.type);
            }
        }
        if (arr->count > 0 && pos >= buf_size - 32) {
            pos += snprintf(buf + pos, buf_size - pos, "...");
        }
        buf[pos++] = ']';
        buf[pos] = '\0';
        return val_string_take(buf);
    }
    if (args[0].type == VAL_NIL) {
        return val_string("nil");
    }

    if (args[0].type == VAL_INSTANCE && args[0].as.instance->class_def) {
        Method* str_method = class_find_method(args[0].as.instance->class_def, "__str__", 7);
        if (str_method) {
            Stmt* method_node = (Stmt*)str_method->method_stmt;
            ProcStmt* str_stmt = (method_node->type == STMT_ASYNC_PROC) ? &method_node->as.async_proc : &method_node->as.proc;
            Env* def_env = args[0].as.instance->class_def->defining_env;
            Env* str_env = env_create(def_env ? def_env : g_global_env);
            env_define(str_env, "self", 4, args[0]);
            ExecResult str_res = interpret(str_stmt->body, str_env);
            if (!str_res.is_throwing && str_res.value.type == VAL_STRING) {
                return str_res.value;
            }
        }
    }

    // For other types, return a type description
    const char* type_names[] = {"int","number","bool","nil","string","function","native",
                                "array","dict","tuple","class","instance","module",
                                "exception","generator","clib","pointer","vm_program","thread","mutex", "bytes"};
    int type = args[0].type;
    if (type >= 0 && type <= 20) {
        if (type == VAL_CLASS) {
            snprintf(buffer, sizeof(buffer), "<class %s>", args[0].as.class_val->name);
        } else if (type == VAL_INSTANCE) {
            snprintf(buffer, sizeof(buffer), "<instance of %s>", args[0].as.instance->class_def->name);
        } else if (type == VAL_MODULE) {
            snprintf(buffer, sizeof(buffer), "<module %s>", args[0].as.module->module->name);
        } else {
            snprintf(buffer, sizeof(buffer), "<%s>", type_names[type]);
        }
    } else {
        snprintf(buffer, sizeof(buffer), "<unknown:%d>", type);
    }
    return val_string(buffer);
}

static Value len_native(int argCount, Value* args) {
    if (argCount != 1) return val_nil();
    if (args[0].type == VAL_ARRAY) {
        return val_int(args[0].as.array->count);
    }
    if (args[0].type == VAL_STRING) {
        return val_int((int64_t)strlen(AS_STRING(args[0])));
    }
    if (args[0].type == VAL_TUPLE) {
        return val_int(args[0].as.tuple->count);
    }
    if (args[0].type == VAL_DICT) {
        return val_int(args[0].as.dict->count);
    }
    if (args[0].type == VAL_BYTES) {
        return val_int(args[0].as.bytes->length);
    }
    return val_nil();
}

static Value push_native(int argCount, Value* args) {
    if (argCount != 2) return val_nil();
    if (args[0].type != VAL_ARRAY) return val_nil();
    array_push(&args[0], args[1]);
    return val_nil();
}

// array_extend(target, source) - append all elements of source to target (native speed)
static Value array_extend_native(int argCount, Value* args) {
    if (argCount != 2 || args[0].type != VAL_ARRAY || args[1].type != VAL_ARRAY) return val_nil();
    ArrayValue* target = args[0].as.array;
    ArrayValue* source = args[1].as.array;
    int new_count = target->count + source->count;
    if (new_count > target->capacity) {
        size_t old_bytes = sizeof(Value) * (size_t)target->capacity;
        while (target->capacity < new_count) target->capacity = target->capacity == 0 ? 8 : target->capacity * 2;
        target->elements = SAGE_REALLOC(target->elements, sizeof(Value) * target->capacity);
        gc_track_external_resize(old_bytes, sizeof(Value) * (size_t)target->capacity);
    }
    memcpy(target->elements + target->count, source->elements, sizeof(Value) * source->count);
    target->count = new_count;
    return val_nil();
}

static Value pop_native(int argCount, Value* args) {
    if (argCount != 1) return val_nil();
    if (args[0].type != VAL_ARRAY) return val_nil();
    
    ArrayValue* a = args[0].as.array;
    if (a->count == 0) return val_nil();
    
    Value result = a->elements[a->count - 1];
    a->count--;
    return result;
}

// build_quad_verts(quads_array) -> flat float array of vertices
// Each quad is a dict with x,y,w,h,color (array of 4 floats)
// Output: 6 verts per quad, each vert = [px,py,u,v,r,g,b,a] = 8 floats
static Value build_quad_verts_native(int argCount, Value* args) {
    if (argCount != 1 || args[0].type != VAL_ARRAY) return val_nil();
    ArrayValue* quads = args[0].as.array;
    int quad_count = quads->count;
    int vert_count = quad_count * 6;
    int float_count = vert_count * 8;

    // Pre-allocate output array
    Value out_val = val_array();
    ArrayValue* out = out_val.as.array;
    out->count = 0;
    out->capacity = float_count;
    out->elements = SAGE_ALLOC(sizeof(Value) * float_count);
    gc_track_external_allocation(sizeof(Value) * (size_t)float_count);

    for (int i = 0; i < quad_count; i++) {
        Value q = quads->elements[i];
        if (q.type != VAL_DICT) continue;

        // Extract quad properties via dict_get
        Value vx = dict_get(&q, "x");
        Value vy = dict_get(&q, "y");
        Value vw = dict_get(&q, "w");
        Value vh = dict_get(&q, "h");
        Value vc = dict_get(&q, "color");
        if (!IS_NUMERIC(vx) || !IS_NUMERIC(vy)) continue;

        double x0 = NUMERIC_AS_DOUBLE(vx), y0 = NUMERIC_AS_DOUBLE(vy);
        double w = IS_NUMERIC(vw) ? NUMERIC_AS_DOUBLE(vw) : 0;
        double h = IS_NUMERIC(vh) ? NUMERIC_AS_DOUBLE(vh) : 0;
        double x1 = x0 + w, y1 = y0 + h;

        double cr = 1, cg = 1, cb = 1, ca = 1;
        if (vc.type == VAL_ARRAY && vc.as.array->count >= 4) {
            cr = NUMERIC_AS_DOUBLE(vc.as.array->elements[0]);
            cg = NUMERIC_AS_DOUBLE(vc.as.array->elements[1]);
            cb = NUMERIC_AS_DOUBLE(vc.as.array->elements[2]);
            ca = NUMERIC_AS_DOUBLE(vc.as.array->elements[3]);
        }

        // 6 vertices per quad (2 triangles)
        // Vertex = px,py,u,v,r,g,b,a
        #define EMIT_VERT(px,py,u,v) do { \
            out->elements[out->count++] = val_number(px); \
            out->elements[out->count++] = val_number(py); \
            out->elements[out->count++] = val_number(u);  \
            out->elements[out->count++] = val_number(v);  \
            out->elements[out->count++] = val_number(cr);  \
            out->elements[out->count++] = val_number(cg);  \
            out->elements[out->count++] = val_number(cb);  \
            out->elements[out->count++] = val_number(ca);  \
        } while(0)

        EMIT_VERT(x0, y0, 0, 0);
        EMIT_VERT(x1, y0, 1, 0);
        EMIT_VERT(x1, y1, 1, 1);
        EMIT_VERT(x0, y0, 0, 0);
        EMIT_VERT(x1, y1, 1, 1);
        EMIT_VERT(x0, y1, 0, 1);
        #undef EMIT_VERT
    }

    Value result;
    result.type = VAL_ARRAY;
    result.as.array = out;
    return result;
}

// build_line_quads(line_verts, thickness, color_r, color_g, color_b, color_a) -> quad array
// Takes line segments [x1,y1,x2,y2,...] and produces quads suitable for build_quad_verts
static Value build_line_quads_native(int argCount, Value* args) {
    if (argCount < 2 || args[0].type != VAL_ARRAY) return val_nil();
    ArrayValue* lines = args[0].as.array;
    double thickness = NUMERIC_AS_DOUBLE(args[1]);
    double cr = argCount > 2 ? NUMERIC_AS_DOUBLE(args[2]) : 1.0;
    double cg = argCount > 3 ? NUMERIC_AS_DOUBLE(args[3]) : 1.0;
    double cb = argCount > 4 ? NUMERIC_AS_DOUBLE(args[4]) : 1.0;
    double ca = argCount > 5 ? NUMERIC_AS_DOUBLE(args[5]) : 1.0;

    int seg_count = lines->count / 4;
    // Output: array of dicts, each with x,y,w,h,color
    Value out_val = val_array();
    ArrayValue* out = out_val.as.array;
    out->count = 0;
    out->capacity = seg_count;
    out->elements = SAGE_ALLOC(sizeof(Value) * (size_t)seg_count);
    gc_track_external_allocation(sizeof(Value) * (size_t)seg_count);

    // Color array (shared)
    Value color_val = val_array();
    ArrayValue* color = color_val.as.array;
    color->count = 4;
    color->capacity = 4;
    color->elements = SAGE_ALLOC(sizeof(Value) * 4);
    gc_track_external_allocation(sizeof(Value) * 4);
    color->elements[0] = val_number(cr);
    color->elements[1] = val_number(cg);
    color->elements[2] = val_number(cb);
    color->elements[3] = val_number(ca);

    double half = thickness * 0.5;

    for (int i = 0; i + 3 < lines->count; i += 4) {
        double x1 = NUMERIC_AS_DOUBLE(lines->elements[i]);
        double y1 = NUMERIC_AS_DOUBLE(lines->elements[i+1]);
        double x2 = NUMERIC_AS_DOUBLE(lines->elements[i+2]);
        double y2 = NUMERIC_AS_DOUBLE(lines->elements[i+3]);

        // For each line, create an axis-aligned bounding quad
        double minx = x1 < x2 ? x1 : x2;
        double miny = y1 < y2 ? y1 : y2;
        double maxx = x1 > x2 ? x1 : x2;
        double maxy = y1 > y2 ? y1 : y2;
        double w = maxx - minx;
        double h = maxy - miny;
        if (w < thickness) { minx -= half; w = thickness; }
        if (h < thickness) { miny -= half; h = thickness; }

        Value quad_dict = val_dict();
        dict_set(&quad_dict, "x", val_number(minx));
        dict_set(&quad_dict, "y", val_number(miny));
        dict_set(&quad_dict, "w", val_number(w));
        dict_set(&quad_dict, "h", val_number(h));
        dict_set(&quad_dict, "color", color_val);

        if (out->count >= out->capacity) {
            size_t old_cap = (size_t)out->capacity;
            out->capacity = out->capacity * 2 + 1;
            out->elements = SAGE_REALLOC(out->elements, sizeof(Value) * (size_t)out->capacity);
            gc_track_external_resize(sizeof(Value) * old_cap, sizeof(Value) * (size_t)out->capacity);
        }
        out->elements[out->count++] = quad_dict;
    }

    return out_val;
}

static Value range_native(int argCount, Value* args) {
    if (argCount < 1 || argCount > 2) return val_nil();
    
    int start = 0, end = 0;
    
    if (argCount == 1) {
        if (!IS_NUMERIC(args[0])) return val_nil();
        end = (int)NUMERIC_AS_INT(args[0]);
    } else {
        if (!IS_NUMERIC(args[0]) || !IS_NUMERIC(args[1])) return val_nil();
        start = (int)NUMERIC_AS_INT(args[0]);
        end = (int)NUMERIC_AS_INT(args[1]);
    }

    Value arr = val_array();
    for (int i = start; i < end; i++) {
        array_push(&arr, val_int(i));
    }
    return arr;
}

// String functions
static Value split_native(int argCount, Value* args) {
    if (argCount != 2) return val_nil();
    if (!IS_STRING(args[0]) || !IS_STRING(args[1])) return val_nil();
    return string_split(AS_STRING(args[0]), AS_STRING(args[1]));
}

static Value join_native(int argCount, Value* args) {
    if (argCount != 2) return val_nil();
    if (!IS_ARRAY(args[0]) || !IS_STRING(args[1])) return val_nil();
    return string_join(&args[0], AS_STRING(args[1]));
}

static Value replace_native(int argCount, Value* args) {
    if (argCount != 3) return val_nil();
    if (!IS_STRING(args[0]) || !IS_STRING(args[1]) || !IS_STRING(args[2])) return val_nil();
    char* result = string_replace(AS_STRING(args[0]), AS_STRING(args[1]), AS_STRING(args[2]));
    return val_string_take(result);
}

static Value upper_native(int argCount, Value* args) {
    if (argCount != 1) return val_nil();
    if (!IS_STRING(args[0])) return val_nil();
    char* result = string_upper(AS_STRING(args[0]));
    return val_string_take(result);
}

static Value lower_native(int argCount, Value* args) {
    if (argCount != 1) return val_nil();
    if (!IS_STRING(args[0])) return val_nil();
    char* result = string_lower(AS_STRING(args[0]));
    return val_string_take(result);
}

static Value strip_native(int argCount, Value* args) {
    if (argCount != 1) return val_nil();
    if (!IS_STRING(args[0])) return val_nil();
    char* result = string_strip(AS_STRING(args[0]));
    return val_string_take(result);
}

// type(val) -> string name of type
static Value type_native(int argCount, Value* args) {
    if (argCount != 1) return val_nil();
    switch (args[0].type) {
        case VAL_NIL: return val_string("nil");
        case VAL_INT: return val_string("int");
        case VAL_NUMBER: return val_string("float");
        case VAL_BOOL: return val_string("bool");
        case VAL_STRING: return val_string("string");
        case VAL_ARRAY: return val_string("array");
        case VAL_DICT: return val_string("dict");
        case VAL_FUNCTION: return val_string("function");
        case VAL_NATIVE: return val_string("native");
        case VAL_INSTANCE: return val_string("instance");
        case VAL_TUPLE: return val_string("tuple");
        case VAL_GENERATOR: return val_string("generator");
        default: return val_string("unknown");
    }
}

// chr(n) -> single-character string from ASCII code
static Value chr_native(int argCount, Value* args) {
    if (argCount != 1 || !IS_NUMERIC(args[0])) return val_nil();
    int code = (int)NUMERIC_AS_INT(args[0]);
    if (code < 0 || code > 127) return val_nil();
    if (code == 0) return val_string("");  // Null byte returns empty string (C strings are null-terminated)
    char* s = SAGE_ALLOC(2);
    s[0] = (char)code;
    s[1] = '\0';
    return val_string_take(s);
}

// ord(s) -> ASCII code of first character
static Value ord_native(int argCount, Value* args) {
    if (argCount != 1 || !IS_STRING(args[0])) return val_nil();
    char* s = AS_STRING(args[0]);
    if (s[0] == '\0') return val_nil();
    return val_int((int64_t)(unsigned char)s[0]);
}

// startswith(s, prefix) -> bool
static Value startswith_native(int argCount, Value* args) {
    if (argCount != 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) return val_nil();
    char* s = AS_STRING(args[0]);
    char* prefix = AS_STRING(args[1]);
    size_t plen = strlen(prefix);
    return val_bool(strncmp(s, prefix, plen) == 0);
}

// endswith(s, suffix) -> bool
static Value endswith_native(int argCount, Value* args) {
    if (argCount != 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) return val_nil();
    char* s = AS_STRING(args[0]);
    char* suffix = AS_STRING(args[1]);
    size_t slen = strlen(s);
    size_t suflen = strlen(suffix);
    if (suflen > slen) return val_bool(0);
    return val_bool(strcmp(s + slen - suflen, suffix) == 0);
}

// contains(s, sub) -> bool
static Value contains_native(int argCount, Value* args) {
    if (argCount != 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) return val_nil();
    return val_bool(strstr(AS_STRING(args[0]), AS_STRING(args[1])) != NULL);
}

// indexof(s, sub) -> number (-1 if not found)
static Value indexof_native(int argCount, Value* args) {
    if (argCount != 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) return val_nil();
    char* s = AS_STRING(args[0]);
    char* sub = AS_STRING(args[1]);
    char* found = strstr(s, sub);
    if (found == NULL) return val_number(-1);
    return val_int((int64_t)(found - s));
}

static Value slice_native(int argCount, Value* args) {
    if (argCount != 3) return val_nil();
    if (!IS_NUMERIC(args[1]) || !IS_NUMERIC(args[2])) return val_nil();
    int start = (int)NUMERIC_AS_INT(args[1]);
    int end = (int)NUMERIC_AS_INT(args[2]);
    if (IS_ARRAY(args[0])) return array_slice(&args[0], start, end);
    if (IS_STRING(args[0])) return string_slice(&args[0], start, end);
    return val_nil();
}

// Dictionary functions
static Value dict_keys_native(int argCount, Value* args) {
    if (argCount != 1) return val_nil();
    if (!IS_DICT(args[0])) return val_nil();
    return dict_keys(&args[0]);
}

static Value dict_values_native(int argCount, Value* args) {
    if (argCount != 1) return val_nil();
    if (!IS_DICT(args[0])) return val_nil();
    return dict_values(&args[0]);
}

static Value dict_has_native(int argCount, Value* args) {
    if (argCount != 2) return val_nil();
    if (!IS_DICT(args[0]) || !IS_STRING(args[1])) return val_nil();
    return val_bool(dict_has(&args[0], AS_STRING(args[1])));
}

static Value dict_delete_native(int argCount, Value* args) {
    if (argCount != 2) return val_nil();
    if (!IS_DICT(args[0]) || !IS_STRING(args[1])) return val_nil();
    dict_delete(&args[0], AS_STRING(args[1]));
    return val_nil();
}

// GC functions
// ── Phase 2: Option / Result constructors ────────────────────────────────
// These produce tagged dicts recognized by the match interpreter and
// the !, ?, and ?? operators.

static Value sage_Some_native(int argc, Value* args) {
    if (argc < 1) return val_nil();
    gc_pin();
    Value d = val_dict();
    dict_set(&d, "__type", val_string("option.some"));
    dict_set(&d, "__val",  args[0]);
    gc_unpin();
    return d;
}

static Value sage_Ok_native(int argc, Value* args) {
    gc_pin();
    Value d = val_dict();
    dict_set(&d, "__type", val_string("result.ok"));
    dict_set(&d, "__val",  argc > 0 ? args[0] : val_nil());
    gc_unpin();
    return d;
}

static Value sage_Err_native(int argc, Value* args) {
    gc_pin();
    Value d = val_dict();
    dict_set(&d, "__type", val_string("result.err"));
    dict_set(&d, "__val",  argc > 0 ? args[0] : val_nil());
    gc_unpin();
    return d;
}

static Value gc_collect_native(int argCount, Value* args) {
    gc_collect();
    return val_nil();
}

static Value gc_stats_native(int argCount, Value* args) {
    GCStats stats = gc_get_stats();
    gc_pin();
    Value dict = val_dict();
    
    dict_set(&dict, "bytes_allocated", val_number(stats.bytes_allocated));
    dict_set(&dict, "current_bytes", val_number(stats.current_bytes));
    dict_set(&dict, "num_objects", val_number(stats.num_objects));
    dict_set(&dict, "collections", val_int((int64_t)stats.collections));
    dict_set(&dict, "objects_freed", val_number(stats.objects_freed));
    dict_set(&dict, "next_gc", val_number(stats.next_gc));
    dict_set(&dict, "next_gc_bytes", val_number(stats.next_gc_bytes));
    
    gc_unpin();
    return dict;
}

static Value gc_collections_native(int argCount, Value* args) {
    GCStats stats = gc_get_stats();
    return val_int((int64_t)stats.collections);
}

static Value gc_enable_native(int argCount, Value* args) {
    gc_enable();
    return val_nil();
}

static Value gc_disable_native(int argCount, Value* args) {
    gc_disable();
    return val_nil();
}

static Value gc_mode_native(int argCount, Value* args) {
    (void)argCount; (void)args;
    if (gc.mode == GC_MODE_ORC) return val_string("orc");
    if (gc.mode == GC_MODE_ARC) return val_string("arc");
    return val_string("tracing");
}

static Value gc_set_arc_native(int argCount, Value* args) {
    (void)argCount; (void)args;
    gc_set_mode(GC_MODE_ARC);
    return val_nil();
}

static Value gc_set_orc_native(int argCount, Value* args) {
    (void)argCount; (void)args;
    gc_set_mode(GC_MODE_ORC);
    return val_nil();
}

// CPU topology / SMP detection natives
static Value cpu_count_native(int argCount, Value* args) {
    (void)argCount; (void)args;
    return val_int((int64_t)sage_cpu_count());
}

static Value cpu_physical_cores_native(int argCount, Value* args) {
    (void)argCount; (void)args;
    return val_number((double)sage_cpu_physical_cores());
}

static Value cpu_has_hyperthreading_native(int argCount, Value* args) {
    (void)argCount; (void)args;
    return val_bool(sage_cpu_has_hyperthreading());
}

static Value thread_set_affinity_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_bool(0);
    int core_id = (int)NUMERIC_AS_INT(args[0]);
    return val_int((int64_t)sage_thread_set_affinity(core_id));
}

static Value thread_get_core_native(int argCount, Value* args) {
    (void)argCount; (void)args;
    return val_number((double)sage_thread_get_core());
}

// Atomic operations (C-level, truly atomic)
static Value atomic_new_native(int argCount, Value* args) {
    sage_atomic_t* a = SAGE_ALLOC(sizeof(sage_atomic_t));
    a->value = (argCount >= 1 && IS_NUMERIC(args[0])) ? (long)NUMERIC_AS_INT(args[0]) : 0;
    return val_pointer(a, sizeof(sage_atomic_t), 1);
}
static Value atomic_load_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_POINTER(args[0])) return val_nil();
    sage_atomic_t* a = (sage_atomic_t*)args[0].as.pointer->ptr;
    return val_number((double)sage_atomic_load(a));
}
static Value atomic_store_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_POINTER(args[0]) || !IS_NUMERIC(args[1])) return val_nil();
    sage_atomic_t* a = (sage_atomic_t*)args[0].as.pointer->ptr;
    sage_atomic_store(a, (long)NUMERIC_AS_INT(args[1]));
    return val_nil();
}
static Value atomic_add_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_POINTER(args[0]) || !IS_NUMERIC(args[1])) return val_nil();
    sage_atomic_t* a = (sage_atomic_t*)args[0].as.pointer->ptr;
    return val_number((double)sage_atomic_add(a, (long)NUMERIC_AS_INT(args[1])));
}
static Value atomic_cas_native(int argCount, Value* args) {
    if (argCount < 3 || !IS_POINTER(args[0]) || !IS_NUMERIC(args[1]) || !IS_NUMERIC(args[2])) return val_bool(0);
    sage_atomic_t* a = (sage_atomic_t*)args[0].as.pointer->ptr;
    return val_bool(sage_atomic_cas(a, (long)NUMERIC_AS_INT(args[1]), (long)NUMERIC_AS_INT(args[2])));
}
static Value atomic_exchange_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_POINTER(args[0]) || !IS_NUMERIC(args[1])) return val_nil();
    sage_atomic_t* a = (sage_atomic_t*)args[0].as.pointer->ptr;
    return val_number((double)sage_atomic_exchange(a, (long)NUMERIC_AS_INT(args[1])));
}

// Semaphore natives
static Value sem_new_native(int argCount, Value* args) {
    sage_sem_t* sem = SAGE_ALLOC(sizeof(sage_sem_t));
    int initial = (argCount >= 1 && IS_NUMERIC(args[0])) ? (int)NUMERIC_AS_INT(args[0]) : 1;
    sage_sem_init(sem, initial);
    return val_pointer(sem, sizeof(sage_sem_t), 1);
}
static Value sem_wait_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_POINTER(args[0])) return val_nil();
    sage_sem_wait((sage_sem_t*)args[0].as.pointer->ptr);
    return val_nil();
}
static Value sem_post_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_POINTER(args[0])) return val_nil();
    sage_sem_post((sage_sem_t*)args[0].as.pointer->ptr);
    return val_nil();
}
static Value sem_trywait_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_POINTER(args[0])) return val_bool(0);
    return val_bool(sage_sem_trywait((sage_sem_t*)args[0].as.pointer->ptr) == 0);
}

// PHASE 7: Generator next() function - Forward declaration (REMOVED static keyword)
ExecResult interpret(Stmt* stmt, Env* env);

static Value native_next(int arg_count, Value* args) {
    if (arg_count != 1) {
        fprintf(stderr, "next() expects 1 argument\n");
        sage_error_exit();
    }
    if (!IS_GENERATOR(args[0])) {
        fprintf(stderr, "next() expects a generator\n");
        sage_error_exit();
    }
    
    GeneratorValue* gen = AS_GENERATOR(args[0]);
    if (gen->is_exhausted) return val_nil();
    
    // Initialize generator environment on first call
    if (!gen->is_started) {
        gen->gen_env = env_create(gen->closure);
        gen->is_started = 1;
    }

    if (gen->has_resume_target && gen->current_stmt == NULL) {
        gen->is_exhausted = 1;
        return val_nil();
    }

    g_generator_resume_target = gen->has_resume_target ? (Stmt*)gen->current_stmt : NULL;
    ExecResult result = interpret((Stmt*)gen->body, gen->gen_env);
    g_generator_resume_target = NULL;
    
    if (result.is_yielding) {
        gen->current_stmt = result.next_stmt;
        gen->has_resume_target = 1;
        return result.value;
    }
    
    if (result.is_returning) {
        gen->is_exhausted = 1;
        return result.value;
    }
    
    if (result.is_throwing) {
        gen->is_exhausted = 1;
        fprintf(stderr, "Exception in generator\n");
        sage_error_exit();
    }
    
    // Generator completed without yielding or returning
    gen->is_exhausted = 1;
    gen->has_resume_target = 0;
    return val_nil();
}

// ============================================================================
// Phase 9: FFI Functions (requires dlfcn.h - disabled with SAGE_NO_FFI)
// ============================================================================

#ifndef SAGE_NO_FFI

#endif
// ffi_open("libname.so") -> CLib handle
Value ffi_open_native(int argCount, Value* args) {
    if (argCount != 1 || !IS_STRING(args[0])) {
        fprintf(stderr, "ffi_open() expects 1 string argument (library path).\n");
        return val_nil();
    }
    const char* lib_name = AS_STRING(args[0]);
    void* handle = dlopen(lib_name, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "ffi_open: %s\n", dlerror());
        return val_nil();
    }
    return val_clib(handle, lib_name);
}

// ffi_close(lib) -> nil
Value ffi_close_native(int argCount, Value* args) {
    if (argCount != 1 || !IS_CLIB(args[0])) {
        fprintf(stderr, "ffi_close() expects 1 clib argument.\n");
        return val_nil();
    }
    CLibValue* lib = AS_CLIB(args[0]);
    if (lib->handle) {
        dlclose(lib->handle);
        lib->handle = NULL;
    }
    return val_nil();
}

// ffi_call(lib, "func_name", "return_type", [args...])
// Supported return types: "double", "int", "void", "string"
// Args are automatically marshaled from Sage values
Value ffi_call_native(int argCount, Value* args) {
    if (argCount < 3 || argCount > 4) {
        fprintf(stderr, "ffi_call() expects 3-4 arguments: (lib, func_name, return_type, [args]).\n");
        return val_nil();
    }
    if (!IS_CLIB(args[0])) {
        fprintf(stderr, "ffi_call(): first argument must be a clib handle.\n");
        return val_nil();
    }
    if (!IS_STRING(args[1])) {
        fprintf(stderr, "ffi_call(): second argument must be function name string.\n");
        return val_nil();
    }
    if (!IS_STRING(args[2])) {
        fprintf(stderr, "ffi_call(): third argument must be return type string.\n");
        return val_nil();
    }

    CLibValue* lib = AS_CLIB(args[0]);
    if (!lib->handle) {
        fprintf(stderr, "ffi_call(): library handle is closed.\n");
        return val_nil();
    }

    const char* func_name = AS_STRING(args[1]);
    const char* ret_type = AS_STRING(args[2]);

    // Look up symbol
    dlerror(); // Clear errors
    void* sym = dlsym(lib->handle, func_name);
    char* error = dlerror();
    if (error) {
        fprintf(stderr, "ffi_call: %s\n", error);
        return val_nil();
    }

    // Get args array
    int call_argc = 0;
    ArrayValue* call_args = NULL;
    if (argCount == 4) {
        if (!IS_ARRAY(args[3])) {
            fprintf(stderr, "ffi_call(): fourth argument must be an array of arguments.\n");
            return val_nil();
        }
        call_args = AS_ARRAY(args[3]);
        call_argc = call_args->count;
    }

    if (call_argc > 16) {
        fprintf(stderr, "ffi_call(): maximum 16 arguments supported (got %d).\n", call_argc);
        return val_nil();
    }

#ifdef SAGE_HAS_FFI
    // P15: Use libffi for proper vararg/mixed-type support
    
    ffi_cif cif;
    ffi_type* arg_types[16];
    void* arg_values[16];
    
    // Storage for converted values
    int64_t int_vals[16];
    double dbl_vals[16];
    const char* str_vals[16];
    
    for (int i = 0; i < call_argc; i++) {
        Value v = call_args->elements[i];
        if (IS_INT(v)) {
            int_vals[i] = AS_INT(v);
            arg_types[i] = &ffi_type_sint64;
            arg_values[i] = &int_vals[i];
        } else if (IS_NUMBER(v)) {
            dbl_vals[i] = AS_NUMBER(v);
            arg_types[i] = &ffi_type_double;
            arg_values[i] = &dbl_vals[i];
        } else if (IS_STRING(v)) {
            str_vals[i] = AS_STRING(v);
            arg_types[i] = &ffi_type_pointer;
            arg_values[i] = &str_vals[i];
        } else if (IS_POINTER(v) && v.as.pointer) {
            str_vals[i] = (const char*)v.as.pointer->ptr;
            arg_types[i] = &ffi_type_pointer;
            arg_values[i] = &str_vals[i];
        } else {
            int_vals[i] = 0;
            arg_types[i] = &ffi_type_sint64;
            arg_values[i] = &int_vals[i];
        }
    }
    
    ffi_type* rtype;
    if (strcmp(ret_type, "double") == 0 || strcmp(ret_type, "float") == 0)
        rtype = &ffi_type_double;
    else if (strcmp(ret_type, "int") == 0)
        rtype = &ffi_type_sint32;
    else if (strcmp(ret_type, "long") == 0 || strcmp(ret_type, "int64") == 0)
        rtype = &ffi_type_sint64;
    else if (strcmp(ret_type, "string") == 0 || strcmp(ret_type, "pointer") == 0)
        rtype = &ffi_type_pointer;
    else if (strcmp(ret_type, "void") == 0)
        rtype = &ffi_type_void;
    else {
        fprintf(stderr, "ffi_call: unknown return type '%s'. Use: double, int, long, string, pointer, void.\n", ret_type);
        return val_nil();
    }
    
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, call_argc, rtype, arg_types) != FFI_OK) {
        fprintf(stderr, "ffi_call: failed to prepare call interface.\n");
        return val_nil();
    }
    
    if (rtype == &ffi_type_double) {
        double result;
        ffi_call(&cif, FFI_FN(sym), &result, arg_values);
        return val_number(result);
    } else if (rtype == &ffi_type_sint32) {
        int result;
        ffi_call(&cif, FFI_FN(sym), &result, arg_values);
        return val_int((int64_t)result);
    } else if (rtype == &ffi_type_sint64) {
        int64_t result;
        ffi_call(&cif, FFI_FN(sym), &result, arg_values);
        return val_int(result);
    } else if (rtype == &ffi_type_pointer) {
        void* result;
        ffi_call(&cif, FFI_FN(sym), &result, arg_values);
        if (strcmp(ret_type, "string") == 0) {
            return result ? val_string((const char*)result) : val_nil();
        }
        // Return raw pointer
        Value pval;
        pval.type = VAL_POINTER;
        PointerValue* pv = SAGE_ALLOC(sizeof(PointerValue));
        pv->ptr = result; pv->size = 0; pv->type_tag = 0; pv->owned = 0;
        pval.as.pointer = pv;
        return pval;
    } else {
        // void return
        ffi_call(&cif, FFI_FN(sym), NULL, arg_values);
        return val_nil();
    }
    
#else
    // Fallback without libffi -- limited hardcoded dispatch
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wpedantic"

    if (strcmp(ret_type, "double") == 0) {
        if (call_argc == 1 && IS_NUMERIC(call_args->elements[0])) {
            double (*fn)(double) = (double (*)(double))sym;
            return val_number(fn(NUMERIC_AS_DOUBLE(call_args->elements[0])));
        } else if (call_argc == 2 && IS_NUMERIC(call_args->elements[0]) && IS_NUMERIC(call_args->elements[1])) {
            double (*fn)(double, double) = (double (*)(double, double))sym;
            return val_number(fn(NUMERIC_AS_DOUBLE(call_args->elements[0]), NUMERIC_AS_DOUBLE(call_args->elements[1])));
        }
    }
    if (strcmp(ret_type, "int") == 0) {
        if (call_argc == 1 && IS_STRING(call_args->elements[0])) {
            int (*fn)(const char*) = (int (*)(const char*))sym;
            return val_int((int64_t)fn(AS_STRING(call_args->elements[0])));
        } else if (call_argc == 1 && IS_NUMERIC(call_args->elements[0])) {
            int (*fn)(int) = (int (*)(int))sym;
            return val_int((int64_t)fn((int)NUMERIC_AS_INT(call_args->elements[0])));
        }
    }
    if (strcmp(ret_type, "void") == 0 && call_argc == 0) {
        void (*fn)(void) = (void (*)(void))sym;
        fn();
        return val_nil();
    }

    #pragma GCC diagnostic pop
    fprintf(stderr, "ffi_call: unsupported signature (build with libffi for full support).\n");
    return val_nil();
#endif
}

// ffi_sym(lib, "symbol_name") -> true/false (check if symbol exists)
Value ffi_sym_native(int argCount, Value* args) {
    if (argCount != 2 || !IS_CLIB(args[0]) || !IS_STRING(args[1])) {
        fprintf(stderr, "ffi_sym() expects (clib, string).\n");
        return val_bool(0);
    }
    CLibValue* lib = AS_CLIB(args[0]);
    if (!lib->handle) return val_bool(0);

    dlerror();
    dlsym(lib->handle, AS_STRING(args[1]));
    return val_bool(dlerror() == NULL);
}


// ========== Phase 9: Raw Memory Operations ==========

// mem_alloc(size) -> pointer
// Phase 1.8: Bytes operations
static Value bytes_new_native(int argCount, Value* args) {
    if (argCount == 0) return val_bytes(NULL, 0);
    if (argCount == 1 && IS_NUMERIC(args[0])) {
        return val_bytes_empty((int)NUMERIC_AS_INT(args[0]));
    }
    if (argCount == 1 && args[0].type == VAL_STRING) {
        const char* s = AS_STRING(args[0]);
        return val_bytes((const unsigned char*)s, (int)strlen(s));
    }
    if (argCount == 1 && args[0].type == VAL_ARRAY) {
        ArrayValue* arr = args[0].as.array;
        Value b = val_bytes_empty(arr->count);
        for (int i = 0; i < arr->count; i++) {
            if (IS_NUMERIC(arr->elements[i])) {
                bytes_push(&b, (unsigned char)(int)NUMERIC_AS_INT(arr->elements[i]));
            }
        }
        return b;
    }
    return val_bytes(NULL, 0);
}

static Value bytes_len_native(int argCount, Value* args) {
    if (argCount == 1 && args[0].type == VAL_BYTES) {
        return val_int(args[0].as.bytes->length);
    }
    return val_number(0);
}

static Value bytes_get_native(int argCount, Value* args) {
    if (argCount == 2 && args[0].type == VAL_BYTES && IS_NUMERIC(args[1])) {
        int idx = (int)NUMERIC_AS_INT(args[1]);
        BytesValue* b = args[0].as.bytes;
        if (idx < 0) idx += b->length;
        if (idx >= 0 && idx < b->length) {
            return val_int((int64_t)b->data[idx]);
        }
    }
    return val_nil();
}

static Value bytes_set_native(int argCount, Value* args) {
    if (argCount == 3 && args[0].type == VAL_BYTES && IS_NUMERIC(args[1]) && IS_NUMERIC(args[2])) {
        int idx = (int)NUMERIC_AS_INT(args[1]);
        BytesValue* b = args[0].as.bytes;
        if (idx >= 0 && idx < b->length) {
            b->data[idx] = (unsigned char)(int)NUMERIC_AS_INT(args[2]);
        }
    }
    return val_nil();
}

static Value bytes_to_string_native(int argCount, Value* args) {
    if (argCount == 1 && args[0].type == VAL_BYTES) {
        BytesValue* b = args[0].as.bytes;
        char* s = SAGE_ALLOC(b->length + 1);
        memcpy(s, b->data, b->length);
        s[b->length] = '\0';
        return val_string_take(s);
    }
    return val_string("");
}

static Value bytes_slice_native(int argCount, Value* args) {
    if (argCount >= 2 && args[0].type == VAL_BYTES && IS_NUMERIC(args[1])) {
        BytesValue* b = args[0].as.bytes;
        int start = (int)NUMERIC_AS_INT(args[1]);
        int end = (argCount >= 3 && IS_NUMERIC(args[2])) ? (int)NUMERIC_AS_INT(args[2]) : b->length;
        if (start < 0) start += b->length;
        if (end < 0) end += b->length;
        if (start < 0) start = 0;
        if (end > b->length) end = b->length;
        if (start >= end) return val_bytes(NULL, 0);
        return val_bytes(b->data + start, end - start);
    }
    return val_bytes(NULL, 0);
}

static Value bytes_push_native(int argCount, Value* args) {
    if (argCount == 2 && args[0].type == VAL_BYTES && IS_NUMERIC(args[1])) {
        bytes_push(&args[0], (unsigned char)(int)NUMERIC_AS_INT(args[1]));
    }
    return val_nil();
}

// Phase 1.8: sizeof builtin
static Value sizeof_native(int argCount, Value* args) {
    if (argCount != 1) return val_nil();
    switch (args[0].type) {
        case VAL_NUMBER: return val_int((int64_t)sizeof(double));
        case VAL_BOOL: return val_int((int64_t)sizeof(int));
        case VAL_STRING: return val_int((int64_t)strlen(AS_STRING(args[0])));
        case VAL_BYTES: return val_int(args[0].as.bytes->length);
        case VAL_ARRAY: return val_int(args[0].as.array->count);
        case VAL_DICT: return val_int(args[0].as.dict->count);
        case VAL_POINTER: return val_int((int64_t)args[0].as.pointer->size);
        default: return val_int((int64_t)sizeof(Value));
    }
}

// Phase 1.8: Pointer arithmetic
static Value ptr_add_native(int argCount, Value* args) {
    if (argCount == 2 && args[0].type == VAL_POINTER && IS_NUMERIC(args[1])) {
        PointerValue* p = args[0].as.pointer;
        int offset = (int)NUMERIC_AS_INT(args[1]);
        Value v;
        v.type = VAL_POINTER;
        v.as.pointer = gc_alloc(VAL_POINTER, sizeof(PointerValue));
        v.as.pointer->ptr = (char*)p->ptr + offset;
        v.as.pointer->size = (p->size > (size_t)offset) ? p->size - offset : 0;
        v.as.pointer->owned = 0;
        return v;
    }
    return val_nil();
}

static Value ptr_to_int_native(int argCount, Value* args) {
    if (argCount == 1 && args[0].type == VAL_POINTER) {
        return val_int((int64_t)(uintptr_t)args[0].as.pointer->ptr);
    }
    return val_nil();
}

static Value mem_alloc_native(int argCount, Value* args) {
    if (argCount != 1 || !IS_NUMERIC(args[0])) {
        fprintf(stderr, "mem_alloc() expects (number).\n");
        return val_nil();
    }
    size_t size = (size_t)NUMERIC_AS_DOUBLE(args[0]);
    if (size == 0 || size > 1024 * 1024 * 64) { // Cap at 64MB
        fprintf(stderr, "mem_alloc(): invalid size (0 < size <= 64MB).\n");
        return val_nil();
    }
    void* ptr = calloc(1, size); // Zero-initialized
    if (!ptr) {
        fprintf(stderr, "mem_alloc(): allocation failed.\n");
        return val_nil();
    }
    return val_pointer(ptr, size, 1);
}

// mem_free(ptr) -> nil
static Value mem_free_native(int argCount, Value* args) {
    if (argCount != 1 || !IS_POINTER(args[0])) {
        fprintf(stderr, "mem_free() expects (pointer).\n");
        return val_nil();
    }
    PointerValue* p = AS_POINTER(args[0]);
    if (p->ptr == NULL) {
        // P14: Double-free detection
        FireflyLoc loc = {0};
        firefly_set_code("E080");
        firefly_report(FIREFLY_ERROR, loc, "double free detected — pointer was already freed");
        firefly_explain("This pointer has already been freed. Freeing it again is undefined behavior.");
        firefly_advice("Remove the duplicate mem_free() call, or check ptr before freeing.");
        firefly_end();
        return val_nil();
    }
    if (p->owned) {
        free(p->ptr);
        p->ptr = NULL;
        p->size = 0;
        p->owned = 0;
    }
    return val_nil();
}

// mem_read(ptr, offset, type) -> value
// type: "byte", "int", "double", "string"
static Value mem_read_native(int argCount, Value* args) {
    if (argCount != 3 || !IS_POINTER(args[0]) || !IS_NUMERIC(args[1]) || !IS_STRING(args[2])) {
        fprintf(stderr, "mem_read() expects (pointer, offset, type_string).\n");
        return val_nil();
    }
    PointerValue* p = AS_POINTER(args[0]);
    if (!p->ptr) {
        FireflyLoc loc = {0};
        firefly_set_code("E081");
        firefly_report(FIREFLY_ERROR, loc, "use after free — reading from freed pointer");
        firefly_explain("This pointer has been freed. Reading from it is undefined behavior.");
        firefly_end();
        return val_nil();
    }
    double offset_d = NUMERIC_AS_DOUBLE(args[1]);
    if (offset_d < 0) {
        fprintf(stderr, "mem_read(): offset cannot be negative.\n");
        return val_nil();
    }
    size_t offset = (size_t)offset_d;
    const char* type = AS_STRING(args[2]);

    // Bounds checking for owned memory
    if (p->size > 0) {
        size_t needed = 0;
        if (strcmp(type, "byte") == 0) needed = 1;
        else if (strcmp(type, "int") == 0) needed = sizeof(int);
        else if (strcmp(type, "double") == 0) needed = sizeof(double);
        else if (strcmp(type, "string") == 0) needed = 1; // at least 1 byte
        if (offset + needed > p->size) {
            fprintf(stderr, "mem_read(): offset %zu + %zu bytes exceeds allocation size %zu.\n",
                    offset, needed, p->size);
            return val_nil();
        }
    }

    unsigned char* base = (unsigned char*)p->ptr + offset;

    if (strcmp(type, "byte") == 0) {
        return val_int((int64_t)*base);
    } else if (strcmp(type, "int") == 0) {
        int v;
        memcpy(&v, base, sizeof(int));
        return val_int((int64_t)v);
    } else if (strcmp(type, "double") == 0) {
        double val;
        memcpy(&val, base, sizeof(double));
        return val_number(val);
    } else if (strcmp(type, "string") == 0) {
        // Bounds-check: scan for null terminator within allocation
        if (p->size > 0) {
            size_t max_len = p->size - offset;
            int found_null = 0;
            for (size_t i = 0; i < max_len; i++) {
                if (base[i] == '\0') { found_null = 1; break; }
            }
            if (!found_null) {
                fprintf(stderr, "mem_read(): string not null-terminated within allocation.\n");
                return val_nil();
            }
        }
        return val_string((const char*)base);
    } else {
        fprintf(stderr, "mem_read(): unknown type '%s' (use byte/int/double/string).\n", type);
        return val_nil();
    }
}

// mem_write(ptr, offset, type, value) -> nil
static Value mem_write_native(int argCount, Value* args) {
    if (argCount != 4 || !IS_POINTER(args[0]) || !IS_NUMERIC(args[1]) || !IS_STRING(args[2])) {
        fprintf(stderr, "mem_write() expects (pointer, offset, type_string, value).\n");
        return val_nil();
    }
    PointerValue* p = AS_POINTER(args[0]);
    if (!p->ptr) {
        fprintf(stderr, "mem_write(): null pointer.\n");
        return val_nil();
    }
    double offset_d = NUMERIC_AS_DOUBLE(args[1]);
    if (offset_d < 0) {
        fprintf(stderr, "mem_write(): offset cannot be negative.\n");
        return val_nil();
    }
    size_t offset = (size_t)offset_d;
    const char* type = AS_STRING(args[2]);

    // Bounds checking for owned memory
    if (p->size > 0) {
        size_t needed = 0;
        if (strcmp(type, "byte") == 0) needed = 1;
        else if (strcmp(type, "int") == 0) needed = sizeof(int);
        else if (strcmp(type, "double") == 0) needed = sizeof(double);
        if (needed > 0 && offset + needed > p->size) {
            fprintf(stderr, "mem_write(): offset %zu + %zu bytes exceeds allocation size %zu.\n",
                    offset, needed, p->size);
            return val_nil();
        }
    }

    unsigned char* base = (unsigned char*)p->ptr + offset;

    if (strcmp(type, "byte") == 0) {
        if (!IS_NUMERIC(args[3])) {
            fprintf(stderr, "mem_write(): byte value must be a number.\n");
            return val_nil();
        }
        *base = (unsigned char)NUMERIC_AS_DOUBLE(args[3]);
    } else if (strcmp(type, "int") == 0) {
        if (!IS_NUMERIC(args[3])) {
            fprintf(stderr, "mem_write(): int value must be a number.\n");
            return val_nil();
        }
        int val = (int)NUMERIC_AS_INT(args[3]);
        memcpy(base, &val, sizeof(int));
    } else if (strcmp(type, "double") == 0) {
        if (!IS_NUMERIC(args[3])) {
            fprintf(stderr, "mem_write(): double value must be a number.\n");
            return val_nil();
        }
        double val = NUMERIC_AS_DOUBLE(args[3]);
        memcpy(base, &val, sizeof(double));
    } else {
        fprintf(stderr, "mem_write(): unknown type '%s' (use byte/int/double).\n", type);
    }
    return val_nil();
}

// mem_size(ptr) -> number
static Value mem_size_native(int argCount, Value* args) {
    if (argCount != 1 || !IS_POINTER(args[0])) {
        fprintf(stderr, "mem_size() expects (pointer).\n");
        return val_nil();
    }
    return val_int((int64_t)AS_POINTER(args[0])->size);
}

// addressof(value) -> number (address as integer, for inspection only)
static Value addressof_native(int argCount, Value* args) {
    if (argCount != 1) {
        fprintf(stderr, "addressof() expects (value).\n");
        return val_nil();
    }
    // Return address of the underlying data
    void* addr = NULL;
    switch (args[0].type) {
        case VAL_STRING:   addr = (void*)AS_STRING(args[0]); break;
        case VAL_ARRAY:    addr = (void*)AS_ARRAY(args[0]); break;
        case VAL_DICT:     addr = (void*)AS_DICT(args[0]); break;
        case VAL_POINTER:  addr = AS_POINTER(args[0])->ptr; break;
        case VAL_INSTANCE: addr = (void*)args[0].as.instance; break;
        default:           addr = (void*)&args[0]; break;
    }
    return val_number((double)(uintptr_t)addr);
}

// ========== Phase 9: C Struct Interop ==========

// Helper: get size and alignment for a C type string
static int struct_type_info(const char* type, size_t* out_size, size_t* out_align) {
    if (strcmp(type, "char") == 0 || strcmp(type, "byte") == 0) {
        *out_size = 1; *out_align = 1;
    } else if (strcmp(type, "short") == 0) {
        *out_size = sizeof(short); *out_align = sizeof(short);
    } else if (strcmp(type, "int") == 0) {
        *out_size = sizeof(int); *out_align = sizeof(int);
    } else if (strcmp(type, "long") == 0) {
        *out_size = sizeof(long); *out_align = sizeof(long);
    } else if (strcmp(type, "float") == 0) {
        *out_size = sizeof(float); *out_align = sizeof(float);
    } else if (strcmp(type, "double") == 0) {
        *out_size = sizeof(double); *out_align = sizeof(double);
    } else if (strcmp(type, "ptr") == 0) {
        *out_size = sizeof(void*); *out_align = sizeof(void*);
    } else {
        return -1;
    }
    return 0;
}

// Helper: align offset to alignment boundary
static size_t align_to(size_t offset, size_t alignment) {
    return (offset + alignment - 1) & ~(alignment - 1);
}

// struct_def(fields) -> dict
// fields: array of [name, type] pairs
// Returns a dict with field metadata:
//   "__size__" -> total struct size (number)
//   "__align__" -> struct alignment (number)
//   "field_name" -> [offset, size, type] (tuple)
static Value struct_def_native(int argCount, Value* args) {
    if (argCount != 1 || !IS_ARRAY(args[0])) {
        fprintf(stderr, "struct_def() expects (array of [name, type] pairs).\n");
        return val_nil();
    }

    ArrayValue* fields = AS_ARRAY(args[0]);
    Value result = val_dict();
    size_t offset = 0;
    size_t max_align = 1;

    for (int i = 0; i < fields->count; i++) {
        Value field = fields->elements[i];
        if (!IS_ARRAY(field)) {
            fprintf(stderr, "struct_def(): each field must be [name, type].\n");
            return val_nil();
        }
        ArrayValue* pair = AS_ARRAY(field);
        if (pair->count != 2 || !IS_STRING(pair->elements[0]) || !IS_STRING(pair->elements[1])) {
            fprintf(stderr, "struct_def(): each field must be [name_string, type_string].\n");
            return val_nil();
        }

        const char* name = AS_STRING(pair->elements[0]);
        const char* type = AS_STRING(pair->elements[1]);

        size_t fsize, falign;
        if (struct_type_info(type, &fsize, &falign) != 0) {
            fprintf(stderr, "struct_def(): unknown type '%s'.\n", type);
            return val_nil();
        }

        // Align offset
        offset = align_to(offset, falign);
        if (falign > max_align) max_align = falign;

        // Store field info as tuple: (offset, size, type_string)
        Value tuple_elems[3];
        tuple_elems[0] = val_int((int64_t)offset);
        tuple_elems[1] = val_int((int64_t)fsize);
        tuple_elems[2] = val_string(type);
        dict_set(&result, name, val_tuple(tuple_elems, 3));

        offset += fsize;
    }

    // Align total size to struct alignment
    offset = align_to(offset, max_align);

    dict_set(&result, "__size__", val_int((int64_t)offset));
    dict_set(&result, "__align__", val_number((double)max_align));

    return result;
}

// struct_new(def) -> pointer
// Allocates zeroed memory for the struct
static Value struct_new_native(int argCount, Value* args) {
    if (argCount != 1 || !IS_DICT(args[0])) {
        fprintf(stderr, "struct_new() expects (struct_def dict).\n");
        return val_nil();
    }

    Value size_val = dict_get(&args[0], "__size__");
    if (!IS_NUMERIC(size_val)) {
        fprintf(stderr, "struct_new(): invalid struct definition (missing __size__).\n");
        return val_nil();
    }

    size_t size = (size_t)NUMERIC_AS_DOUBLE(size_val);
    void* ptr = calloc(1, size);
    if (!ptr) {
        fprintf(stderr, "struct_new(): allocation failed.\n");
        return val_nil();
    }
    return val_pointer(ptr, size, 1);
}

// struct_get(ptr, def, field_name) -> value
static Value struct_get_native(int argCount, Value* args) {
    if (argCount != 3 || !IS_POINTER(args[0]) || !IS_DICT(args[1]) || !IS_STRING(args[2])) {
        fprintf(stderr, "struct_get() expects (pointer, struct_def, field_name).\n");
        return val_nil();
    }

    PointerValue* p = AS_POINTER(args[0]);
    if (!p->ptr) {
        fprintf(stderr, "struct_get(): null pointer.\n");
        return val_nil();
    }

    Value field_info = dict_get(&args[1], AS_STRING(args[2]));
    if (!IS_TUPLE(field_info) || field_info.as.tuple->count != 3) {
        fprintf(stderr, "struct_get(): unknown field '%s'.\n", AS_STRING(args[2]));
        return val_nil();
    }

    size_t offset = (size_t)NUMERIC_AS_DOUBLE(field_info.as.tuple->elements[0]);
    const char* type = AS_STRING(field_info.as.tuple->elements[2]);
    unsigned char* base = (unsigned char*)p->ptr + offset;

    if (strcmp(type, "char") == 0 || strcmp(type, "byte") == 0) {
        return val_int((int64_t)*base);
    } else if (strcmp(type, "short") == 0) {
        short v; memcpy(&v, base, sizeof(short));
        return val_int((int64_t)v);
    } else if (strcmp(type, "int") == 0) {
        int v; memcpy(&v, base, sizeof(int));
        return val_int((int64_t)v);
    } else if (strcmp(type, "long") == 0) {
        long v; memcpy(&v, base, sizeof(long));
        return val_int((int64_t)v);
    } else if (strcmp(type, "float") == 0) {
        float v; memcpy(&v, base, sizeof(float));
        return val_number((double)v);
    } else if (strcmp(type, "double") == 0) {
        double v; memcpy(&v, base, sizeof(double));
        return val_number(v);
    } else if (strcmp(type, "ptr") == 0) {
        void* v; memcpy(&v, base, sizeof(void*));
        return val_pointer(v, 0, 0); // Non-owned external pointer
    }
    return val_nil();
}

// struct_set(ptr, def, field_name, value) -> nil
static Value struct_set_native(int argCount, Value* args) {
    if (argCount != 4 || !IS_POINTER(args[0]) || !IS_DICT(args[1]) || !IS_STRING(args[2])) {
        fprintf(stderr, "struct_set() expects (pointer, struct_def, field_name, value).\n");
        return val_nil();
    }

    PointerValue* p = AS_POINTER(args[0]);
    if (!p->ptr) {
        fprintf(stderr, "struct_set(): null pointer.\n");
        return val_nil();
    }

    Value field_info = dict_get(&args[1], AS_STRING(args[2]));
    if (!IS_TUPLE(field_info) || field_info.as.tuple->count != 3) {
        fprintf(stderr, "struct_set(): unknown field '%s'.\n", AS_STRING(args[2]));
        return val_nil();
    }

    size_t offset = (size_t)NUMERIC_AS_DOUBLE(field_info.as.tuple->elements[0]);
    const char* type = AS_STRING(field_info.as.tuple->elements[2]);
    unsigned char* base = (unsigned char*)p->ptr + offset;

    if (!IS_NUMERIC(args[3]) && strcmp(type, "ptr") != 0) {
        fprintf(stderr, "struct_set(): value must be a number for type '%s'.\n", type);
        return val_nil();
    }

    if (strcmp(type, "char") == 0 || strcmp(type, "byte") == 0) {
        *base = (unsigned char)NUMERIC_AS_DOUBLE(args[3]);
    } else if (strcmp(type, "short") == 0) {
        short v = (short)NUMERIC_AS_DOUBLE(args[3]);
        memcpy(base, &v, sizeof(short));
    } else if (strcmp(type, "int") == 0) {
        int v = (int)NUMERIC_AS_INT(args[3]);
        memcpy(base, &v, sizeof(int));
    } else if (strcmp(type, "long") == 0) {
        long v = (long)NUMERIC_AS_INT(args[3]);
        memcpy(base, &v, sizeof(long));
    } else if (strcmp(type, "float") == 0) {
        float v = (float)NUMERIC_AS_DOUBLE(args[3]);
        memcpy(base, &v, sizeof(float));
    } else if (strcmp(type, "double") == 0) {
        double v = NUMERIC_AS_DOUBLE(args[3]);
        memcpy(base, &v, sizeof(double));
    } else if (strcmp(type, "ptr") == 0) {
        if (!IS_POINTER(args[3])) {
            fprintf(stderr, "struct_set(): value must be a pointer for type 'ptr'.\n");
            return val_nil();
        }
        void* v = AS_POINTER(args[3])->ptr;
        memcpy(base, &v, sizeof(void*));
    }
    return val_nil();
}

// struct_size(def) -> number
static Value struct_size_native(int argCount, Value* args) {
    if (argCount != 1 || !IS_DICT(args[0])) {
        fprintf(stderr, "struct_size() expects (struct_def dict).\n");
        return val_nil();
    }
    Value size_val = dict_get(&args[0], "__size__");
    if (!IS_NUMERIC(size_val)) return val_nil();
    return size_val;
}

// ========== Phase 9: Inline Assembly ==========

// Helper: process \n and \t escape sequences in assembly code strings
static char* asm_process_escapes(const char* raw) {
    size_t len = strlen(raw);
    char* out = SAGE_ALLOC(len + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (raw[i] == '\\' && i + 1 < len) {
            if (raw[i + 1] == 'n') { out[j++] = '\n'; i++; continue; }
            if (raw[i + 1] == 't') { out[j++] = '\t'; i++; continue; }
        }
        out[j++] = raw[i];
    }
    out[j] = '\0';
    return out;
}

// Helper: detect host architecture and return assembler flags
static const char* asm_detect_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__riscv) && __riscv_xlen == 64
    return "rv64";
#else
    return "unknown";
#endif
}

// Validate a path contains no shell metacharacters (prevents injection via system())
static int is_safe_path(const char* path) {
    for (const char* p = path; *p; p++) {
        // Allow only alphanumeric, /, ., -, _, ~
        if (!isalnum((unsigned char)*p) && *p != '/' && *p != '.' &&
            *p != '-' && *p != '_' && *p != '~') {
            return 0;
        }
    }
    return 1;
}

// Helper: get assembler command for architecture
// For native arch, uses system `as`. For cross, tries arch-specific cross-assembler.
static int asm_get_commands(const char* arch, const char* asm_path,
                            const char* obj_path, const char* so_path,
                            char* as_cmd, size_t as_sz,
                            char* ld_cmd, size_t ld_sz) {
    // Validate all paths to prevent shell injection
    if (!is_safe_path(asm_path) || !is_safe_path(obj_path) || !is_safe_path(so_path)) {
        fprintf(stderr, "asm: path contains unsafe characters.\n");
        return -1;
    }
    const char* host_arch = asm_detect_arch();
    int cross = (strcmp(arch, host_arch) != 0);

    if (strcmp(arch, "x86_64") == 0) {
        if (cross) {
            snprintf(as_cmd, as_sz, "x86_64-linux-gnu-as -o %s %s 2>/dev/null", obj_path, asm_path);
            snprintf(ld_cmd, ld_sz, "x86_64-linux-gnu-gcc -shared -o %s %s 2>/dev/null", so_path, obj_path);
        } else {
            snprintf(as_cmd, as_sz, "as --64 -o %s %s 2>/dev/null", obj_path, asm_path);
            snprintf(ld_cmd, ld_sz, "gcc -shared -o %s %s 2>/dev/null", so_path, obj_path);
        }
    } else if (strcmp(arch, "aarch64") == 0) {
        if (cross) {
            snprintf(as_cmd, as_sz, "aarch64-linux-gnu-as -o %s %s 2>/dev/null", obj_path, asm_path);
            snprintf(ld_cmd, ld_sz, "aarch64-linux-gnu-gcc -shared -o %s %s 2>/dev/null", so_path, obj_path);
        } else {
            snprintf(as_cmd, as_sz, "as -o %s %s 2>/dev/null", obj_path, asm_path);
            snprintf(ld_cmd, ld_sz, "gcc -shared -o %s %s 2>/dev/null", so_path, obj_path);
        }
    } else if (strcmp(arch, "rv64") == 0) {
        if (cross) {
            snprintf(as_cmd, as_sz, "riscv64-linux-gnu-as -o %s %s 2>/dev/null", obj_path, asm_path);
            snprintf(ld_cmd, ld_sz, "riscv64-linux-gnu-gcc -shared -o %s %s 2>/dev/null", so_path, obj_path);
        } else {
            snprintf(as_cmd, as_sz, "as -o %s %s 2>/dev/null", obj_path, asm_path);
            snprintf(ld_cmd, ld_sz, "gcc -shared -o %s %s 2>/dev/null", so_path, obj_path);
        }
    } else {
        return -1; // Unknown arch
    }
    return cross ? 1 : 0;
}

// Helper: write the assembly source file with proper function wrapper
static int asm_write_source(const char* path, const char* code, const char* arch) {
    FILE* f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, ".text\n");
    fprintf(f, ".globl sage_asm_fn\n");

    // Architecture-specific directives
    if (strcmp(arch, "x86_64") == 0 || strcmp(arch, "aarch64") == 0 || strcmp(arch, "rv64") == 0) {
        fprintf(f, ".type sage_asm_fn, @function\n");
    }

    fprintf(f, "sage_asm_fn:\n");
    fprintf(f, "%s\n", code);

    // Architecture-specific return instruction
    if (strcmp(arch, "x86_64") == 0) {
        fprintf(f, "    ret\n");
    } else if (strcmp(arch, "aarch64") == 0) {
        fprintf(f, "    ret\n");
    } else if (strcmp(arch, "rv64") == 0) {
        fprintf(f, "    ret\n"); // RISC-V pseudo-instruction (jalr x0, x1, 0)
    }

    fprintf(f, ".size sage_asm_fn, .-sage_asm_fn\n");
    fclose(f);
    return 0;
}

#ifndef SAGE_NO_FFI
// asm_exec(code, ret_type, ...args) -> value
#endif
// Compiles assembly to a temp shared library, calls it, returns result.
// code: string of assembly instructions (\n for newlines)
// ret_type: "int", "double", or "void"
// args: up to 4 numeric arguments
// Uses host architecture by default. For cross-compilation, see asm_compile().
static Value asm_exec_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) {
        fprintf(stderr, "asm_exec() expects (code_string, ret_type, ...args).\n");
        return val_nil();
    }

    char* code = asm_process_escapes(AS_STRING(args[0]));
    const char* ret_type = AS_STRING(args[1]);
    int num_args = argCount - 2;
    const char* arch = asm_detect_arch();
    Value result = val_nil();

    if (num_args > 4) {
        fprintf(stderr, "asm_exec(): max 4 arguments supported.\n");
        free(code);
        return val_nil();
    }

    for (int i = 0; i < num_args; i++) {
        if (!IS_NUMERIC(args[i + 2])) {
            fprintf(stderr, "asm_exec(): argument %d must be a number.\n", i);
            free(code);
            return val_nil();
        }
    }

    // Generate secure temp file names using mkstemp
    char asm_path[] = "/tmp/sage_asm_XXXXXX.s";
    char obj_path[] = "/tmp/sage_asm_XXXXXX.o";
    char so_path[]  = "/tmp/sage_asm_XXXXXX.so";
    int asm_fd = mkstemps(asm_path, 2);
    int obj_fd = mkstemps(obj_path, 2);
    int so_fd  = mkstemps(so_path, 3);
    if (asm_fd >= 0) close(asm_fd);
    if (obj_fd >= 0) close(obj_fd);
    if (so_fd >= 0)  close(so_fd);

    // Write assembly source
    if (asm_write_source(asm_path, code, arch) != 0) {
        fprintf(stderr, "asm_exec(): failed to create temp file.\n");
        free(code);
        return val_nil();
    }
    free(code);

    // Get assembler/linker commands
    char as_cmd[512], ld_cmd[512];
    if (asm_get_commands(arch, asm_path, obj_path, so_path,
                         as_cmd, sizeof(as_cmd), ld_cmd, sizeof(ld_cmd)) < 0) {
        fprintf(stderr, "asm_exec(): unsupported architecture '%s'.\n", arch);
        unlink(asm_path);
        return val_nil();
    }

    // Assemble
    if (system(as_cmd) != 0) {
        fprintf(stderr, "asm_exec(): assembly failed for %s.\n", arch);
        unlink(asm_path);
        return val_nil();
    }

    // Link as shared library
    if (system(ld_cmd) != 0) {
        fprintf(stderr, "asm_exec(): linking failed.\n");
        unlink(asm_path);
        unlink(obj_path);
        return val_nil();
    }

    // Load and call
    void* handle = dlopen(so_path, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "asm_exec(): dlopen failed: %s\n", dlerror());
        goto cleanup_files;
    }

    void* sym = dlsym(handle, "sage_asm_fn");
    if (!sym) {
        fprintf(stderr, "asm_exec(): symbol lookup failed.\n");
        dlclose(handle);
        goto cleanup_files;
    }

    // Call function via appropriate signature
    int use_double = (strcmp(ret_type, "double") == 0);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    if (use_double) {
        double dargs[4] = {0};
        for (int i = 0; i < num_args; i++) dargs[i] = NUMERIC_AS_DOUBLE(args[i + 2]);

        double (*fn0)(void) = (double (*)(void))sym;
        double (*fn1)(double) = (double (*)(double))sym;
        double (*fn2)(double, double) = (double (*)(double, double))sym;
        double (*fn3)(double, double, double) = (double (*)(double, double, double))sym;
        double (*fn4)(double, double, double, double) = (double (*)(double, double, double, double))sym;

        double dres;
        switch (num_args) {
            case 0: dres = fn0(); break;
            case 1: dres = fn1(dargs[0]); break;
            case 2: dres = fn2(dargs[0], dargs[1]); break;
            case 3: dres = fn3(dargs[0], dargs[1], dargs[2]); break;
            case 4: dres = fn4(dargs[0], dargs[1], dargs[2], dargs[3]); break;
            default: dres = 0; break;
        }
        result = val_number(dres);
    } else if (strcmp(ret_type, "void") == 0) {
        long long iargs[4] = {0};
        for (int i = 0; i < num_args; i++) iargs[i] = (long long)NUMERIC_AS_INT(args[i + 2]);

        void (*fn0)(void) = (void (*)(void))sym;
        void (*fn1)(long long) = (void (*)(long long))sym;
        void (*fn2)(long long, long long) = (void (*)(long long, long long))sym;

        switch (num_args) {
            case 0: fn0(); break;
            case 1: fn1(iargs[0]); break;
            case 2: fn2(iargs[0], iargs[1]); break;
            default: fn0(); break;
        }
    } else {
        // "int" / "long" - integer return
        long long iargs[4] = {0};
        for (int i = 0; i < num_args; i++) iargs[i] = (long long)NUMERIC_AS_INT(args[i + 2]);

        long long (*fn0)(void) = (long long (*)(void))sym;
        long long (*fn1)(long long) = (long long (*)(long long))sym;
        long long (*fn2)(long long, long long) = (long long (*)(long long, long long))sym;
        long long (*fn3)(long long, long long, long long) = (long long (*)(long long, long long, long long))sym;
        long long (*fn4)(long long, long long, long long, long long) = (long long (*)(long long, long long, long long, long long))sym;

        long long ires;
        switch (num_args) {
            case 0: ires = fn0(); break;
            case 1: ires = fn1(iargs[0]); break;
            case 2: ires = fn2(iargs[0], iargs[1]); break;
            case 3: ires = fn3(iargs[0], iargs[1], iargs[2]); break;
            case 4: ires = fn4(iargs[0], iargs[1], iargs[2], iargs[3]); break;
            default: ires = 0; break;
        }
        result = val_number((double)ires);
    }
#pragma GCC diagnostic pop

    dlclose(handle);

cleanup_files:
    unlink(asm_path);
    unlink(obj_path);
    unlink(so_path);

    return result;
}

// asm_compile(code, arch, output_path) -> bool
// Cross-compile assembly for a target architecture without executing.
// arch: "x86_64", "aarch64", or "rv64"
// output_path: path to write the .o object file
static Value asm_compile_native(int argCount, Value* args) {
    if (argCount != 3 || !IS_STRING(args[0]) || !IS_STRING(args[1]) || !IS_STRING(args[2])) {
        fprintf(stderr, "asm_compile() expects (code_string, arch, output_path).\n");
        return val_bool(0);
    }

    char* code = asm_process_escapes(AS_STRING(args[0]));
    const char* arch = AS_STRING(args[1]);
    const char* output_path = AS_STRING(args[2]);

    // Write assembly source with secure temp file
    char asm_path[] = "/tmp/sage_xasm_XXXXXX.s";
    int asm_fd = mkstemps(asm_path, 2);
    if (asm_fd >= 0) close(asm_fd);

    if (asm_write_source(asm_path, code, arch) != 0) {
        fprintf(stderr, "asm_compile(): failed to create temp file.\n");
        free(code);
        return val_bool(0);
    }
    free(code);

    // Get assembler command (we only need the assembler, not linker)
    char as_cmd[512], ld_cmd[512];
    if (asm_get_commands(arch, asm_path, output_path, "/dev/null",
                         as_cmd, sizeof(as_cmd), ld_cmd, sizeof(ld_cmd)) < 0) {
        fprintf(stderr, "asm_compile(): unsupported architecture '%s'.\n", arch);
        unlink(asm_path);
        return val_bool(0);
    }

    // Assemble to object file (output_path is already in as_cmd as obj_path)
    int ok = (system(as_cmd) == 0);
    unlink(asm_path);

    return val_bool(ok);
}

// asm_arch() -> string
// Returns the host architecture name
static Value asm_arch_native(int argCount, Value* args) {
    (void)argCount; (void)args;
    return val_string(asm_detect_arch());
}

// Phase 1.9: doc() builtin — retrieve documentation from a function
static Value doc_native(int argCount, Value* args) {
    if (argCount != 1) return val_nil();
    if (args[0].type == VAL_FUNCTION && args[0].as.function->proc) {
        ProcStmt* proc = (ProcStmt*)args[0].as.function->proc;
        if (proc->doc) return val_string(proc->doc);
    }
    return val_nil();
}

// Phase 1.9: hash() builtin — FNV-1a hash for any value
static unsigned int fnv1a_str(const char* str) {
    unsigned int h = 2166136261u;
    while (*str) { h ^= (unsigned char)*str++; h *= 16777619u; }
    return h;
}

static Value hash_native(int argCount, Value* args) {
    if (argCount != 1) return val_number(0);
    Value v = args[0];
    switch (v.type) {
        case VAL_NUMBER: {
            double d = NUMERIC_AS_DOUBLE(v);
            unsigned int h = 2166136261u;
            unsigned char* p = (unsigned char*)&d;
            for (int i = 0; i < (int)sizeof(double); i++) { h ^= p[i]; h *= 16777619u; }
            return val_number(h);
        }
        case VAL_STRING: return val_number(fnv1a_str(AS_STRING(v)));
        case VAL_BOOL: return val_number(AS_BOOL(v) ? 1231 : 1237);
        case VAL_NIL: return val_number(0);
        case VAL_BYTES: {
            unsigned int h = 2166136261u;
            for (int i = 0; i < v.as.bytes->length; i++) { h ^= v.as.bytes->data[i]; h *= 16777619u; }
            return val_number(h);
        }
        default: {
            // Use heap pointer as identity hash for heap-allocated types
            void* ptr = NULL;
            switch (v.type) {
                case VAL_ARRAY:     ptr = v.as.array; break;
                case VAL_DICT:      ptr = v.as.dict; break;
                case VAL_TUPLE:     ptr = v.as.tuple; break;
                case VAL_FUNCTION:  ptr = v.as.function; break;
                case VAL_CLASS:     ptr = v.as.class_val; break;
                case VAL_INSTANCE:  ptr = v.as.instance; break;
                case VAL_GENERATOR: ptr = v.as.generator; break;
                case VAL_EXCEPTION: ptr = v.as.exception; break;
                case VAL_MODULE:    ptr = v.as.module; break;
                case VAL_CLIB:      ptr = v.as.clib; break;
                case VAL_POINTER:   ptr = v.as.pointer; break;
                case VAL_THREAD:    ptr = v.as.thread; break;
                case VAL_MUTEX:     ptr = v.as.mutex; break;
                default:            ptr = NULL; break;
            }
            return val_number(ptr ? (double)(uintptr_t)ptr : 0);
        }
    }
}

// Phase 1.9: Path utility builtins
static Value path_join_native(int argCount, Value* args) {
    if (argCount < 2) return val_nil();
    int total_len = 0;
    for (int i = 0; i < argCount; i++) {
        if (!IS_STRING(args[i])) return val_nil();
        total_len += (int)strlen(AS_STRING(args[i])) + 1;
    }
    char* result = SAGE_ALLOC(total_len + 1);
    result[0] = '\0';
    for (int i = 0; i < argCount; i++) {
        if (i > 0 && result[0] != '\0') {
            int len = (int)strlen(result);
            if (len > 0 && result[len - 1] != '/') strcat(result, "/");
        }
        strcat(result, AS_STRING(args[i]));
    }
    return val_string_take(result);
}

static Value path_dirname_native(int argCount, Value* args) {
    if (argCount != 1 || !IS_STRING(args[0])) return val_string(".");
    const char* path = AS_STRING(args[0]);
    const char* last_slash = strrchr(path, '/');
    if (!last_slash) return val_string(".");
    int len = (int)(last_slash - path);
    if (len == 0) return val_string("/");
    char* dir = SAGE_ALLOC(len + 1);
    memcpy(dir, path, len);
    dir[len] = '\0';
    return val_string_take(dir);
}

static Value path_basename_native(int argCount, Value* args) {
    if (argCount != 1 || !IS_STRING(args[0])) return val_string("");
    const char* path = AS_STRING(args[0]);
    const char* last_slash = strrchr(path, '/');
    return val_string(last_slash ? last_slash + 1 : path);
}

static Value path_ext_native(int argCount, Value* args) {
    if (argCount != 1 || !IS_STRING(args[0])) return val_string("");
    const char* path = AS_STRING(args[0]);
    const char* base = strrchr(path, '/');
    const char* dot = strrchr(base ? base : path, '.');
    if (!dot || dot == path) return val_string("");
    return val_string(dot);
}

static Value path_exists_native(int argCount, Value* args) {
    if (argCount != 1 || !IS_STRING(args[0])) return val_bool(0);
    return val_bool(access(AS_STRING(args[0]), F_OK) == 0);
}

static Value path_is_dir_native(int argCount, Value* args) {
    if (argCount != 1 || !IS_STRING(args[0])) return val_bool(0);
    struct stat st;
    if (stat(AS_STRING(args[0]), &st) != 0) return val_bool(0);
    return val_bool(S_ISDIR(st.st_mode));
}

static Value path_is_file_native(int argCount, Value* args) {
    if (argCount != 1 || !IS_STRING(args[0])) return val_bool(0);
    struct stat st;
    if (stat(AS_STRING(args[0]), &st) != 0) return val_bool(0);
    return val_bool(S_ISREG(st.st_mode));
}

void init_stdlib(Env* env) {
    // Core functions
    env_define_const(env, "clock", 5, val_native(clock_native));
    env_define_const(env, "input", 5, val_native(input_native));
    env_define_const(env, "tonumber", 8, val_native(tonumber_native));
    env_define_const(env, "int", 3, val_native(int_native));
    env_define_const(env, "float",   5, val_native(float_native));
    env_define_const(env, "typeof",  6, val_native(typeof_native));
    env_define_const(env, "precision", 9, val_native(precision_native));
    env_define_const(env, "str",     3, val_native(str_native));
    env_define_const(env, "println", 7, val_native(println_native));
    env_define_const(env, "eprint",  6, val_native(eprint_native));
    env_define_const(env, "len", 3, val_native(len_native));

    // VM / Gas functions
    env_define_const(env, "vm_gas_limit_set", 16, val_native(vm_set_gas_limit_native));
    env_define_const(env, "vm_gas_used_get", 15, val_native(vm_get_gas_used_native));
    env_define_const(env, "vm_gas_limit_get", 16, val_native(vm_get_gas_limit_native));
    
    // Array functions
    env_define_const(env, "push", 4, val_native(push_native));
    env_define_const(env, "append", 6, val_native(push_native));
    env_define_const(env, "build_quad_verts", 16, val_native(build_quad_verts_native));
    env_define_const(env, "array_extend", 12, val_native(array_extend_native));
    env_define_const(env, "build_line_quads", 16, val_native(build_line_quads_native));
    env_define_const(env, "pop", 3, val_native(pop_native));
    env_define_const(env, "range", 5, val_native(range_native));
    env_define_const(env, "slice", 5, val_native(slice_native));
    
    // String functions
    env_define_const(env, "split", 5, val_native(split_native));
    env_define_const(env, "join", 4, val_native(join_native));
    env_define_const(env, "replace", 7, val_native(replace_native));
    env_define_const(env, "upper", 5, val_native(upper_native));
    env_define_const(env, "lower", 5, val_native(lower_native));
    env_define_const(env, "strip", 5, val_native(strip_native));
    env_define_const(env, "type", 4, val_native(type_native));
    env_define_const(env, "chr", 3, val_native(chr_native));
    env_define_const(env, "ord", 3, val_native(ord_native));
    env_define_const(env, "startswith", 10, val_native(startswith_native));
    env_define_const(env, "endswith", 8, val_native(endswith_native));
    env_define_const(env, "contains", 8, val_native(contains_native));
    env_define_const(env, "indexof", 7, val_native(indexof_native));

    // Dictionary functions
    env_define_const(env, "dict_keys", 9, val_native(dict_keys_native));
    env_define_const(env, "dict_values", 11, val_native(dict_values_native));
    env_define_const(env, "dict_has", 8, val_native(dict_has_native));
    env_define_const(env, "dict_delete", 11, val_native(dict_delete_native));
    
    // GC functions
    env_define_const(env, "gc_collect", 10, val_native(gc_collect_native));
    env_define_const(env, "gc_stats", 8, val_native(gc_stats_native));
    env_define_const(env, "gc_collections", 14, val_native(gc_collections_native));
    env_define_const(env, "gc_enable", 9, val_native(gc_enable_native));
    env_define_const(env, "gc_disable", 10, val_native(gc_disable_native));
    env_define_const(env, "gc_mode", 7, val_native(gc_mode_native));
    env_define_const(env, "gc_set_arc", 10, val_native(gc_set_arc_native));
    env_define_const(env, "gc_set_orc", 10, val_native(gc_set_orc_native));

    // ── Phase 2: Option / Result built-in constructors ──────────────────
    env_define_const(env, "Some", 4, val_native(sage_Some_native));
    env_define_const(env, "Ok",   2, val_native(sage_Ok_native));
    env_define_const(env, "Err",  3, val_native(sage_Err_native));
    // None is just nil — but expose it as a named constant
    env_define_const(env, "None", 4, val_nil());

    // PHASE 7: Generator function
    env_define_const(env, "next", 4, val_native(native_next));

#ifndef SAGE_NO_FFI
    // Phase 9: FFI functions
    env_define_const(env, "ffi_open", 8, val_native(ffi_open_native));
    env_define_const(env, "ffi_close", 9, val_native(ffi_close_native));
    env_define_const(env, "ffi_call", 8, val_native(ffi_call_native));
    env_define_const(env, "ffi_sym", 7, val_native(ffi_sym_native));
#endif

    // Phase 1.8: Bytes operations
    env_define_const(env, "bytes", 5, val_native(bytes_new_native));
    env_define_const(env, "bytes_len", 9, val_native(bytes_len_native));
    env_define_const(env, "bytes_get", 9, val_native(bytes_get_native));
    env_define_const(env, "bytes_set", 9, val_native(bytes_set_native));
    env_define_const(env, "bytes_to_string", 15, val_native(bytes_to_string_native));
    env_define_const(env, "bytes_slice", 11, val_native(bytes_slice_native));
    env_define_const(env, "bytes_push", 10, val_native(bytes_push_native));

    // Phase 1.8: sizeof and pointer arithmetic
    env_define_const(env, "sizeof", 6, val_native(sizeof_native));
    env_define_const(env, "ptr_add", 7, val_native(ptr_add_native));
    env_define_const(env, "ptr_to_int", 10, val_native(ptr_to_int_native));

    // Phase 1.9: Hash, doc, and path utilities
    env_define_const(env, "hash", 4, val_native(hash_native));
    env_define_const(env, "doc", 3, val_native(doc_native));
    env_define_const(env, "path_join", 9, val_native(path_join_native));
    env_define_const(env, "path_dirname", 12, val_native(path_dirname_native));
    env_define_const(env, "path_basename", 13, val_native(path_basename_native));
    env_define_const(env, "path_ext", 8, val_native(path_ext_native));
    env_define_const(env, "path_exists", 11, val_native(path_exists_native));
    env_define_const(env, "path_is_dir", 11, val_native(path_is_dir_native));
    env_define_const(env, "path_is_file", 12, val_native(path_is_file_native));

    // Phase 9: Memory operations
    env_define_const(env, "mem_alloc", 9, val_native(mem_alloc_native));
    env_define_const(env, "mem_free", 8, val_native(mem_free_native));
    env_define_const(env, "mem_read", 8, val_native(mem_read_native));
    env_define_const(env, "mem_write", 9, val_native(mem_write_native));
    env_define_const(env, "mem_size", 8, val_native(mem_size_native));
    env_define_const(env, "addressof", 9, val_native(addressof_native));

    // Phase 9: C struct interop
    env_define_const(env, "struct_def", 10, val_native(struct_def_native));
    env_define_const(env, "struct_new", 10, val_native(struct_new_native));
    env_define_const(env, "struct_get", 10, val_native(struct_get_native));
    env_define_const(env, "struct_set", 10, val_native(struct_set_native));
    env_define_const(env, "struct_size", 11, val_native(struct_size_native));

    // Phase 9: Inline assembly
#ifndef SAGE_NO_FFI
    env_define_const(env, "asm_exec", 8, val_native(asm_exec_native));
#endif
    env_define_const(env, "asm_compile", 11, val_native(asm_compile_native));
    env_define_const(env, "asm_arch", 8, val_native(asm_arch_native));

    // SMP / CPU topology
    env_define_const(env, "cpu_count", 9, val_native(cpu_count_native));
    env_define_const(env, "cpu_physical_cores", 18, val_native(cpu_physical_cores_native));
    env_define_const(env, "cpu_has_hyperthreading", 22, val_native(cpu_has_hyperthreading_native));
    env_define_const(env, "thread_set_affinity", 19, val_native(thread_set_affinity_native));
    env_define_const(env, "thread_get_core", 15, val_native(thread_get_core_native));

    // True atomic operations (C-level __atomic builtins)
    env_define_const(env, "atomic_new", 10, val_native(atomic_new_native));
    env_define_const(env, "atomic_load", 11, val_native(atomic_load_native));
    env_define_const(env, "atomic_store", 12, val_native(atomic_store_native));
    env_define_const(env, "atomic_add", 10, val_native(atomic_add_native));
    env_define_const(env, "atomic_cas", 10, val_native(atomic_cas_native));
    env_define_const(env, "atomic_exchange", 15, val_native(atomic_exchange_native));

    // Semaphores (C-level POSIX semaphores)
    env_define_const(env, "sem_new", 7, val_native(sem_new_native));
    env_define_const(env, "sem_wait", 8, val_native(sem_wait_native));
    env_define_const(env, "sem_post", 8, val_native(sem_post_native));
    env_define_const(env, "sem_trywait", 11, val_native(sem_trywait_native));
}

// --- Helper: Truthiness ---
static int is_truthy(Value v) {
    if (IS_NIL(v)) return 0;
    if (IS_BOOL(v)) return AS_BOOL(v);
    if (IS_INT(v)) return AS_INT(v) != 0;
    if (IS_NUMBER(v)) return NUMERIC_AS_DOUBLE(v) != 0.0;
    if (IS_STRING(v)) return AS_STRING(v)[0] != '\0';
    return 1;
}

// PHASE 7: Helper to detect if a statement block contains yield
static int contains_yield(Stmt* body) {
    Stmt* current = body;
    while (current != NULL) {
        if (current->type == STMT_YIELD) return 1;
        if (current->type == STMT_BLOCK && contains_yield(current->as.block.statements)) return 1;
        if (current->type == STMT_IF) {
            if (contains_yield(current->as.if_stmt.then_branch)) return 1;
            if (current->as.if_stmt.else_branch && contains_yield(current->as.if_stmt.else_branch)) return 1;
        }
        if (current->type == STMT_WHILE && contains_yield(current->as.while_stmt.body)) return 1;
        if (current->type == STMT_FOR && contains_yield(current->as.for_stmt.body)) return 1;
        current = current->next;
    }
    return 0;
}

// --- Forward Declaration ---
static ExecResult eval_expr(Expr* expr, Env* env);
// eval_expr_impl merged into eval_expr — recursion depth checked at
// function call boundaries only (interpret()), not per-expression.
static ExecResult interpret_inner(Stmt* stmt, Env* env);

// --- Evaluator ---

static ExecResult eval_binary(BinaryExpr* b, Env* env) {
    ExecResult left_result = eval_expr(b->left, env);
    if (left_result.is_throwing) return left_result;
    Value left = left_result.value;

    if (b->op.type == TOKEN_NOT) {
        return EVAL_RESULT(val_bool(!is_truthy(left)));
    }

    // Phase 9: Bitwise NOT (~x) — P1: always produces VAL_INT
    if (b->op.type == TOKEN_TILDE) {
        if (!IS_NUMERIC(left)) {
            firefly_report(FIREFLY_ERROR, firefly_loc_from_token(&b->op), "bitwise NOT (~) operand must be a number, got %s", sage_typeof_str(left));
            return EVAL_RESULT(val_nil());
        }
        return EVAL_RESULT(val_int(~NUMERIC_AS_INT(left)));
    }

    AST_GC_PUSH(left);

    if (b->op.type == TOKEN_OR) {
        if (is_truthy(left)) {
            AST_GC_POP();
            return EVAL_RESULT(val_bool(1));
        }
        ExecResult right_result = eval_expr(b->right, env);
        AST_GC_POP();
        if (right_result.is_throwing) return right_result;
        return EVAL_RESULT(val_bool(is_truthy(right_result.value)));
    }

    if (b->op.type == TOKEN_AND) {
        if (!is_truthy(left)) {
            AST_GC_POP();
            return EVAL_RESULT(val_bool(0));
        }
        ExecResult right_result = eval_expr(b->right, env);
        AST_GC_POP();
        if (right_result.is_throwing) return right_result;
        return EVAL_RESULT(val_bool(is_truthy(right_result.value)));
    }

    ExecResult right_result = eval_expr(b->right, env);
    if (right_result.is_throwing) { AST_GC_POP(); return right_result; }
    Value right = right_result.value;

    if (b->op.type == TOKEN_EQ || b->op.type == TOKEN_NEQ) {
        int equal;
        // __eq__ hook: check if left operand has custom equality method
        if (left.type == VAL_INSTANCE && left.as.instance->class_def) {
            Method* eq_method = class_find_method(left.as.instance->class_def, "__eq__", 6);
            if (eq_method) {
                AST_GC_PUSH(right);
                Stmt* method_node = (Stmt*)eq_method->method_stmt;
                ProcStmt* proc = (method_node->type == STMT_ASYNC_PROC) ? &method_node->as.async_proc : &method_node->as.proc;
                Env* defining = left.as.instance->class_def->defining_env;
                Env* method_env = env_create(defining ? defining : env);

                env_define(method_env, "self", 4, left);
                int p_start = (proc->param_count > 0 &&
                              strncmp(proc->params[0].start, "self", 4) == 0) ? 1 : 0;
                if (p_start < proc->param_count) {
                    env_define(method_env, proc->params[p_start].start,
                               proc->params[p_start].length, right);
                }
                ExecResult eq_res = interpret(proc->body, method_env);
                equal = !eq_res.is_throwing && is_truthy(eq_res.value);
                AST_GC_POP(); // pop right
            } else {
                equal = values_equal(left, right);
            }
        } else {
            equal = values_equal(left, right);
        }
        AST_GC_POP(); // pop left
        if (b->op.type == TOKEN_EQ) return EVAL_RESULT(val_bool(equal));
        if (b->op.type == TOKEN_NEQ) return EVAL_RESULT(val_bool(!equal));
    }

    if (b->op.type == TOKEN_GT || b->op.type == TOKEN_LT || b->op.type == TOKEN_GTE || b->op.type == TOKEN_LTE) {
        if (IS_NUMERIC(left) && IS_NUMERIC(right)) {
            AST_GC_POP();
            if (b->op.type == TOKEN_GT) return EVAL_RESULT(val_bool(sage_cmp_gt(left, right)));
            if (b->op.type == TOKEN_LT) return EVAL_RESULT(val_bool(sage_cmp_lt(left, right)));
            if (b->op.type == TOKEN_GTE) return EVAL_RESULT(val_bool(sage_cmp_gte(left, right)));
            if (b->op.type == TOKEN_LTE) return EVAL_RESULT(val_bool(sage_cmp_lte(left, right)));
        }
        if (IS_STRING(left) && IS_STRING(right)) {
            int cmp = strcmp(AS_STRING(left), AS_STRING(right));
            AST_GC_POP();
            if (b->op.type == TOKEN_GT) return EVAL_RESULT(val_bool(cmp > 0));
            if (b->op.type == TOKEN_LT) return EVAL_RESULT(val_bool(cmp < 0));
            if (b->op.type == TOKEN_GTE) return EVAL_RESULT(val_bool(cmp >= 0));
            if (b->op.type == TOKEN_LTE) return EVAL_RESULT(val_bool(cmp <= 0));
        }
        AST_GC_POP();
        firefly_report(FIREFLY_ERROR, firefly_loc_from_token(&b->op), "unsupported operand types for +: %s and %s", sage_typeof_str(left), sage_typeof_str(right));
            firefly_end();
        return EVAL_RESULT(val_nil());
    }

    switch (b->op.type) {
        // ── Operator overloading via __add__, __sub__, __mul__, __div__, __mod__ ──
        // If left is an instance and has the corresponding dunder method, call it.
        #define CALL_DUNDER(method_name, method_len) do { \
            if (left.type == VAL_INSTANCE && left.as.instance->class_def) { \
                Method* m = class_find_method(left.as.instance->class_def, method_name, method_len); \
                if (m) { \
                    Stmt* mnode = (Stmt*)m->method_stmt; \
                    ProcStmt* mproc = (mnode->type == STMT_ASYNC_PROC) ? &mnode->as.async_proc : &mnode->as.proc; \
                    Env* menv = env_create(left.as.instance->class_def->defining_env \
                                          ? left.as.instance->class_def->defining_env : env); \
                    env_define(menv, "self", 4, left); \
                    int pstart = (mproc->param_count > 0 && strncmp(mproc->params[0].start,"self",4)==0) ? 1 : 0; \
                    if (pstart < mproc->param_count) \
                        env_define(menv, mproc->params[pstart].start, mproc->params[pstart].length, right); \
                    ExecResult dr = interpret(mproc->body, menv); \
                    AST_GC_POP(); \
                    if (dr.is_throwing) return dr; \
                    return EVAL_RESULT(dr.value); \
                } \
            } \
        } while(0)

        case TOKEN_PLUS:
            CALL_DUNDER("__add__", 7);
            if (IS_NUMERIC(left) && IS_NUMERIC(right)) {
                AST_GC_POP();
                return EVAL_RESULT(sage_add(left, right));
            }
            if (IS_STRING(left) && IS_STRING(right)) {
                char* s1 = AS_STRING(left);
                char* s2 = AS_STRING(right);
                size_t len1 = strlen(s1);
                size_t len2 = strlen(s2);
                char* result = SAGE_ALLOC(len1 + len2 + 1);
                memcpy(result, s1, len1);
                memcpy(result + len1, s2, len2 + 1);
                AST_GC_POP();
                return EVAL_RESULT(val_string_take(result));
            }
            // String * int: "abc" * 3 = "abcabcabc"
            if (IS_STRING(left) && IS_NUMERIC(right)) {
                int rep = (int)NUMERIC_AS_INT(right);
                const char* s = AS_STRING(left);
                size_t len = strlen(s);
                char* out = SAGE_ALLOC(len * (size_t)(rep > 0 ? rep : 0) + 1);
                char* p = out;
                for (int ri = 0; ri < rep; ri++) { memcpy(p, s, len); p += len; }
                *p = '\0';
                AST_GC_POP();
                return EVAL_RESULT(val_string_take(out));
            }
            AST_GC_POP();
            firefly_report(FIREFLY_ERROR, firefly_loc_from_token(&b->op), "cannot compare %s and %s", sage_typeof_str(left), sage_typeof_str(right));
            firefly_end();
            return EVAL_RESULT(val_nil());

        case TOKEN_MINUS:
            CALL_DUNDER("__sub__", 7);
            if (!IS_NUMERIC(left) || !IS_NUMERIC(right)) {
                AST_GC_POP();
                fprintf(stderr, "TypeError: unsupported operand types for -: %s and %s\n",
                        sage_typeof_str(left), sage_typeof_str(right));
                return EVAL_RESULT(val_nil());
            }
            AST_GC_POP();
            return EVAL_RESULT(sage_sub(left, right));

        case TOKEN_STAR:
            CALL_DUNDER("__mul__", 7);
            if (IS_NUMERIC(left) && IS_NUMERIC(right)) {
                AST_GC_POP();
                return EVAL_RESULT(sage_mul(left, right));
            }
            // P6: String repetition: "ha" * 3 → "hahaha"
            if (IS_STRING(left) && IS_NUMERIC(right)) {
                int rep = (int)NUMERIC_AS_INT(right);
                if (rep <= 0) { AST_GC_POP(); return EVAL_RESULT(val_string("")); }
                const char* s = AS_STRING(left);
                size_t slen = strlen(s);
                char* out = SAGE_ALLOC(slen * (size_t)rep + 1);
                char* p = out;
                for (int i = 0; i < rep; i++) { memcpy(p, s, slen); p += slen; }
                *p = '\0';
                AST_GC_POP();
                return EVAL_RESULT(val_string_take(out));
            }
            if (IS_NUMERIC(left) && IS_STRING(right)) {
                int rep = (int)NUMERIC_AS_INT(left);
                if (rep <= 0) { AST_GC_POP(); return EVAL_RESULT(val_string("")); }
                const char* s = AS_STRING(right);
                size_t slen = strlen(s);
                char* out = SAGE_ALLOC(slen * (size_t)rep + 1);
                char* p = out;
                for (int i = 0; i < rep; i++) { memcpy(p, s, slen); p += slen; }
                *p = '\0';
                AST_GC_POP();
                return EVAL_RESULT(val_string_take(out));
            }
            AST_GC_POP();
            firefly_report(FIREFLY_ERROR, firefly_loc_from_token(&b->op),
                    "unsupported operand types for *: %s and %s",
                    sage_typeof_str(left), sage_typeof_str(right));
            return EVAL_RESULT(val_nil());

        case TOKEN_SLASH:
            CALL_DUNDER("__div__", 7);
            if (!IS_NUMERIC(left) || !IS_NUMERIC(right)) {
                AST_GC_POP();
                fprintf(stderr, "TypeError: unsupported operand types for /: %s and %s\n",
                        sage_typeof_str(left), sage_typeof_str(right));
                return EVAL_RESULT(val_nil());
            }
            AST_GC_POP();
            // P12: Division by zero check
            if ((IS_INT(right) && AS_INT(right) == 0) ||
                (IS_NUMBER(right) && AS_NUMBER(right) == 0.0)) {
                firefly_div_zero(NULL);  // no expr available in eval_binary
                return EVAL_RESULT(val_nil());
            }
            return EVAL_RESULT(sage_div(left, right));

        case TOKEN_PERCENT:
            CALL_DUNDER("__mod__", 7);
            if (!IS_NUMERIC(left) || !IS_NUMERIC(right)) {
                AST_GC_POP();
                fprintf(stderr, "TypeError: unsupported operand types for %%: %s and %s\n",
                        sage_typeof_str(left), sage_typeof_str(right));
                return EVAL_RESULT(val_nil());
            }
            AST_GC_POP();
            // P12: Modulo by zero check
            if ((IS_INT(right) && AS_INT(right) == 0) ||
                (IS_NUMBER(right) && AS_NUMBER(right) == 0.0)) {
                firefly_div_zero(NULL);
                return EVAL_RESULT(val_nil());
            }
            return EVAL_RESULT(sage_mod(left, right));

        // Phase 9: Bitwise operators — P1: always produce VAL_INT
        case TOKEN_AMP:
            if (!IS_NUMERIC(left) || !IS_NUMERIC(right)) { AST_GC_POP(); return EVAL_RESULT(val_nil()); }
            AST_GC_POP();
            return EVAL_RESULT(val_int(NUMERIC_AS_INT(left) & NUMERIC_AS_INT(right)));

        case TOKEN_PIPE:
            if (!IS_NUMERIC(left) || !IS_NUMERIC(right)) { AST_GC_POP(); return EVAL_RESULT(val_nil()); }
            AST_GC_POP();
            return EVAL_RESULT(val_int(NUMERIC_AS_INT(left) | NUMERIC_AS_INT(right)));

        case TOKEN_CARET:
            if (!IS_NUMERIC(left) || !IS_NUMERIC(right)) { AST_GC_POP(); return EVAL_RESULT(val_nil()); }
            AST_GC_POP();
            return EVAL_RESULT(val_int(NUMERIC_AS_INT(left) ^ NUMERIC_AS_INT(right)));

        case TOKEN_LSHIFT: {
            if (!IS_NUMERIC(left) || !IS_NUMERIC(right)) { AST_GC_POP(); return EVAL_RESULT(val_nil()); }
            int64_t shift = NUMERIC_AS_INT(right);
            AST_GC_POP();
            if (shift < 0 || shift >= 64) return EVAL_RESULT(val_int(0));
            return EVAL_RESULT(val_int(NUMERIC_AS_INT(left) << shift));
        }

        case TOKEN_RSHIFT: {
            if (!IS_NUMERIC(left) || !IS_NUMERIC(right)) { AST_GC_POP(); return EVAL_RESULT(val_nil()); }
            int64_t shift = NUMERIC_AS_INT(right);
            AST_GC_POP();
            if (shift < 0 || shift >= 64) return EVAL_RESULT(val_int(0));
            return EVAL_RESULT(val_int(NUMERIC_AS_INT(left) >> shift));
        }

        default:
            AST_GC_POP();
            return EVAL_RESULT(val_nil());
    }
}

// Inlined eval_expr — recursion depth is checked only at function call
// boundaries (EXPR_CALL), not on every expression. This eliminates 2
// atomic increments per expression evaluation in the critical path.
// Fix all variable/identifier tokens in an Expr tree to use SAGE_ALLOC'd name copies
// This is needed after sub-parsing from a temporary snippet buffer
static void fix_expr_tokens_impl(Expr* e) {
    if (!e) return;
    switch (e->type) {
        case EXPR_VARIABLE: {
            int len = e->as.variable.name.length;
            char* copy = SAGE_ALLOC(len + 1);
            memcpy(copy, e->as.variable.name.start, len);
            copy[len] = '\0';
            e->as.variable.name.start = copy;
            break;
        }
        case EXPR_BINARY:
            fix_expr_tokens_impl(e->as.binary.left);
            fix_expr_tokens_impl(e->as.binary.right);
            break;
        case EXPR_CALL:
            fix_expr_tokens_impl(e->as.call.callee);
            for (int i = 0; i < e->as.call.arg_count; i++)
                fix_expr_tokens_impl(e->as.call.args[i]);
            break;
        case EXPR_GET:
        case EXPR_SET: {
            fix_expr_tokens_impl(e->as.get.object);
            int plen = e->as.get.property.length;
            char* pcopy = SAGE_ALLOC(plen + 1);
            memcpy(pcopy, e->as.get.property.start, plen);
            pcopy[plen] = '\0';
            e->as.get.property.start = pcopy;
            break;
        }
        // Note: more cases can be added as needed for complex interpolations
        // Basic interpolations (variable, arithmetic, method calls) are covered above
        case EXPR_NULLCOAL:
            fix_expr_tokens_impl(e->as.nullcoal.left);
            fix_expr_tokens_impl(e->as.nullcoal.right);
            break;
        case EXPR_FORCE_UNWRAP:
        case EXPR_PROPAGATE:
            fix_expr_tokens_impl(e->as.unwrap.operand);
            break;
        default: break;
    }
}
void fix_expr_tokens(Expr* e, const char* src_buf) {
    (void)src_buf;
    fix_expr_tokens_impl(e);
}

static ExecResult eval_expr(Expr* expr, Env* env) {
    switch (expr->type) {
        case EXPR_NUMBER: return EVAL_RESULT(val_number(expr->as.number.value));
        case EXPR_INT:    return EVAL_RESULT(val_int(expr->as.int_val.value));
        case EXPR_STRING: return EVAL_RESULT(val_string(expr->as.string.value));
        case EXPR_BOOL:   return EVAL_RESULT(val_bool(expr->as.boolean.value));
        case EXPR_NIL:    return EVAL_RESULT(val_nil());
        
        case EXPR_ARRAY: {
            gc_pin();
            Value arr = val_array();
            for (int i = 0; i < expr->as.array.count; i++) {
                Expr* el = expr->as.array.elements[i];
                // Spread: EXPR_BINARY with TOKEN_DOTDOT op and NULL right
                if (el->type == EXPR_BINARY &&
                    el->as.binary.op.type == TOKEN_DOTDOT &&
                    el->as.binary.right == NULL) {
                    ExecResult sr = eval_expr(el->as.binary.left, env);
                    if (sr.is_throwing) { gc_unpin(); return sr; }
                    if (IS_ARRAY(sr.value)) {
                        ArrayValue* av = AS_ARRAY(sr.value);
                        for (int j = 0; j < av->count; j++)
                            array_push(&arr, av->elements[j]);
                    }
                    continue;
                }
                ExecResult elem_result = eval_expr(el, env);
                if (elem_result.is_throwing) { gc_unpin(); return elem_result; }
                array_push(&arr, elem_result.value);
            }
            gc_unpin();
            return EVAL_RESULT(arr);
        }

        case EXPR_DICT: {
            gc_pin();
            Value dict = val_dict();
            for (int i = 0; i < expr->as.dict.count; i++) {
                ExecResult val_result = eval_expr(expr->as.dict.values[i], env);
                if (val_result.is_throwing) {
                    gc_unpin();
                    return val_result;
                }
                dict_set(&dict, expr->as.dict.keys[i], val_result.value);
            }
            gc_unpin();
            return EVAL_RESULT(dict);
        }

        case EXPR_TUPLE: {
            gc_pin();
            Value* elements = SAGE_ALLOC(sizeof(Value) * expr->as.tuple.count);
            for (int i = 0; i < expr->as.tuple.count; i++) {
                ExecResult elem_result = eval_expr(expr->as.tuple.elements[i], env);
                if (elem_result.is_throwing) {
                    free(elements);
                    gc_unpin();
                    return elem_result;
                }
                elements[i] = elem_result.value;
            }
            Value tuple = val_tuple(elements, expr->as.tuple.count);
            free(elements);
            gc_unpin();
            return EVAL_RESULT(tuple);
        }

        case EXPR_INDEX: {
            ExecResult arr_result = eval_expr(expr->as.index.array, env);
            if (arr_result.is_throwing) return arr_result;
            Value arr = arr_result.value;
            AST_GC_PUSH(arr);
            
            ExecResult idx_result = eval_expr(expr->as.index.index, env);
            if (idx_result.is_throwing) { AST_GC_POP(); return idx_result; }
            Value idx = idx_result.value;
            
            ExecResult result;
            if (arr.type == VAL_ARRAY && IS_NUMERIC(idx)) {
                int index = (int)NUMERIC_AS_INT(idx);
                if (index < 0) index += arr.as.array->count;
                if (index < 0 || index >= arr.as.array->count) {
                    firefly_index_error(expr, (int)NUMERIC_AS_INT(idx), arr.as.array->count, "Array");
                    result = EVAL_RESULT(val_nil());
                } else {
                    result = EVAL_RESULT(array_get(&arr, index));
                }
            } else if (arr.type == VAL_TUPLE && IS_NUMERIC(idx)) {
                int index = (int)NUMERIC_AS_INT(idx);
                if (index < 0) index += arr.as.tuple->count;
                if (index < 0 || index >= arr.as.tuple->count) {
                    firefly_index_error(expr, (int)NUMERIC_AS_INT(idx), arr.as.tuple->count, "Tuple");
                    result = EVAL_RESULT(val_nil());
                } else {
                    result = EVAL_RESULT(tuple_get(&arr, index));
                }
            } else if (arr.type == VAL_STRING && IS_NUMERIC(idx)) {
                int index = (int)NUMERIC_AS_INT(idx);
                char* str = AS_STRING(arr);
                int slen = (int)strlen(str);
                if (index < 0) index += slen;
                if (index < 0 || index >= slen) {
                    firefly_index_error(expr, index, slen, "str");
                    AST_GC_POP();
                    return EVAL_RESULT(val_nil());
                }
                char* ch = SAGE_ALLOC(2);
                ch[0] = str[index];
                ch[1] = '\0';
                result = EVAL_RESULT(val_string_take(ch));
            } else if (arr.type == VAL_DICT && IS_STRING(idx)) {
                result = EVAL_RESULT(dict_get(&arr, AS_STRING(idx)));
            } else {
                firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr), "invalid indexing operation — can only index Array, Dict, Tuple, or str");
                result = EVAL_RESULT(val_nil());
            }
            AST_GC_POP();
            return result;
        }

        case EXPR_INDEX_SET: {
            ExecResult arr_result = eval_expr(expr->as.index_set.array, env);
            if (arr_result.is_throwing) return arr_result;
            Value arr = arr_result.value;
            AST_GC_PUSH(arr);

            ExecResult idx_result = eval_expr(expr->as.index_set.index, env);
            if (idx_result.is_throwing) { AST_GC_POP(); return idx_result; }
            Value idx = idx_result.value;
            AST_GC_PUSH(idx);

            ExecResult val_result = eval_expr(expr->as.index_set.value, env);
            if (val_result.is_throwing) { AST_GC_POP_N(2); return val_result; }
            Value value = val_result.value;

            ExecResult result;
            if (arr.type == VAL_ARRAY && IS_NUMERIC(idx)) {
                int index = (int)NUMERIC_AS_INT(idx);
                array_set(&arr, index, value);
                result = EVAL_RESULT(value);
            } else if (arr.type == VAL_DICT && IS_STRING(idx)) {
                dict_set(&arr, AS_STRING(idx), value);
                result = EVAL_RESULT(value);
            } else {
                firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr), "invalid index assignment — can only assign to Array or Dict elements");
                result = EVAL_RESULT(val_nil());
            }
            AST_GC_POP_N(2);
            return result;
        }

        case EXPR_SLICE: {
            ExecResult arr_result = eval_expr(expr->as.slice.array, env);
            if (arr_result.is_throwing) return arr_result;
            Value arr = arr_result.value;
            AST_GC_PUSH(arr);
            
            if (arr.type != VAL_ARRAY && arr.type != VAL_STRING) {
                firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr), "can only slice arrays or strings"); firefly_end();
                AST_GC_POP();
                return EVAL_RESULT(val_nil());
            }
            
            int start = 0;
            int end = 0;
            if (arr.type == VAL_ARRAY) {
                end = arr.as.array->count;
            } else {
                end = (int)strlen(arr.as.string);
            }
            
            if (expr->as.slice.start != NULL) {
                ExecResult start_result = eval_expr(expr->as.slice.start, env);
                if (start_result.is_throwing) { AST_GC_POP(); return start_result; }
                if (!IS_NUMERIC(start_result.value)) { AST_GC_POP(); return EVAL_RESULT(val_nil()); }
                start = (int)NUMERIC_AS_INT(start_result.value);
            }
            
            if (expr->as.slice.end != NULL) {
                ExecResult end_result = eval_expr(expr->as.slice.end, env);
                if (end_result.is_throwing) { AST_GC_POP(); return end_result; }
                if (!IS_NUMERIC(end_result.value)) { AST_GC_POP(); return EVAL_RESULT(val_nil()); }
                end = (int)NUMERIC_AS_INT(end_result.value);
            }
            
            ExecResult result;
            if (arr.type == VAL_ARRAY) {
                result = EVAL_RESULT(array_slice(&arr, start, end));
            } else {
                result = EVAL_RESULT(string_slice(&arr, start, end));
            }
            AST_GC_POP();
            return result;
        }

        case EXPR_GET: {
            ExecResult obj_result = eval_expr(expr->as.get.object, env);
            if (obj_result.is_throwing) return obj_result;
            Value object = obj_result.value;
            Token prop = expr->as.get.property;

            if (IS_INSTANCE(object)) {
                Value result = instance_get_field(object.as.instance, prop.start, prop.length);
                return EVAL_RESULT(result);
            }

            if (IS_MODULE(object)) {
                Module* mod = (Module*)object.as.pointer->ptr;
                if (mod == NULL) {
                     firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr), "module is NULL"); firefly_end();
                     return EVAL_RESULT(val_nil());
                }
                int found = 0;
                Value result = module_get_attr(mod, prop.start, prop.length, &found);
                if (!found) {
                    firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr), "module '%s' has no attribute '%.*s'", mod->name, prop.length, prop.start); firefly_end();
                    return EVAL_RESULT(val_nil());
                }
                return EVAL_RESULT(result);
            }

            // P2: Dict dot-access (enums and regular dicts)
            if (IS_DICT(object)) {
                char key[256];
                int klen = prop.length < 255 ? prop.length : 255;
                memcpy(key, prop.start, klen);
                key[klen] = '\0';
                Value result = dict_get(&object, key);
                return EVAL_RESULT(result);
            }

            // P13: PyObject attribute access — np.pi, math.e, etc.
#ifndef SAGE_NO_PYTHON
            if (object.type == VAL_POINTER && object.as.pointer && object.as.pointer->type_tag == 99) {
                extern Value sage_py_getattr_direct(Value obj, const char* name, int name_len);
                Value result = sage_py_getattr_direct(object, prop.start, prop.length);
                return EVAL_RESULT(result);
            }
#endif

            // P2: String property access (length)
            if (IS_STRING(object)) {
                if (prop.length == 6 && strncmp(prop.start, "length", 6) == 0) {
                    return EVAL_RESULT(val_int((int64_t)strlen(AS_STRING(object))));
                }
            }

            // P2: Array property access (length)
            if (IS_ARRAY(object)) {
                if (prop.length == 6 && strncmp(prop.start, "length", 6) == 0) {
                    return EVAL_RESULT(val_int(object.as.array->count));
                }
            }

            // P9: Firefly — no property on this type
            firefly_no_property(expr, sage_typeof_str(object), prop.start, prop.length);
            return EVAL_RESULT(val_nil());
        }

        case EXPR_SET: {
            // Handle variable assignment (object is NULL)
            if (expr->as.set.object == NULL) {
                // Variable reassignment: x = value
                Token var_name = expr->as.set.property;
                ExecResult val_result = eval_expr(expr->as.set.value, env);
                if (val_result.is_throwing) return val_result;
                Value value = val_result.value;

                // Inline caching for variable assignment
                if (expr->as.set.cached_env_id == env->id) {
                    EnvNode* node = expr->as.set.cached_node;
                    // P4: Immutability check
                    if (node->is_const) {
                        firefly_immutable_error(expr, var_name.start, var_name.length);
                        return EVAL_RESULT(val_nil());
                    }
                    if (gc.mode == GC_MODE_ARC || gc.mode == GC_MODE_ORC) {
                        arc_assign_value(&node->value, value);
                    } else {
                        GC_WRITE_BARRIER(node->value);
                        node->value = value;
                    }
                    // Keep map in sync when env has one
                    if (env->map) {
                        envmap_set(env->map, var_name.start, var_name.length, value);
                    }
                    return EVAL_RESULT(value);
                }
                
                // Try to update the variable in the environment
                Env* found_env = NULL;
                EnvNode* found_node = NULL;
                if (env_get_node(env, var_name.start, var_name.length, &found_env, &found_node)) {
                    // P4: Immutability check
                    if (found_node->is_const) {
                        firefly_immutable_error(expr, var_name.start, var_name.length);
                        return EVAL_RESULT(val_nil());
                    }
                    if (found_env == env) {
                        expr->as.set.cached_env_id = env->id;
                        expr->as.set.cached_node = found_node;
                    }
                    if (gc.mode == GC_MODE_ARC || gc.mode == GC_MODE_ORC) {
                        arc_assign_value(&found_node->value, value);
                    } else {
                        GC_WRITE_BARRIER(found_node->value);
                        found_node->value = value;
                    }
                    // Sync map index in the env that owns this variable
                    if (found_env && found_env->map) {
                        envmap_set(found_env->map, var_name.start, var_name.length, value);
                    }
                    return EVAL_RESULT(value);
                }
                // P9: Firefly undefined variable with "did you mean?"
                firefly_undefined_var(expr, var_name.start, var_name.length, env);
                return EVAL_RESULT(val_nil());
            }
            
            // Property assignment: obj.prop = value
            ExecResult obj_result = eval_expr(expr->as.set.object, env);
            if (obj_result.is_throwing) return obj_result;
            Value object = obj_result.value;
            AST_GC_PUSH(object);
            
            if (!IS_INSTANCE(object)) {
                firefly_no_property(expr, sage_typeof_str(object), expr->as.set.property.start, expr->as.set.property.length);
                AST_GC_POP();
                return EVAL_RESULT(val_nil());
            }
            
            ExecResult val_result = eval_expr(expr->as.set.value, env);
            if (val_result.is_throwing) { AST_GC_POP(); return val_result; }
            Value value = val_result.value;
            
            Token prop = expr->as.set.property;
            
            // Optimized property assignment: no string allocation/copy
            instance_set_field(object.as.instance, prop.start, prop.length, value);
            AST_GC_POP();
            return EVAL_RESULT(value);
        }

        case EXPR_AWAIT: {
            ExecResult inner = eval_expr(expr->as.await.expression, env);
            if (inner.is_throwing) return inner;
            Value v = inner.value;
            if (IS_THREAD(v)) {
                // Join the thread and return its result
                ThreadValue* tv = AS_THREAD(v);
                if (!tv->joined) {
                    sage_thread_t* handle = (sage_thread_t*)tv->handle;
                    sage_thread_join(*handle, NULL);
                    tv->joined = 1;
                }
                typedef struct { FunctionValue* func; int arg_count; Value* args; Value result; } SageThreadData;
                SageThreadData* td = (SageThreadData*)tv->data;
                return EVAL_RESULT(td->result);
            }
            // If not a thread, just return the value (already resolved)
            return EVAL_RESULT(v);
        }

        case EXPR_BINARY:
            return eval_binary(&expr->as.binary, env);

        case EXPR_VARIABLE: {
            // Inline caching for variable lookup
            if (expr->as.variable.cached_env_id == env->id) {
                return EVAL_RESULT(expr->as.variable.cached_node->value);
            }

            Value val;
            Token t = expr->as.variable.name;
            Env* found_env = NULL;
            EnvNode* found_node = NULL;
            if (env_get_node(env, t.start, t.length, &found_env, &found_node)) {
                // Only cache if found in the current environment (most frequent case in loops)
                if (found_env == env) {
                    expr->as.variable.cached_env_id = env->id;
                    expr->as.variable.cached_node = found_node;
                }
                return EVAL_RESULT(found_node->value);
            }
            // P9: Firefly undefined variable with suggestions
            {
                firefly_undefined_var(expr, t.start, t.length, env);
                char _err_buf[256];
                snprintf(_err_buf, sizeof(_err_buf), "Undefined variable '%.*s'", t.length, t.start);
                return EVAL_EXCEPTION(val_exception(_err_buf));
            }
        }

        case EXPR_CALL: {
            Expr* callee_expr = expr->as.call.callee;

            if (callee_expr->type == EXPR_GET) {
                ExecResult obj_result = eval_expr(callee_expr->as.get.object, env);
                if (obj_result.is_throwing) return obj_result;
                Value object = obj_result.value;
                AST_GC_PUSH(object);

                if (IS_INSTANCE(object)) {
                    Token method_token = callee_expr->as.get.property;

                    Method* method = class_find_method(object.as.instance->class_def, method_token.start, method_token.length);
                    if (!method) {
                        { firefly_no_method(callee_expr, sage_typeof_str(object), method_token.start, method_token.length); }
                        AST_GC_POP();
                        return EVAL_RESULT(val_nil());
                    }

                    Stmt* method_node = (Stmt*)method->method_stmt;
                    ProcStmt* method_stmt = (method_node->type == STMT_ASYNC_PROC) ? &method_node->as.async_proc : &method_node->as.proc;
                    
                    Env* defining = object.as.instance->class_def->defining_env;
                    Env* method_env = env_create(defining ? defining : env);
                    AST_GC_PUSH_ENV(method_env);
                    env_define_const(method_env, "self", 4, object);
                    // Track which class owns this method (for super resolution)
                    ClassValue* owner = class_find_method_owner(object.as.instance->class_def, method_token.start, method_token.length);
                    if (owner) env_define_const(method_env, "__class__", 9, val_class(owner));

                    int param_start = (method_stmt->param_count > 0 &&
                                      strncmp(method_stmt->params[0].start, "self", 4) == 0) ? 1 : 0;

                    int arg_count = expr->as.call.arg_count;
                    Value* eval_args = NULL;
                    int pushed_args = 0;
                    if (method_stmt->param_count > param_start) {
                        eval_args = SAGE_ALLOC(sizeof(Value) * (method_stmt->param_count - param_start));
                        for (int i = 0; i < arg_count && i < method_stmt->param_count - param_start; i++) {
                            ExecResult arg_result = eval_expr(expr->as.call.args[i], env);
                            if (arg_result.is_throwing) { 
                                free(eval_args); 
                                AST_GC_POP_ENV(); 
                                AST_GC_POP_N(1 + pushed_args); 
                                return arg_result; 
                            }
                            eval_args[i] = arg_result.value;
                            AST_GC_PUSH(eval_args[i]);
                            pushed_args++;
                            env_define_const(method_env, method_stmt->params[i + param_start].start,
                                       method_stmt->params[i + param_start].length, arg_result.value);
                        }
                        // Missing args set to nil
                        for (int i = arg_count; i < method_stmt->param_count - param_start; i++) {
                            eval_args[i] = val_nil();
                            env_define_const(method_env, method_stmt->params[i + param_start].start,
                                       method_stmt->params[i + param_start].length, val_nil());
                        }
                    }

                    if (method_node->type == STMT_ASYNC_PROC) {
                        // Create a FunctionValue wrapper for this method call
                        FunctionValue* fv = SAGE_ALLOC(sizeof(FunctionValue));
                        fv->proc = method_stmt;
                        fv->closure = method_env;
                        fv->is_async = 1;
                        fv->is_vm = 0;
                        Value callee;
                        callee.type = VAL_FUNCTION;
                        callee.as.function = fv;

                        int total_params = method_stmt->param_count - param_start;
                        Value spawn_args[1 + total_params];
                        spawn_args[0] = callee;
                        for (int i = 0; i < total_params; i++) {
                            spawn_args[i + 1] = eval_args[i];
                        }
                        if (eval_args) free(eval_args);

                        extern Value thread_spawn_native(int argCount, Value* args);
                        Value handle = thread_spawn_native(1 + total_params, spawn_args);
                        AST_GC_POP_ENV();
                        AST_GC_POP_N(1 + pushed_args);
                        return EVAL_RESULT(handle);
                    }

                    if (eval_args) free(eval_args);
                    ExecResult res = interpret(method_stmt->body, method_env);
                    AST_GC_POP_ENV();
                    AST_GC_POP_N(1 + pushed_args);
                    if (res.is_throwing) return res;
                    return EVAL_RESULT(res.value);
                }

                // P2: Builtin method dispatch for string, array, dict
                if (!IS_INSTANCE(object)) {
                    Token method_token = callee_expr->as.get.property;
                    int arg_count = expr->as.call.arg_count;
                    Value eval_args_buf[16];
                    Value* eval_args = arg_count <= 16 ? eval_args_buf : SAGE_ALLOC(sizeof(Value) * arg_count);
                    for (int i = 0; i < arg_count; i++) {
                        ExecResult arg_result = eval_expr(expr->as.call.args[i], env);
                        if (arg_result.is_throwing) {
                            if (eval_args != eval_args_buf) free(eval_args);
                            AST_GC_POP();
                            return arg_result;
                        }
                        eval_args[i] = arg_result.value;
                    }
                    Value method_result;
                    if (builtin_method_call(object, method_token.start, method_token.length,
                                           arg_count, eval_args, &method_result)) {
                        if (eval_args != eval_args_buf) free(eval_args);
                        AST_GC_POP();
                        return EVAL_RESULT(method_result);
                    }
                    if (eval_args != eval_args_buf) free(eval_args);
                    
                    // P13: ADT enum constructor via method call — Shape.Circle(5.0)
                    if (IS_DICT(object)) {
                        Value variant_val = dict_get(&object, (char*)method_token.start);
                        // dict_get uses null-terminated strings, need to build one
                        char _mname[128];
                        int _ml = method_token.length < 127 ? method_token.length : 127;
                        memcpy(_mname, method_token.start, _ml);
                        _mname[_ml] = '\0';
                        variant_val = dict_get(&object, _mname);
                        if (IS_DICT(variant_val)) {
                            Value ctor_type = dict_get(&variant_val, "__type");
                            if (IS_STRING(ctor_type) && strcmp(AS_STRING(ctor_type), "__enum_ctor__") == 0) {
                                Value variant_type = dict_get(&variant_val, "__variant_type");
                                Value tag_val = dict_get(&variant_val, "__tag");
                                Value field_names = dict_get(&variant_val, "__fields");
                                // Re-evaluate args for the constructor
                                int carg_count = expr->as.call.arg_count;
                                gc_pin();
                                Value result = val_dict();
                                dict_set(&result, "__type", variant_type);
                                dict_set(&result, "__tag", tag_val);
                                if (IS_ARRAY(field_names)) {
                                    ArrayValue* fnames = field_names.as.array;
                                    for (int i = 0; i < fnames->count && i < carg_count; i++) {
                                        ExecResult arg_res = eval_expr(expr->as.call.args[i], env);
                                        if (arg_res.is_throwing) { gc_unpin(); AST_GC_POP(); return arg_res; }
                                        dict_set(&result, AS_STRING(fnames->elements[i]), arg_res.value);
                                    }
                                    if (fnames->count == 1 && carg_count >= 1) {
                                        ExecResult arg_res = eval_expr(expr->as.call.args[0], env);
                                        if (!arg_res.is_throwing) dict_set(&result, "__val", arg_res.value);
                                    }
                                }
                                gc_unpin();
                                AST_GC_POP();
                                return EVAL_RESULT(result);
                            }
                        }
                    }
                    // P13: PyObject method call — math.sqrt(144), json.dumps(data)
#ifndef SAGE_NO_PYTHON
                    if (object.type == VAL_POINTER && object.as.pointer &&
                        object.as.pointer->type_tag == 99) {
#endif
                        // Get the Python attribute, then call it
                        extern Value sage_py_getattr_direct(Value obj, const char* name, int name_len);
                        extern Value sage_py_call_direct(Value callable, int argc, Value* args);
                        Value py_method = sage_py_getattr_direct(object, method_token.start, method_token.length);
                        if (py_method.type == VAL_POINTER && py_method.as.pointer &&
                            py_method.as.pointer->type_tag == 99) {
                            // Re-evaluate args
                            int nargs = expr->as.call.arg_count;
                            Value cargs[16];
                            Value* cargs_ptr = nargs <= 16 ? cargs : SAGE_ALLOC(sizeof(Value) * nargs);
                            for (int ci = 0; ci < nargs; ci++) {
                                ExecResult ar = eval_expr(expr->as.call.args[ci], env);
                                if (ar.is_throwing) {
                                    if (cargs_ptr != cargs) free(cargs_ptr);
                                    AST_GC_POP();
                                    return ar;
                                }
                                cargs_ptr[ci] = ar.value;
                            }
                            Value result = sage_py_call_direct(py_method, nargs, cargs_ptr);
                            if (cargs_ptr != cargs) free(cargs_ptr);
                            AST_GC_POP();
                            return EVAL_RESULT(result);
                        }
                    }
                    
                    // P9: Firefly — no method found on this type
                    if (!IS_MODULE(object) && object.type != VAL_CLASS) {
                        firefly_no_method(callee_expr, sage_typeof_str(object),
                                         method_token.start, method_token.length);
                        AST_GC_POP();
                        return EVAL_RESULT(val_nil());
                    }
                }

                AST_GC_POP();
            }

            // super.method(args) — call parent class method
            if (callee_expr->type == EXPR_SUPER) {
                Token method_token = callee_expr->as.super_expr.method;
                // Get 'self' from environment to find the instance
                Value self_val;
                if (!env_get(env, "self", 4, &self_val) || !IS_INSTANCE(self_val)) {
                    firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr), "'super' can only be used inside a method"); firefly_end();
                    return EVAL_RESULT(val_nil());
                }
                // Get __class__ from env (the class owning the current method)
                // If not set, fall back to instance's class
                Value class_ctx;
                ClassValue* current_class;
                if (env_get(env, "__class__", 9, &class_ctx) && class_ctx.type == VAL_CLASS) {
                    current_class = class_ctx.as.class_val;
                } else {
                    current_class = self_val.as.instance->class_def;
                }
                ClassValue* parent_class = current_class->parent;
                if (!parent_class) {
                    firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr), "class has no parent class for 'super'"); firefly_end();
                    return EVAL_RESULT(val_nil());
                }

                Method* method = class_find_method(parent_class, method_token.start, method_token.length);
                if (!method) {
                    fprintf(stderr, "Runtime Error: Parent class has no method '%.*s'.\n", method_token.length, method_token.start);
                    return EVAL_RESULT(val_nil());
                }

                Stmt* method_node = (Stmt*)method->method_stmt;
                ProcStmt* method_stmt = (method_node->type == STMT_ASYNC_PROC) ? &method_node->as.async_proc : &method_node->as.proc;
                Env* parent_defining = parent_class->defining_env;
                Env* method_env = env_create(parent_defining ? parent_defining : env);
                AST_GC_PUSH_ENV(method_env);
                
                // Set __class__ to the parent class so nested super calls resolve correctly
                env_define_const(method_env, "__class__", 9, val_class(parent_class));

                // super calls: auto-inject self, skip self param like regular methods
                env_define_const(method_env, "self", 4, self_val);
                int param_start = (method_stmt->param_count > 0 &&
                                  strncmp(method_stmt->params[0].start, "self", 4) == 0) ? 1 : 0;
                
                int arg_count = expr->as.call.arg_count;
                Value* eval_args = NULL;
                int pushed_args = 0;
                if (method_stmt->param_count > param_start) {
                    eval_args = SAGE_ALLOC(sizeof(Value) * (method_stmt->param_count - param_start));
                    for (int i = 0; i < arg_count && i < method_stmt->param_count - param_start; i++) {
                        ExecResult arg_result = eval_expr(expr->as.call.args[i], env);
                        if (arg_result.is_throwing) { 
                            free(eval_args); 
                            AST_GC_POP_ENV(); 
                            AST_GC_POP_N(pushed_args);
                            return arg_result; 
                        }
                        eval_args[i] = arg_result.value;
                        AST_GC_PUSH(eval_args[i]);
                        pushed_args++;
                        env_define_const(method_env, method_stmt->params[i + param_start].start,
                                   method_stmt->params[i + param_start].length, arg_result.value);
                    }
                    for (int i = arg_count; i < method_stmt->param_count - param_start; i++) {
                        eval_args[i] = val_nil();
                        env_define_const(method_env, method_stmt->params[i + param_start].start,
                                   method_stmt->params[i + param_start].length, val_nil());
                    }
                }

                if (method_node->type == STMT_ASYNC_PROC) {
                    FunctionValue* fv = SAGE_ALLOC(sizeof(FunctionValue));
                    fv->proc = method_stmt;
                    fv->closure = method_env;
                    fv->is_async = 1;
                    fv->is_vm = 0;
                    Value callee;
                    callee.type = VAL_FUNCTION;
                    callee.as.function = fv;

                    int total_params = method_stmt->param_count - param_start;
                    Value spawn_args[1 + total_params];
                    spawn_args[0] = callee;
                    for (int i = 0; i < total_params; i++) {
                        spawn_args[i + 1] = eval_args[i];
                    }
                    if (eval_args) free(eval_args);

                    extern Value thread_spawn_native(int argCount, Value* args);
                    Value handle = thread_spawn_native(1 + total_params, spawn_args);
                    AST_GC_POP_ENV();
                    AST_GC_POP_N(pushed_args);
                    return EVAL_RESULT(handle);
                }

                if (eval_args) free(eval_args);
                ExecResult res = interpret(method_stmt->body, method_env);
                AST_GC_POP_ENV();
                AST_GC_POP_N(pushed_args);
                if (res.is_throwing) return res;
                return EVAL_RESULT(res.value);
            }

            ExecResult callee_result = eval_expr(callee_expr, env);
            if (callee_result.is_throwing) return callee_result;
            Value callee_value = callee_result.value;
            AST_GC_PUSH(callee_value);

            // Phase 4: extern.proc call — dispatch via libffi
            if (IS_DICT(callee_value)) {
                Value ext_type = dict_get(&callee_value, "__type");
                if (IS_STRING(ext_type) && strcmp(AS_STRING(ext_type), "extern.proc") == 0) {
                    Value lib_val  = dict_get(&callee_value, "__lib");
                    Value sym_name = dict_get(&callee_value, "__sym_name");
                    Value ret_type = dict_get(&callee_value, "__ret");

                    void* lib_handle = RTLD_DEFAULT;
                    if (IS_CLIB(lib_val) && AS_CLIB(lib_val)->handle)
                        lib_handle = AS_CLIB(lib_val)->handle;

                    const char* sym_str = IS_STRING(sym_name) ? AS_STRING(sym_name) : "";
                    dlerror();
                    void* sym = dlsym(lib_handle, sym_str);
                    if (!sym) {
                        fprintf(stderr, "Runtime Error: extern proc '%s' not found in library\n", sym_str);
                        AST_GC_POP();
                        return EVAL_RESULT(val_nil());
                    }

                    // Evaluate arguments
                    int argc = expr->as.call.arg_count;
                    if (argc > 32) argc = 32;
                    Value sage_args[32];
                    for (int i = 0; i < argc; i++) {
                        ExecResult ar = eval_expr(expr->as.call.args[i], env);
                        if (ar.is_throwing) { AST_GC_POP(); return ar; }
                        sage_args[i] = ar.value;
                    }

                    // Determine return type
                    const char* ret_str = IS_STRING(ret_type) ? AS_STRING(ret_type) : "void";
                    int ret_is_str  = (strcmp(ret_str,"str")==0 || strstr(ret_str,"str") != NULL);
                    int ret_is_void = (strcmp(ret_str,"void")==0);
                    int ret_is_int  = (strcmp(ret_str,"int")==0 || strcmp(ret_str,"uint")==0 ||
                                      strcmp(ret_str,"i32")==0 || strcmp(ret_str,"u32")==0 ||
                                      strcmp(ret_str,"i64")==0 || strcmp(ret_str,"u64")==0 ||
                                      strcmp(ret_str,"bool")==0);
                    int ret_is_flt  = (strcmp(ret_str,"float")==0 || strcmp(ret_str,"f64")==0 ||
                                      strcmp(ret_str,"f32")==0);

                    // Build libffi arg types and values
                    // Sage numbers go as double (covers both float and small ints)
                    // Sage strings/nil go as pointer
                    ffi_cif cif;
                    ffi_type* arg_types[32];
                    void*     arg_vals[32];
                    double    num_storage[32];
                    const char* str_storage[32];
                    void*     ptr_storage[32];

                    for (int i = 0; i < argc; i++) {
                        Value av = sage_args[i];
                        if (IS_NUMERIC(av)) {
                            num_storage[i] = NUMERIC_AS_DOUBLE(av);
                            arg_types[i]   = &ffi_type_double;
                            arg_vals[i]    = &num_storage[i];
                        } else if (IS_STRING(av)) {
                            str_storage[i] = AS_STRING(av);
                            arg_types[i]   = &ffi_type_pointer;
                            arg_vals[i]    = &str_storage[i];
                        } else {
                            ptr_storage[i] = NULL;
                            arg_types[i]   = &ffi_type_pointer;
                            arg_vals[i]    = &ptr_storage[i];
                        }
                    }

                    // Return type: use proper ffi type for sign extension
                    ffi_type* rtype = ret_is_void ? &ffi_type_void :
                                      ret_is_str  ? &ffi_type_pointer :
                                      ret_is_flt  ? &ffi_type_double :
                                      ret_is_int  ? &ffi_type_sint :
                                                    &ffi_type_slong;

                    ffi_type* fixed_args[32];
                    memcpy(fixed_args, arg_types, sizeof(ffi_type*) * (size_t)argc);
                    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)argc, rtype,
                                     argc > 0 ? fixed_args : NULL) != FFI_OK) {
                        fprintf(stderr, "Runtime Error: ffi_prep_cif failed for '%s'\n", sym_str);
                        AST_GC_POP();
                        return EVAL_RESULT(val_nil());
                    }

                    Value result = val_nil();
                    if (ret_is_void) {
                        ffi_call(&cif, (void(*)(void))sym, NULL, argc > 0 ? arg_vals : NULL);
                    } else if (ret_is_str) {
                        void* ret_ptr = NULL;
                        ffi_call(&cif, (void(*)(void))sym, &ret_ptr, argc > 0 ? arg_vals : NULL);
                        result = ret_ptr ? val_string((const char*)ret_ptr) : val_nil();
                    } else if (ret_is_flt) {
                        double ret_d = 0;
                        ffi_call(&cif, (void(*)(void))sym, &ret_d, argc > 0 ? arg_vals : NULL);
                        result = val_number(ret_d);
                    } else {
                        // signed int return — properly sign-extended
                        long ret_l = 0;
                        ffi_call(&cif, (void(*)(void))sym, &ret_l, argc > 0 ? arg_vals : NULL);
                        result = val_int((int64_t)ret_l);
                    }

                    AST_GC_POP();
                    return EVAL_RESULT(result);
                }
            }

            // P13: PyObject callable — math.sqrt(144), np.array([1,2,3])
#ifndef SAGE_NO_PYTHON
            if (callee_value.type == VAL_POINTER && callee_value.as.pointer &&
                callee_value.as.pointer->type_tag == 99) {
#endif
                // Collect args into a Sage array, delegate to sage_python helper
                extern Value sage_py_call_direct(Value callable, int argc, Value* args);
                int nargs = expr->as.call.arg_count;
                Value call_args[16];
                Value* args_ptr = nargs <= 16 ? call_args : SAGE_ALLOC(sizeof(Value) * nargs);
                for (int i = 0; i < nargs; i++) {
                    ExecResult arg_r = eval_expr(expr->as.call.args[i], env);
                    if (arg_r.is_throwing) {
                        if (args_ptr != call_args) free(args_ptr);
                        AST_GC_POP();
                        return arg_r;
                    }
                    args_ptr[i] = arg_r.value;
                }
                Value result = sage_py_call_direct(callee_value, nargs, args_ptr);
                if (args_ptr != call_args) free(args_ptr);
                AST_GC_POP();
                return EVAL_RESULT(result);
            }

            // P3: ADT enum variant constructor — calling Shape.Circle(5.0)
            // The callee is a dict with __type == "__enum_ctor__"
            if (IS_DICT(callee_value)) {
                Value ctor_type = dict_get(&callee_value, "__type");
                if (IS_STRING(ctor_type) && strcmp(AS_STRING(ctor_type), "__enum_ctor__") == 0) {
                    Value variant_type = dict_get(&callee_value, "__variant_type");
                    Value tag_val = dict_get(&callee_value, "__tag");
                    Value field_names = dict_get(&callee_value, "__fields");

                    int arg_count = expr->as.call.arg_count;
                    gc_pin();
                    Value result = val_dict();
                    dict_set(&result, "__type", variant_type);
                    dict_set(&result, "__tag", tag_val);

                    // Set fields by name from args
                    if (IS_ARRAY(field_names)) {
                        ArrayValue* fnames = field_names.as.array;
                        for (int i = 0; i < fnames->count && i < arg_count; i++) {
                            ExecResult arg_res = eval_expr(expr->as.call.args[i], env);
                            if (arg_res.is_throwing) { gc_unpin(); AST_GC_POP(); return arg_res; }
                            dict_set(&result, AS_STRING(fnames->elements[i]), arg_res.value);
                        }
                        // Also store as __val for single-field variants (compat with Some/Ok pattern)
                        if (fnames->count == 1 && arg_count >= 1) {
                            ExecResult arg_res = eval_expr(expr->as.call.args[0], env);
                            if (!arg_res.is_throwing) dict_set(&result, "__val", arg_res.value);
                        }
                    }
                    gc_unpin();
                    AST_GC_POP();
                    return EVAL_RESULT(result);
                }
            }

            if (callee_value.type == VAL_NATIVE) {
                if (callee_value.as.native == NULL) {
                    firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr), "attempted to call a null native function"); firefly_end();
                    AST_GC_POP();
                    return EVAL_RESULT(val_nil());
                }
                int count = expr->as.call.arg_count;
                if (count > 255) {
                    firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr), "too many arguments (%d, max 255)", count); firefly_end();
                    AST_GC_POP();
                    return EVAL_RESULT(val_nil());
                }
                Value args[255];
                int pushed_args = 0;
                for (int i = 0; i < count; i++) {
                    ExecResult arg_result = eval_expr(expr->as.call.args[i], env);
                    if (arg_result.is_throwing) { AST_GC_POP_N(1 + pushed_args); return arg_result; }
                    args[i] = arg_result.value;
                    AST_GC_PUSH(args[i]);
                    pushed_args++;
                }
                Value native_res = callee_value.as.native(count, args);
                AST_GC_POP_N(1 + pushed_args);
                return EVAL_RESULT(native_res);
            }

            if (callee_value.type == VAL_FUNCTION) {
                if (callee_value.as.function == NULL || callee_value.as.function->proc == NULL) {
                    firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr), "attempted to call a null function"); firefly_end();
                    AST_GC_POP();
                    return EVAL_RESULT(val_nil());
                }
                ProcStmt* func = AS_FUNCTION(callee_value);
                int required = func->required_count;
                if (expr->as.call.arg_count < required || expr->as.call.arg_count > func->param_count) {
                    firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr), "expected %d to %d arguments but got %d",
                            required, func->param_count, expr->as.call.arg_count);
                    AST_GC_POP();
                    return EVAL_RESULT(val_nil());
                }

                // Pre-evaluate all provided arguments
                Value* eval_args = NULL;
                int pushed_args = 0;
                if (func->param_count > 0) {
                    eval_args = SAGE_ALLOC(sizeof(Value) * func->param_count);
                    for (int i = 0; i < expr->as.call.arg_count; i++) {
                        ExecResult arg_result = eval_expr(expr->as.call.args[i], env);
                        if (arg_result.is_throwing) { 
                            free(eval_args); 
                            AST_GC_POP_N(1 + pushed_args); 
                            return arg_result; 
                        }
                        eval_args[i] = arg_result.value;
                        AST_GC_PUSH(eval_args[i]);
                        pushed_args++;
                    }
                    // Fill in defaults for missing arguments
                    for (int i = expr->as.call.arg_count; i < func->param_count; i++) {
                        if (func->defaults && func->defaults[i]) {
                            ExecResult def_result = eval_expr(func->defaults[i], env);
                            if (def_result.is_throwing) { 
                                free(eval_args); 
                                AST_GC_POP_N(1 + pushed_args); 
                                return def_result; 
                            }
                            eval_args[i] = def_result.value;
                            AST_GC_PUSH(eval_args[i]);
                            pushed_args++;
                        } else {
                            eval_args[i] = val_nil();
                            AST_GC_PUSH(eval_args[i]);
                            pushed_args++;
                        }
                    }
                }

                if (callee_value.as.function->is_async) {
                    // Async call: spawn thread, return thread handle
                    Value spawn_args[1 + func->param_count];
                    spawn_args[0] = callee_value;
                    for (int i = 0; i < func->param_count; i++) {
                        spawn_args[i + 1] = eval_args[i];
                    }
                    free(eval_args);
                    // Use thread_spawn_native from stdlib.c (declared as extern)
                    extern Value thread_spawn_native(int argCount, Value* args);
                    Value handle = thread_spawn_native(1 + func->param_count, spawn_args);
                    AST_GC_POP_N(1 + pushed_args);
                    return EVAL_RESULT(handle);
                }

                Env* scope = env_create(callee_value.as.function->closure);
                AST_GC_PUSH_ENV(scope);
                
                // P13: Lightweight Firefly frame — store token pointer, no alloc
                FireflyLoc floc = firefly_loc_from_expr(expr);
                // Use the function name token start directly (lives in source text)
                char _ff_fname[64];
                int _ff_nl = func->name.length < 63 ? func->name.length : 63;
                memcpy(_ff_fname, func->name.start, _ff_nl);
                _ff_fname[_ff_nl] = '\0';
                firefly_push_frame(_ff_fname, floc);
                int has_firefly_frame = 1;
                
                // P13: Fast path for untyped params (no type checking overhead)
                if (func->param_types == NULL) {
                    for (int i = 0; i < func->param_count; i++) {
                        env_define_const(scope, func->params[i].start,
                                        func->params[i].length, eval_args[i]);
                    }
                } else {
                    for (int i = 0; i < func->param_count; i++) {
                        Token paramName = func->params[i];
                        if (func->param_types[i]) {
                            const char* err_expected = NULL;
                            if (!sage_typecheck(eval_args[i], func->param_types[i], &err_expected)) {
                                firefly_type_error(expr, err_expected, sage_typeof_str(eval_args[i]),
                                                  "parameter '%.*s' of '%.*s' expects %s",
                                                  paramName.length, paramName.start,
                                                  func->name.length, func->name.start,
                                                  err_expected);
                                free(eval_args);
                                if (has_firefly_frame) firefly_pop_frame();
                                AST_GC_POP_ENV();
                                AST_GC_POP_N(1 + pushed_args);
                                return EVAL_RESULT(val_nil());
                            }
                        }
                        env_define_const(scope, paramName.start, paramName.length, eval_args[i]);
                    }
                }

                // JIT: Profile this call and check if we should compile.
                int func_id = -1;
                if (g_jit && g_jit->enabled) {
                    func_id = (int)((uintptr_t)func % 100000);
                    jit_record_call(g_jit, func_id, func->param_count, eval_args);

                    JitProfile* profile = jit_get_profile(g_jit, func_id);
                    if (profile && jit_should_compile(g_jit, func_id)) {
                        JitNativeFn native = jit_compile_function(g_jit, func, scope);
                        if (native) {
                            profile->jit_compiled = 1;
                            profile->native_code = (void*)(uintptr_t)native;
                        }
                    }
                }

                free(eval_args);

                ExecResult res = interpret(func->body, scope);
                AST_GC_POP_ENV();
                AST_GC_POP_N(1 + pushed_args);

                // JIT: Record return type for specialization
                if (g_jit && func_id >= 0 && !res.is_throwing) {
                    jit_record_return(g_jit, func_id, res.value);
                }

                if (res.is_throwing) {
                    if (has_firefly_frame) firefly_pop_frame();
                    return res;
                }
                
                // P4: Return type enforcement with Firefly
                if (func->return_type != NULL && !IS_NIL(res.value)) {
                    const char* err_expected = NULL;
                    if (!sage_typecheck(res.value, func->return_type, &err_expected)) {
                        firefly_type_error(expr, err_expected, sage_typeof_str(res.value),
                                          "function '%.*s' declared -> %s but returned %s",
                                          func->name.length, func->name.start,
                                          err_expected, sage_typeof_str(res.value));
                    }
                }
                if (has_firefly_frame) firefly_pop_frame();
                return EVAL_RESULT(res.value);
            }

            if (callee_value.type == VAL_GENERATOR) {
                GeneratorValue* template = callee_value.as.generator;
                if (expr->as.call.arg_count != template->param_count) {
                    firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr), "wrong number of arguments"); firefly_end();
                    AST_GC_POP();
                    return EVAL_RESULT(val_nil());
                }

                Env* gen_closure = env_create(template->closure);
                AST_GC_PUSH_ENV(gen_closure);
                int pushed_args = 0;
                if (template->param_count > 0 && template->params != NULL) {
                    Token* params = (Token*)template->params;
                    for (int i = 0; i < template->param_count; i++) {
                        ExecResult arg_result = eval_expr(expr->as.call.args[i], env);
                        if (arg_result.is_throwing) { 
                            AST_GC_POP_ENV(); 
                            AST_GC_POP_N(1 + pushed_args); 
                            return arg_result; 
                        }
                        AST_GC_PUSH(arg_result.value);
                        pushed_args++;
                        env_define_const(gen_closure, params[i].start, params[i].length, arg_result.value);
                    }
                }

                Value gen_res = val_generator(template->body, template->params,
                                                 template->param_count, gen_closure);
                AST_GC_POP_ENV();
                AST_GC_POP_N(1 + pushed_args);
                return EVAL_RESULT(gen_res);
            }

            if (callee_value.type == VAL_CLASS) {
                ClassValue* class_def = callee_value.as.class_val;
                InstanceValue* instance = instance_create(class_def);
                Value inst_val = val_instance(instance);
                AST_GC_PUSH(inst_val);

                Method* init_method = class_find_method(class_def, "init", 4);
                if (init_method) {
                    Stmt* init_node = (Stmt*)init_method->method_stmt;
                    ProcStmt* init_stmt = (init_node->type == STMT_ASYNC_PROC) ? &init_node->as.async_proc : &init_node->as.proc;
                    Env* def_env = class_def->defining_env;
                    Env* method_env = env_create(def_env ? def_env : env);
                    AST_GC_PUSH_ENV(method_env);
                    env_define(method_env, "self", 4, inst_val);
                    // Track class owning init for super resolution
                    ClassValue* init_owner = class_find_method_owner(class_def, "init", 4);
                    if (init_owner) env_define(method_env, "__class__", 9, val_class(init_owner));

                    int param_start = (init_stmt->param_count > 0 &&
                                      strncmp(init_stmt->params[0].start, "self", 4) == 0) ? 1 : 0;

                    int pushed_args = 0;
                    for (int i = param_start; i < init_stmt->param_count; i++) {
                        int arg_idx = i - param_start;
                        Value bind_val = val_nil();
                        if (arg_idx < expr->as.call.arg_count) {
                            // Provided argument
                            ExecResult arg_result = eval_expr(expr->as.call.args[arg_idx], env);
                            if (arg_result.is_throwing) {
                                AST_GC_POP_ENV();
                                AST_GC_POP_N(2 + pushed_args);
                                return arg_result;
                            }
                            bind_val = arg_result.value;
                        } else if (init_stmt->defaults && init_stmt->defaults[i]) {
                            // Default value
                            ExecResult def_res = eval_expr_public(init_stmt->defaults[i], env);
                            if (def_res.is_throwing) { AST_GC_POP_ENV(); return def_res; }
                            bind_val = def_res.value;
                        }
                        AST_GC_PUSH(bind_val);
                        pushed_args++;
                        env_define_const(method_env, init_stmt->params[i].start,
                                         init_stmt->params[i].length, bind_val);
                    }

                    ExecResult init_res = interpret(init_stmt->body, method_env);
                    AST_GC_POP_ENV();
                    AST_GC_POP_N(pushed_args);
                    if (init_res.is_throwing) { AST_GC_POP_N(2); return init_res; }
                } else {
                    // Auto-init for structs: look for __StructName_fields__ metadata
                    char meta_key[256];
                    snprintf(meta_key, sizeof(meta_key), "__%.*s_fields__",
                             class_def->name_len, class_def->name);
                    Value fields_val;
                    if (env_get(env, meta_key, (int)strlen(meta_key), &fields_val) &&
                        fields_val.type == VAL_ARRAY) {
                        ArrayValue* fields = fields_val.as.array;
                        int pushed_args = 0;
                        for (int i = 0; i < fields->count && i < expr->as.call.arg_count; i++) {
                            ExecResult arg_result = eval_expr(expr->as.call.args[i], env);
                            if (arg_result.is_throwing) { 
                                AST_GC_POP_N(2 + pushed_args); 
                                return arg_result; 
                            }
                            AST_GC_PUSH(arg_result.value);
                            pushed_args++;
                            if (fields->elements[i].type == VAL_STRING) {
                                char* field_name = AS_STRING(fields->elements[i]);
                                instance_set_field(instance, field_name, (int)strlen(field_name), arg_result.value);
                            }
                        }
                        AST_GC_POP_N(pushed_args);
                    }
                }

                AST_GC_POP_N(2); // inst_val and callee_value
                return EVAL_RESULT(inst_val);
            }

            // Debug: show what was attempted to be called
            if (expr->as.call.callee && expr->as.call.callee->type == EXPR_VARIABLE) {
                { firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr),
                        "'%.*s' is not callable (%s)",
                        expr->as.call.callee->as.variable.name.length,
                        expr->as.call.callee->as.variable.name.start,
                        sage_typeof_str(callee_value));
               firefly_explain("'%.*s' is a %s, not a function.",
                        expr->as.call.callee->as.variable.name.length,
                        expr->as.call.callee->as.variable.name.start,
                        sage_typeof_str(callee_value));
               firefly_end(); }
            } else if (expr->as.call.callee && expr->as.call.callee->type == EXPR_GET) {
                { firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr),
                        "'%.*s' is not callable (%s)",
                        expr->as.call.callee->as.get.property.length,
                        expr->as.call.callee->as.get.property.start,
                        sage_typeof_str(callee_value));
               firefly_end(); }
            } else {
                { firefly_report(FIREFLY_ERROR, firefly_loc_from_expr(expr), "value is not callable (%s)", sage_typeof_str(callee_value)); firefly_end(); }
            }
            AST_GC_POP();
            return EVAL_RESULT(val_nil());
        }

        // Phase 17: comptime expression — in interpreter, just evaluate normally
        case EXPR_INTERP: {
            // Runtime string interpolation: "Hello, {name}! {n*2}"
            // Scan the template, evaluate each {expr}, concatenate parts
            const char* tmpl = expr->as.interp.template_str;
            int  tmpl_len    = expr->as.interp.template_len;
            char result_buf[65536];
            int  rlen = 0;

            int i = 0;
            while (i < tmpl_len) {
                if (tmpl[i] == '{'  && (i == 0 || tmpl[i-1] != '\\')) {
                    i++;  // skip {
                    // Collect expression source until matching }
                    char expr_src[4096];
                    int  elen = 0;
                    int  depth = 1;
                    while (i < tmpl_len && depth > 0) {
                        if (tmpl[i] == '{') depth++;
                        else if (tmpl[i] == '}') { depth--; if (depth == 0) break; }
                        if (elen < (int)sizeof(expr_src)-1)
                            expr_src[elen++] = tmpl[i];
                        i++;
                    }
                    expr_src[elen] = '\0';
                    i++;  // skip }

                    if (elen > 0) {
                        // Parse and evaluate the inner expression
                        char* snippet = malloc(elen + 2);

                        memcpy(snippet, expr_src, elen);
                        snippet[elen]   = '\n';
                        snippet[elen+1] = '\0';

                        // Parse using a completely fresh parse context
                        extern LexerState  lexer_get_state(void);
                        extern void        lexer_set_state(LexerState);
                        extern void        parser_set_state(ParserState);
                        extern ParserState parser_get_state(void);
                        extern void        init_lexer(const char*, const char*);
                        extern void        parser_init(void);
                        extern Expr*       parse_expression_public(void);

                        LexerState  sl = lexer_get_state();
                        ParserState sp = parser_get_state();
                        init_lexer(snippet, "<interp>");
                        parser_init();
                        Expr* sub = parse_expression_public();
                        lexer_set_state(sl);
                        parser_set_state(sp);
                        // NOTE: snippet is freed AFTER eval_expr - token .start pointers reference it

                        Value val = val_nil();
                        int interp_failed = 0;
                        if (sub) {
                            ExecResult sr = eval_expr(sub, env);
                            if (!sr.is_throwing) {
                                val = sr.value;
                            } else {
                                interp_failed = 1;  // eval threw — keep original text
                            }
                        } else {
                            interp_failed = 1;
                        }
                        free(snippet);  // safe to free now that eval is done

                        // If evaluation failed, preserve the original {expr} text
                        if (interp_failed) {
                            if (rlen + elen + 2 < (int)sizeof(result_buf)-1) {
                                result_buf[rlen++] = '{';
                                memcpy(result_buf + rlen, expr_src, (size_t)elen);
                                rlen += elen;
                                result_buf[rlen++] = '}';
                            }
                            continue;
                        }

                        // Convert to string and append
                        char vbuf[4096] = {0};
                        if (IS_NUMERIC(val)) {
                            double d = NUMERIC_AS_DOUBLE(val);
                            if (d == (long long)d)
                                snprintf(vbuf, sizeof(vbuf), "%lld", (long long)d);
                            else
                                snprintf(vbuf, sizeof(vbuf), "%g", d);
                        } else if (IS_STRING(val)) {
                            snprintf(vbuf, sizeof(vbuf), "%s", AS_STRING(val));
                        } else if (IS_BOOL(val)) {
                            snprintf(vbuf, sizeof(vbuf), "%s", AS_BOOL(val) ? "true" : "false");
                        } else if (IS_NIL(val)) {
                            snprintf(vbuf, sizeof(vbuf), "nil");
                        } else {
                            // Fallback: use print_value to a string
                            snprintf(vbuf, sizeof(vbuf), "<value>");
                        }
                        int vlen = (int)strlen(vbuf);
                        if (rlen + vlen < (int)sizeof(result_buf)-1) {
                            memcpy(result_buf + rlen, vbuf, vlen);
                            rlen += vlen;
                        }
                    }
                } else if (tmpl[i] == '\\' && i+1 < tmpl_len) {
                    // Handle escape sequences
                    char esc = tmpl[i+1];
                    char out = esc;
                    if      (esc == 'n')  out = '\n';
                    else if (esc == 't')  out = '\t';
                    else if (esc == 'r')  out = '\r';
                    else if (esc == '{')  out = '{';
                    else if (esc == '}')  out = '}';
                    else if (esc == '\\') out = '\\';
                    if (rlen < (int)sizeof(result_buf)-1) result_buf[rlen++] = out;
                    i += 2;
                } else {
                    if (rlen < (int)sizeof(result_buf)-1) result_buf[rlen++] = tmpl[i];
                    i++;
                }
            }
            result_buf[rlen] = '\0';
            return EVAL_RESULT(val_string(result_buf));
        }

        case EXPR_COMPTIME:
            return eval_expr(expr->as.comptime.expression, env);

        // ── Phase 2: Option/Result operators ─────────────────────────────

        case EXPR_NULLCOAL: {
            // left ?? right — return left unless it's nil; then right
            ExecResult left = eval_expr(expr->as.nullcoal.left, env);
            if (left.is_throwing) return left;
            if (!IS_NIL(left.value)) return left;
            return eval_expr(expr->as.nullcoal.right, env);
        }

        case EXPR_FORCE_UNWRAP: {
            // expr! — panics if nil, unwraps Some/Ok
            ExecResult res = eval_expr(expr->as.unwrap.operand, env);
            if (res.is_throwing) return res;
            Value v = res.value;
            if (IS_NIL(v)) {
                return EVAL_EXCEPTION(val_exception("Force-unwrap of nil value (!)"));
            }
            // Unwrap Some(x) or Ok(x) tagged dicts
            if (IS_DICT(v)) {
                Value tag = dict_get(&v, "__type");
                if (IS_STRING(tag)) {
                    const char* t = AS_STRING(tag);
                    if (strcmp(t, "option.some") == 0 || strcmp(t, "result.ok") == 0) {
                        return EVAL_RESULT(dict_get(&v, "__val"));
                    }
                    if (strcmp(t, "result.err") == 0) {
                        return EVAL_EXCEPTION(val_exception("Force-unwrap of Err value (!)"));
                    }
                }
            }
            return res;
        }

        case EXPR_PROPAGATE: {
            // expr? — if nil or Err, return early from enclosing proc
            ExecResult res = eval_expr(expr->as.unwrap.operand, env);
            if (res.is_throwing) return res;
            Value v = res.value;
            if (IS_NIL(v)) {
                // Return None (nil) from the enclosing proc
                ExecResult early = {val_nil(), 1, 0, 0, 0, val_nil(), 0, NULL};
                early.value = val_nil();
                early.is_returning = 1;
                return early;
            }
            if (IS_DICT(v)) {
                Value tag = dict_get(&v, "__type");
                if (IS_STRING(tag)) {
                    const char* t = AS_STRING(tag);
                    if (strcmp(t, "option.some") == 0) {
                        // Unwrap: Some(x)? => x
                        return EVAL_RESULT(dict_get(&v, "__val"));
                    }
                    if (strcmp(t, "result.ok") == 0) {
                        // Unwrap: Ok(x)? => x
                        return EVAL_RESULT(dict_get(&v, "__val"));
                    }
                    if (strcmp(t, "result.err") == 0) {
                        // Propagate: Err(e)? => return Err(e) from proc
                        ExecResult early = {val_nil(), 1, 0, 0, 0, val_nil(), 0, NULL};
                        early.value = v;
                        early.is_returning = 1;
                        return early;
                    }
                }
            }
            return res;
        }

        case EXPR_OPTCHAIN: {
            // expr?.member — nil-safe member access
            ExecResult obj_res = eval_expr(expr->as.optchain.object, env);
            if (obj_res.is_throwing) return obj_res;
            Value obj = obj_res.value;
            if (IS_NIL(obj)) return EVAL_RESULT(val_nil());
            // Delegate to normal GET logic by building a get expression on-the-fly
            GetExpr get;
            get.object = expr->as.optchain.object;
            get.property = expr->as.optchain.member;
            // Replicate GET behaviour inline
            if (IS_DICT(obj)) {
                Value got = dict_get_len(&obj, expr->as.optchain.member.start,
                                              expr->as.optchain.member.length);
                return EVAL_RESULT(got);
            }
            return EVAL_RESULT(val_nil());
        }

        case EXPR_RANGE: {
            // lo..hi or lo..=hi — produces a range dict for use in match/for
            ExecResult lo_res = eval_expr(expr->as.range.low, env);
            if (lo_res.is_throwing) return lo_res;
            ExecResult hi_res = eval_expr(expr->as.range.high, env);
            if (hi_res.is_throwing) return hi_res;
            gc_pin();
            Value d = val_dict();
            dict_set(&d, "__type",     val_string("range"));
            dict_set(&d, "low",        lo_res.value);
            dict_set(&d, "high",       hi_res.value);
            dict_set(&d, "inclusive",  val_bool(expr->as.range.inclusive));
            gc_unpin();
            return EVAL_RESULT(d);
        }

        default:
            return EVAL_RESULT(val_nil());
    }
}

ExecResult interpret(Stmt* stmt, Env* env) {
    if (++g_recursion_depth > MAX_RECURSION_DEPTH) {
        g_recursion_depth--;
        fprintf(stderr, "error[E070]: maximum recursion depth exceeded (%d)\n", MAX_RECURSION_DEPTH);
        return EVAL_EXCEPTION(val_exception("Maximum recursion depth exceeded"));
    }

    ThreadState* ts = gc_get_thread_state();
    EnvRootNode root_node;
    root_node.env = env;
    if (ts) {
        root_node.next = ts->gc_root_stack;
        ts->gc_root_stack = &root_node;
    } else {
        root_node.next = g_gc_root_stack;
        g_gc_root_stack = &root_node;
    }

    ExecResult result = interpret_inner(stmt, env);

    if (ts) {
        ts->gc_root_stack = root_node.next;
    } else {
        g_gc_root_stack = root_node.next;
    }

    g_recursion_depth--;
    return result;
}
static ExecResult interpret_inner(Stmt* stmt, Env* env) {
    // Phase 2: Consume gas for each statement
    if (!consume_gas(10)) return gas_error();

    // Thread-safe first-call detection: only set g_global_env once
    static volatile int first_call = 1;
    if (first_call && stmt != NULL) {
        // Benign race: multiple threads may set g_global_env to their env,
        // but only the main thread's initial call matters.
        g_global_env = env;
        first_call = 0;
    }
    if (!stmt) return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };

    if (g_generator_resume_target != NULL && stmt != g_generator_resume_target &&
        !stmt_contains_target(stmt, g_generator_resume_target)) {
        return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
    }

    if (stmt == g_generator_resume_target) {
        g_generator_resume_target = NULL;
    }

    switch (stmt->type) {
        case STMT_PRINT: {
            ExecResult result = eval_expr(stmt->as.print.expression, env);
            if (result.is_throwing) return result;
            // __str__ hook: if instance has __str__ method, call it for printing
            if (result.value.type == VAL_INSTANCE && result.value.as.instance->class_def) {
                Method* str_method = class_find_method(result.value.as.instance->class_def, "__str__", 7);
                if (str_method) {
                    AST_GC_PUSH(result.value);
                    Stmt* method_node = (Stmt*)str_method->method_stmt;
                    ProcStmt* str_stmt = (method_node->type == STMT_ASYNC_PROC) ? &method_node->as.async_proc : &method_node->as.proc;
                    Env* def_env = result.value.as.instance->class_def->defining_env;
                    Env* str_env = env_create(def_env ? def_env : env);
                    AST_GC_PUSH_ENV(str_env);
                    env_define(str_env, "self", 4, result.value);
                    ExecResult str_res = interpret(str_stmt->body, str_env);
                    AST_GC_POP_ENV();
                    if (!str_res.is_throwing && str_res.value.type == VAL_STRING) {
                        printf("%s\n", AS_STRING(str_res.value));
                    } else {
                        print_value(result.value);
                        printf("\n");
                    }
                    AST_GC_POP();
                    return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
                }
            }
            print_value(result.value);
            printf("\n");
            return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
        }

        case STMT_LET: {
            Value val = val_nil();
            if (stmt->as.let.initializer != NULL) {
                ExecResult result = eval_expr(stmt->as.let.initializer, env);
                if (result.is_throwing) return result;
                if (result.is_returning) return result;   // ? propagation early return
                val = result.value;
            }
            if (stmt->as.let.type_ann != NULL) {
                const char* err_expected = NULL;
                if (!sage_typecheck(val, stmt->as.let.type_ann, &err_expected)) {
                    // P9: Firefly type annotation error
                    FireflyLoc loc = {0};
                    loc.filename = stmt->as.let.name.filename;
                    loc.line = stmt->as.let.name.line;
                    loc.column = stmt->as.let.name.column;
                    loc.line_start = stmt->as.let.name.line_start;
                    loc.span = stmt->as.let.name.length;
                    firefly_set_code("E013");
                    firefly_report(FIREFLY_ERROR, loc,
                                  "cannot assign %s to variable '%.*s' (declared as %s)",
                                  sage_typeof_str(val), stmt->as.let.name.length,
                                  stmt->as.let.name.start, err_expected);
                    firefly_explain("The initializer evaluates to %s, but the type annotation requires %s.",
                                sage_typeof_str(val), err_expected);
                    firefly_advice("Change the initializer, or change the type to match.");
                    firefly_end();
                    return EVAL_RESULT(val_nil());
                }
            }
            Token t = stmt->as.let.name;
            // P2: Struct value semantics — copy on assignment
            if (IS_INSTANCE(val) && val.as.instance->class_def->is_struct) {
                val = instance_copy(val);
            }
            // P4: Mutable (var/type-first) vs immutable (let/const)
            if (stmt->as.let.is_mutable) {
                env_define(env, t.start, t.length, val);
            } else {
                env_define_const(env, t.start, t.length, val);
            }
            return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
        }

        case STMT_EXPRESSION: {
            ExecResult result = eval_expr(stmt->as.expression, env);
            if (result.is_throwing) return result;
            if (result.is_returning) return result;       // ? propagation early return
            // Return actual value (used by REPL to display expression results)
            return (ExecResult){ result.value, 0, 0, 0, 0, val_nil(), 0, NULL };
        }

        case STMT_BLOCK: {
            Stmt* current = stmt->as.block.statements;
            // Collect deferred statements (LIFO order)
            Stmt* deferred[64];
            int defer_count = 0;
            ExecResult block_result = { val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };

            while (current != NULL) {
                if (current->type == STMT_DEFER) {
                    // Collect defer — don't execute yet
                    if (defer_count < 64) {
                        deferred[defer_count++] = current->as.defer.statement;
                    } else {
                        fprintf(stderr, "Warning: Maximum defer count (64) exceeded; statement dropped.\n");
                    }
                    current = current->next;
                    continue;
                }
                ExecResult res = interpret(current, env);
                if (res.is_yielding) {
                    if (res.next_stmt == NULL) {
                        res.next_stmt = current->next;
                    }
                    // Run deferred before yielding
                    for (int di = defer_count - 1; di >= 0; di--) {
                        interpret(deferred[di], env);
                    }
                    return res;
                }
                if (res.is_returning || res.is_breaking || res.is_continuing || res.is_throwing) {
                    block_result = res;
                    break;
                }
                current = current->next;
            }
            // Run deferred statements in LIFO order
            for (int di = defer_count - 1; di >= 0; di--) {
                interpret(deferred[di], env);
            }
            return block_result;
        }

        case STMT_SPAWN: {
            // spawn: <block> — runs the block body in a new thread
            // Creates a closure proc from the block and spawns it
            Stmt* body = stmt->as.spawn_stmt.body;
            if (!body) return (ExecResult){ val_nil(), 0,0,0,0, val_nil(), 0, NULL };

            // Build a synthetic ProcStmt wrapping the body, capturing current env
            // Must be HEAP-allocated — the thread outlives this stack frame
            ProcStmt* proc_wrapper = SAGE_ALLOC(sizeof(ProcStmt));
            memset(proc_wrapper, 0, sizeof(ProcStmt));
            proc_wrapper->param_count   = 0;
            proc_wrapper->required_count = 0;
            proc_wrapper->body          = body;

            FunctionValue* fv = gc_alloc(VAL_FUNCTION, sizeof(FunctionValue));
            fv->proc        = proc_wrapper;
            fv->closure     = env;
            fv->is_async    = 0;
            fv->is_vm       = 0;
            fv->vm_function = NULL;

            Value func_val;
            func_val.type        = VAL_FUNCTION;
            func_val.as.function = fv;

            // Spawn using thread_spawn_native
            extern Value thread_spawn_native(int argCount, Value* args);
            Value handle = thread_spawn_native(1, &func_val);
            (void)handle;  // fire-and-forget unless user captures it
            return (ExecResult){ handle, 0,0,0,0, val_nil(), 0, NULL };
        }

        case STMT_EXTERN: {
            // Phase 4: extern proc — creates a Sage native that calls a C function via libffi
            ExternStmt* ext = &stmt->as.extern_proc;
            char* lib_name = ext->lib_name;
            char func_name_buf[256];
            snprintf(func_name_buf, sizeof(func_name_buf), "%.*s",
                     (int)ext->name.length, ext->name.start);

            // LilyKnight: check FFI permission before loading library
            if (lib_name && lk_current_sandbox) {
                LKViolation _lkffi = lk_check_ffi(lk_current_sandbox, lib_name);
                if (_lkffi != LK_OK) {
                    lk_log_violation(lk_current_sandbox, _lkffi, lib_name);
                    env_define_const(env, ext->name.start, ext->name.length, val_nil());
                    return (ExecResult){ val_nil(), 0,0,0,0, val_nil(), 0, NULL };
                }
            }

            // Find or open the shared library
            void* lib_handle = NULL;
            if (lib_name) {
                // Try common .so suffixes
                const char* suffixes[] = { "", ".so", ".so.0", ".dylib", NULL };
                char lib_path[512];
                for (int si = 0; suffixes[si] && !lib_handle; si++) {
                    snprintf(lib_path, sizeof(lib_path), "%s%s", lib_name, suffixes[si]);
                    lib_handle = dlopen(lib_path, RTLD_LAZY | RTLD_GLOBAL);
                    if (!lib_handle) {
                        // Try without 'lib' prefix
                        if (strncmp(lib_name, "lib", 3) == 0) {
                            snprintf(lib_path, sizeof(lib_path), "%s%s", lib_name+3, suffixes[si]);
                            lib_handle = dlopen(lib_path, RTLD_LAZY | RTLD_GLOBAL);
                        }
                    }
                }
                if (!lib_handle) {
                    // Try RTLD_DEFAULT (already loaded)
                    dlerror();
                }
            }
            if (!lib_handle) lib_handle = RTLD_DEFAULT;

            dlerror();
            void* sym = dlsym(lib_handle, func_name_buf);
            if (!sym) {
                // Not critical — binding might be for documentation only
                // Define as nil so import doesn't fail
                env_define_const(env, ext->name.start, ext->name.length, val_nil());
                return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
            }

            // Capture everything we need in a heap-allocated closure context
            // We create a Sage native function that calls the C function via libffi
            typedef struct {
                void* sym;
                char** param_types;
                int param_count;
                char* ret_type;
            } ExternCtx;
            ExternCtx* ctx = SAGE_ALLOC(sizeof(ExternCtx));
            ctx->sym = sym;
            ctx->param_count = ext->param_count;
            ctx->ret_type = ext->return_type_name ? SAGE_STRDUP(ext->return_type_name) : SAGE_STRDUP("void");
            ctx->param_types = SAGE_ALLOC(sizeof(char*) * (ctx->param_count + 1));
            for (int i = 0; i < ctx->param_count; i++) {
                ctx->param_types[i] = ext->param_type_names[i]
                    ? SAGE_STRDUP(ext->param_type_names[i]) : SAGE_STRDUP("any");
            }

            // We can't easily capture ctx in a static function without globals.
            // Use a thread-local slot trick: store ctx in a global array indexed
            // by a unique native ID, then dispatch via a switch-like mechanism.
            // For now: store the sym pointer directly in the Value's CLib slot
            // and use a generic ffi dispatcher native.
            // Simpler: just register the sym address as a val_clib and let
            // ffi_call() use it. The binding file generator will emit proper wrappers.

            // Actually: for Phase 4 we just expose the handle as a CLib value
            // The user can call it via ffi.call(). Full libffi dispatch in Phase 5.
            Value clib_val = val_clib(lib_handle, lib_name ? lib_name : "unknown");
            // Expose the raw sym as a native number for now
            // TODO Phase 5: proper libffi dispatch closure
            (void)ctx;  // will use in Phase 5

            // Create a simple native wrapper that dispatches via ffi_call
            // by storing the sym pointer in a dict with __sym and __ret_type keys
            gc_pin();
            Value binding = val_dict();
            dict_set(&binding, "__sym_name", val_string(func_name_buf));
            dict_set(&binding, "__lib",      clib_val);
            dict_set(&binding, "__ret",      val_string(ctx->ret_type));
            dict_set(&binding, "__type",     val_string("extern.proc"));
            gc_unpin();

            env_define_const(env, ext->name.start, ext->name.length, binding);
            return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
        }

        case STMT_ANNOTATED_BLOCK: {
            // P7: Memory/safety annotated blocks with real semantics
            if (!stmt->as.annotated_block.statements) {
                return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
            }
            
            BlockAnnotation ann = stmt->as.annotated_block.annotation;
            
            switch (ann) {
                case BLOCK_ANNOT_MANUAL: {
                    // @manual: pause GC, allow explicit mem_alloc/mem_free
                    // User is responsible for freeing memory in this block
                    gc_disable();
                    ExecResult res = interpret(stmt->as.annotated_block.statements, env);
                    gc_enable();
                    return res;
                }
                case BLOCK_ANNOT_GC: {
                    // @gc: force GC-managed mode (useful inside @manual to re-enable GC)
                    gc_enable();
                    ExecResult res = interpret(stmt->as.annotated_block.statements, env);
                    return res;
                }
                case BLOCK_ANNOT_TRUSTED:
                case BLOCK_ANNOT_UNSAFE: {
                    // @trusted / @unsafe: execute without runtime safety checks
                    // For now: same as normal execution but with GC disabled for speed
                    gc_disable();
                    ExecResult res = interpret(stmt->as.annotated_block.statements, env);
                    gc_enable();
                    return res;
                }
                case BLOCK_ANNOT_OWNED: {
                    // @owned: single-owner move semantics (future: track ownership)
                    // For now: execute normally
                    ExecResult res = interpret(stmt->as.annotated_block.statements, env);
                    return res;
                }
                default:
                    return interpret(stmt->as.annotated_block.statements, env);
            }
        }

        case STMT_IF: {
            ExecResult cond_result = eval_expr(stmt->as.if_stmt.condition, env);
            if (cond_result.is_throwing) return cond_result;
            
            if (is_truthy(cond_result.value)) {
                return interpret(stmt->as.if_stmt.then_branch, env);
            } else if (stmt->as.if_stmt.else_branch != NULL) {
                return interpret(stmt->as.if_stmt.else_branch, env);
            }
            return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
        }

        case STMT_WHILE: {
            int iterations = 0;
            while (1) {
                if (++iterations > MAX_LOOP_ITERATIONS) {
                    fprintf(stderr, "error[E071]: while loop exceeded maximum iterations (%d)\n", MAX_LOOP_ITERATIONS);
                    return EVAL_EXCEPTION(val_exception("While loop exceeded maximum iterations"));
                }
                ExecResult cond_result = eval_expr(stmt->as.while_stmt.condition, env);
                if (cond_result.is_throwing) return cond_result;
                if (!is_truthy(cond_result.value)) break;

                ExecResult res = interpret(stmt->as.while_stmt.body, env);
                if (res.is_returning || res.is_throwing) return res;

                if (res.is_yielding) {
                    if (res.next_stmt == NULL) {
                        res.next_stmt = stmt;
                    }
                    return res;
                }

                if (res.is_breaking) break;
                if (res.is_continuing) continue;
            }
            return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
        }

        case STMT_FOR: {
            ExecResult iter_result = eval_expr(stmt->as.for_stmt.iterable, env);
            if (iter_result.is_throwing) return iter_result;
            Value iterable = iter_result.value;

            // Range iteration: for i in 0..10  (dict with __type:"range")
            if (IS_DICT(iterable)) {
                Value tag = dict_get(&iterable, "__type");
                if (IS_STRING(tag) && strcmp(AS_STRING(tag), "range") == 0) {
                    Value lo_v   = dict_get(&iterable, "low");
                    Value hi_v   = dict_get(&iterable, "high");
                    Value inc_v  = dict_get(&iterable, "inclusive");
                    // P1: Use int loop counter when both bounds are int
                    int both_int = IS_INT(lo_v) && IS_INT(hi_v);
                    double lo    = IS_NUMERIC(lo_v) ? NUMERIC_AS_DOUBLE(lo_v) : 0;
                    double hi    = IS_NUMERIC(hi_v) ? NUMERIC_AS_DOUBLE(hi_v) : 0;
                    int inclusive = IS_BOOL(inc_v) && AS_BOOL(inc_v);
                    Token var    = stmt->as.for_stmt.variable;
                    Env* loop_env = env_create(env);
                    AST_GC_PUSH_ENV(loop_env);
                    env_define_const(loop_env, var.start, var.length, both_int ? val_int((int64_t)lo) : val_number(lo));
                    EnvNode* vslot = loop_env->head;
                    if (both_int) {
                        int64_t ilo = AS_INT(lo_v), ihi = AS_INT(hi_v);
                        for (int64_t i = ilo; inclusive ? i <= ihi : i < ihi; i++) {
                            GC_WRITE_BARRIER(vslot->value);
                            vslot->value = val_int(i);
                            if (loop_env->map) {
                                envmap_set(loop_env->map, var.start, var.length, val_int(i));
                            }
                            ExecResult res = interpret(stmt->as.for_stmt.body, loop_env);
                            if (res.is_returning || res.is_throwing) {
                                AST_GC_POP_ENV();
                                return res;
                            }
                            if (res.is_breaking) break;
                        }
                    } else {
                    double i = lo;
                    while (inclusive ? i <= hi : i < hi) {
                        GC_WRITE_BARRIER(vslot->value);
                        vslot->value = val_number(i);
                        if (loop_env->map) {
                            envmap_set(loop_env->map, var.start, var.length, val_number(i));
                        }
                        ExecResult res = interpret(stmt->as.for_stmt.body, loop_env);
                        if (res.is_returning || res.is_throwing) {
                            AST_GC_POP_ENV();
                            return res;
                        }
                        if (res.is_breaking) break;
                        i += 1.0;
                    }
                    }
                    AST_GC_POP_ENV();
                    return (ExecResult){ val_nil(), 0,0,0,0, val_nil(), 0, NULL };
                }
            }

            if (iterable.type != VAL_ARRAY) {
                // Try to convert dict/string to array for iteration
                if (IS_STRING(iterable)) {
                    // Iterate over characters
                    const char* s = AS_STRING(iterable);
                    Token var = stmt->as.for_stmt.variable;
                    Env* loop_env = env_create(env);
                    AST_GC_PUSH_ENV(loop_env);
                    char ch[2] = {0, 0};
                    env_define_const(loop_env, var.start, var.length, val_string(ch));
                    EnvNode* vslot = loop_env->head;
                    while (*s) {
                        ch[0] = *s++;
                        GC_WRITE_BARRIER(vslot->value);
                        vslot->value = val_string(ch);
                        ExecResult res = interpret(stmt->as.for_stmt.body, loop_env);
                        if (res.is_returning || res.is_throwing) { AST_GC_POP_ENV(); return res; }
                        if (res.is_breaking) break;
                    }
                    AST_GC_POP_ENV();
                    return (ExecResult){ val_nil(), 0,0,0,0, val_nil(), 0, NULL };
                }
                fprintf(stderr, "error: for loop iterable must be an array or range\n");
                return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
            }

            AST_GC_PUSH(iterable);
            Env* loop_env = env_create(env);
            AST_GC_PUSH_ENV(loop_env);
            Token var = stmt->as.for_stmt.variable;

            ArrayValue* arr = iterable.as.array;
            // Define loop variable once, then directly update the slot
            // on subsequent iterations (avoids linked-list search per iteration)
            if (arr->count > 0) {
                env_define_const(loop_env, var.start, var.length, arr->elements[0]);
                // Cache the node pointer for direct slot update
                EnvNode* var_slot = loop_env->head; // just-inserted node
                for (int i = 0; i < arr->count; i++) {
                    // Direct slot write (bypasses env_define search)
                    if (i > 0) {
                        GC_WRITE_BARRIER(var_slot->value);
                        var_slot->value = arr->elements[i];
                    }

                    ExecResult res = interpret(stmt->as.for_stmt.body, loop_env);
                    if (res.is_returning || res.is_throwing) {
                        AST_GC_POP_ENV();
                        AST_GC_POP();
                        return res;
                    }

                    if (res.is_yielding) {
                        if (res.next_stmt == NULL) {
                            res.next_stmt = stmt;
                        }
                        AST_GC_POP_ENV();
                        AST_GC_POP();
                        return res;
                    }

                    if (res.is_breaking) break;
                    if (res.is_continuing) continue;
                }
            }
            AST_GC_POP_ENV();
            AST_GC_POP();
            return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
        }

        case STMT_BREAK:
            return (ExecResult){ val_nil(), 0, 1, 0, 0, val_nil(), 0, NULL };

        case STMT_CONTINUE:
            return (ExecResult){ val_nil(), 0, 0, 1, 0, val_nil(), 0, NULL };

        // PHASE 8: Modified STMT_PROC to add functions to environment
        case STMT_PROC: {
            Token name = stmt->as.proc.name;
            int is_generator = contains_yield(stmt->as.proc.body);

            Value func_val;
            if (is_generator) {
                func_val = val_generator(stmt->as.proc.body, stmt->as.proc.params,
                                        stmt->as.proc.param_count, env);
            } else {
                func_val = val_function(&stmt->as.proc, env);
            }

            env_define_const(env, name.start, name.length, func_val);
            return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
        }

        case STMT_ASYNC_PROC: {
            Token name = stmt->as.async_proc.name;
            Value func_val = val_function(&stmt->as.async_proc, env);
            func_val.as.function->is_async = 1;
            env_define_const(env, name.start, name.length, func_val);
            return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
        }

        case STMT_CLASS: {
            ClassValue* parent = NULL;
            if (stmt->as.class_stmt.has_parent) {
                Value parent_val;
                Token parent_name = stmt->as.class_stmt.parent;
                if (env_get(env, parent_name.start, parent_name.length, &parent_val)) {
                    if (parent_val.type == VAL_CLASS) {
                        parent = parent_val.as.class_val;
                    } else {
                        fprintf(stderr, "error[E072]: parent must be a class\n");
                        return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
                    }
                } else {
                    fprintf(stderr, "error[E073]: undefined parent class\n");
                    return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
                }
            }
            
            Token name = stmt->as.class_stmt.name;
            gc_pin();
            ClassValue* class_val = class_create(name.start, name.length, parent);
            class_val->defining_env = env; // Capture defining environment for method scoping

            Stmt* method = stmt->as.class_stmt.methods;
            while (method != NULL) {
                Token method_name;
                if (method->type == STMT_PROC) {
                    method_name = method->as.proc.name;
                } else if (method->type == STMT_ASYNC_PROC) {
                    method_name = method->as.async_proc.name;
                } else {
                    method = method->next;
                    continue;
                }
                class_add_method(class_val, method_name.start, method_name.length, (void*)method);
                method = method->next;
            }
            
            Value class_value = val_class(class_val);
            env_define_const(env, name.start, name.length, class_value);
            gc_unpin();
            
            return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
        }

        // Phase 1.7: Struct — lightweight value type, auto init/eq/str
        case STMT_STRUCT: {
            Token name = stmt->as.struct_stmt.name;
            gc_pin();
            ClassValue* class_val = class_create(name.start, name.length, NULL);
            class_val->defining_env = env;
            class_val->is_struct = 1;  // P2: mark as value type

            // Store field metadata on the class for auto-init/eq/str
            // We use a special dict stored in the env under __struct_fields__
            Value fields_arr = val_array();
            for (int i = 0; i < stmt->as.struct_stmt.field_count; i++) {
                Token fn = stmt->as.struct_stmt.field_names[i];
                char* fname = SAGE_ALLOC((size_t)fn.length + 1);
                memcpy(fname, fn.start, (size_t)fn.length);
                fname[fn.length] = '\0';
                array_push(&fields_arr, val_string(fname));
                free(fname);
            }

            Value class_value = val_class(class_val);
            env_define_const(env, name.start, name.length, class_value);

            // Store field names for auto-init
            char meta_key[256];
            snprintf(meta_key, sizeof(meta_key), "__%.*s_fields__", name.length, name.start);
            env_define(env, meta_key, (int)strlen(meta_key), fields_arr);
            gc_unpin();

            return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
        }

        // Phase 1.7: Enum — tagged variant type
        // P3: ADT enum variants with associated data
        case STMT_ENUM: {
            Token name = stmt->as.enum_stmt.name;
            gc_pin();
            Value enum_dict = val_dict();

            char enum_name_buf[256];
            int enl = name.length < 255 ? name.length : 255;
            memcpy(enum_name_buf, name.start, enl);
            enum_name_buf[enl] = '\0';

            for (int i = 0; i < stmt->as.enum_stmt.variant_count; i++) {
                Token vn = stmt->as.enum_stmt.variant_names[i];
                char vname[256];
                int vnl = vn.length < 255 ? vn.length : 255;
                memcpy(vname, vn.start, vnl);
                vname[vnl] = '\0';

                int field_count = stmt->as.enum_stmt.variant_field_counts
                                  ? stmt->as.enum_stmt.variant_field_counts[i] : 0;

                if (field_count == 0) {
                    // Simple variant: Shape.Point => tagged dict with no data
                    Value tag = val_dict();
                    char type_str[512];
                    snprintf(type_str, sizeof(type_str), "%s.%s", enum_name_buf, vname);
                    dict_set(&tag, "__type", val_string(type_str));
                    dict_set(&tag, "__tag", val_int(i));
                    dict_set(&enum_dict, vname, tag);
                } else {
                    // ADT variant: Shape.Circle(r) => store metadata, actual construction
                    // happens when called via Shape.Circle(5.0)
                    // Store a dict with constructor info
                    Value ctor_info = val_dict();
                    char type_str[512];
                    snprintf(type_str, sizeof(type_str), "%s.%s", enum_name_buf, vname);
                    dict_set(&ctor_info, "__type", val_string("__enum_ctor__"));
                    dict_set(&ctor_info, "__variant_type", val_string(type_str));
                    dict_set(&ctor_info, "__tag", val_int(i));

                    // Store field names
                    Value field_names = val_array();
                    for (int fi = 0; fi < field_count; fi++) {
                        Token fn = stmt->as.enum_stmt.variant_fields[i][fi];
                        char fname[256];
                        int fnl = fn.length < 255 ? fn.length : 255;
                        memcpy(fname, fn.start, fnl);
                        fname[fnl] = '\0';
                        array_push(&field_names, val_string(fname));
                    }
                    dict_set(&ctor_info, "__fields", field_names);
                    dict_set(&enum_dict, vname, ctor_info);
                }
            }

            dict_set(&enum_dict, "__name__", val_string(enum_name_buf));
            env_define_const(env, name.start, name.length, enum_dict);
            gc_unpin();
            return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
        }

        // Phase 1.7: Trait — method signature contract (stored as metadata)
        case STMT_TRAIT: {
            Token name = stmt->as.trait_stmt.name;
            Value trait_dict = val_dict();

            // Store required method names
            Value method_names = val_array();
            Stmt* method = stmt->as.trait_stmt.methods;
            while (method != NULL) {
                if (method->type == STMT_PROC) {
                    Token mn = method->as.proc.name;
                    char* mname = SAGE_ALLOC((size_t)mn.length + 1);
                    memcpy(mname, mn.start, (size_t)mn.length);
                    mname[mn.length] = '\0';
                    array_push(&method_names, val_string(mname));
                    free(mname);
                }
                method = method->next;
            }
            dict_set(&trait_dict, "__methods__", method_names);

            char* tname = SAGE_ALLOC((size_t)name.length + 1);
            memcpy(tname, name.start, (size_t)name.length);
            tname[name.length] = '\0';
            dict_set(&trait_dict, "__name__", val_string(tname));
            free(tname);

            env_define_const(env, name.start, name.length, trait_dict);
            return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
        }

        // P3: impl block — attach methods to an existing type
        case STMT_IMPL: {
            Token target = stmt->as.impl_stmt.target;
            // Look up the class/struct value in the environment
            Value type_val;
            if (!env_get(env, target.start, target.length, &type_val) ||
                type_val.type != VAL_CLASS) {
                { FireflyLoc _loc = {0}; _loc.line = target.line; _loc.column = target.column; _loc.filename = target.filename; _loc.line_start = target.line_start; _loc.span = target.length; firefly_report(FIREFLY_ERROR, _loc, "cannot impl on '%.*s' — not a class or struct", target.length, target.start); firefly_end(); }
                return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
            }
            ClassValue* class_val = type_val.as.class_val;
            
            // Add each method to the class
            Stmt* method = stmt->as.impl_stmt.methods;
            while (method != NULL) {
                if (method->type == STMT_PROC) {
                    Token mname = method->as.proc.name;
                    class_add_method(class_val, mname.start, mname.length, method);
                }
                method = method->next;
            }
            return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
        }

        case STMT_RETURN: {
            Value val = val_nil();
            if (stmt->as.ret.value) {
                ExecResult result = eval_expr(stmt->as.ret.value, env);
                if (result.is_throwing) return result;
                val = result.value;
            }
            return (ExecResult){ val, 1, 0, 0, 0, val_nil(), 0, NULL };
        }

        case STMT_TRY: {
            ExecResult try_result = interpret(stmt->as.try_stmt.try_block, env);
            
            if (try_result.is_throwing) {
                AST_GC_PUSH(try_result.exception_value);
                for (int i = 0; i < stmt->as.try_stmt.catch_count; i++) {
                    CatchClause* catch_clause = stmt->as.try_stmt.catches[i];
                    Env* catch_env = env_create(env);
                    AST_GC_PUSH_ENV(catch_env);
                    Token var = catch_clause->exception_var;
                    
                    Value exc_msg;
                    if (try_result.exception_value.type == VAL_INSTANCE) {
                        // Instance exceptions: pass the instance directly
                        exc_msg = try_result.exception_value;
                    } else if (IS_EXCEPTION(try_result.exception_value)) {
                        exc_msg = val_string(try_result.exception_value.as.exception->message);
                    } else {
                        exc_msg = try_result.exception_value;
                    }
                    env_define_const(catch_env, var.start, var.length, exc_msg);
                    
                    ExecResult catch_result = interpret(catch_clause->body, catch_env);
                    AST_GC_POP_ENV();
                    if (!catch_result.is_throwing) {
                        try_result = catch_result;
                        break;
                    }
                    try_result = catch_result;
                }
                AST_GC_POP();
            }
            
            if (stmt->as.try_stmt.finally_block != NULL) {
                AST_GC_PUSH(try_result.value);
                AST_GC_PUSH(try_result.exception_value);
                ExecResult finally_result = interpret(stmt->as.try_stmt.finally_block, env);
                AST_GC_POP_N(2);
                // Finally control flow overrides try/catch (matches Python/Java semantics)
                if (finally_result.is_throwing) return finally_result;
                if (finally_result.is_returning) return finally_result;
                if (finally_result.is_breaking) return finally_result;
                if (finally_result.is_continuing) return finally_result;
            }

            return try_result;
        }

        case STMT_RAISE: {
            ExecResult exc_result = eval_expr(stmt->as.raise.exception, env);
            if (exc_result.is_throwing) return exc_result;
            
            Value exc_val = exc_result.value;
            if (IS_STRING(exc_val)) {
                exc_val = val_exception(AS_STRING(exc_val));
            } else if (IS_NUMERIC(exc_val)) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%.14g", NUMERIC_AS_DOUBLE(exc_val));
                exc_val = val_exception(buf);
            } else if (IS_BOOL(exc_val)) {
                exc_val = val_exception(AS_BOOL(exc_val) ? "true" : "false");
            } else if (IS_NIL(exc_val)) {
                exc_val = val_exception("nil");
            } else if (exc_val.type == VAL_INSTANCE) {
                // Allow raising class instances as exception values
                // The instance is preserved as-is for the catch clause
            } else if (!IS_EXCEPTION(exc_val)) {
                exc_val = val_exception("Unknown error");
            }
            return (ExecResult){ val_nil(), 0, 0, 0, 1, exc_val, 0, NULL };
        }

        // PHASE 7: Yield statement execution
        case STMT_YIELD: {
            Value yield_value = val_nil();
            if (stmt->as.yield_stmt.value != NULL) {
                ExecResult result = eval_expr(stmt->as.yield_stmt.value, env);
                if (result.is_throwing) return result;
                yield_value = result.value;
            }
            
            ExecResult result = {0};
            result.value = yield_value;
            result.is_yielding = 1;
            result.next_stmt = stmt->next;
            return result;
        }

        // PHASE 8: Import statement execution
        case STMT_IMPORT: {
            char* module_name = stmt->as.import.module_name;
            char** items = stmt->as.import.items;
            int item_count = stmt->as.import.item_count;
            char* alias = stmt->as.import.alias;
            int import_all_flag = stmt->as.import.import_all;
            
            // Handle different import types
            if (import_all_flag && !alias) {
                // import module_name (no alias)
                if (!import_all(env, module_name)) {
                    fprintf(stderr, "error[E074]: Failed to import module '%s'\n", module_name);
                    return (ExecResult){ val_nil(), 0, 0, 0, 1, val_exception("Import error"), 0, NULL };
                }
            } else if (import_all_flag && alias) {
                // import module_name as alias
                if (!import_as(env, module_name, alias)) {
                    fprintf(stderr, "error[E074]: Failed to import module '%s' as '%s'\n", module_name, alias);
                    return (ExecResult){ val_nil(), 0, 0, 0, 1, val_exception("Import error"), 0, NULL };
                }
            } else if (item_count == 1 && items[0] != NULL && strcmp(items[0], "*") == 0) {
                // from module_name import * (wildcard — import all exports into current scope)
                if (!import_wildcard(env, module_name)) {
                    fprintf(stderr, "error[E074]: Failed to wildcard-import module '%s'\n", module_name);
                    return (ExecResult){ val_nil(), 0, 0, 0, 1, val_exception("Import error"), 0, NULL };
                }
            } else {
                // from module_name import item1, item2, ...
                ImportItem* import_items = SAGE_ALLOC(sizeof(ImportItem) * item_count);
                for (int i = 0; i < item_count; i++) {
                    import_items[i].name = items[i];
                    import_items[i].alias = stmt->as.import.item_aliases[i];
                }

                if (!import_from(env, module_name, import_items, item_count)) {
                    fprintf(stderr, "error[E074]: Failed to import from module '%s'\n", module_name);
                    free(import_items);
                    return (ExecResult){ val_nil(), 0, 0, 0, 1, val_exception("Import error"), 0, NULL };
                }

                free(import_items);
            }

            
            return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
        }

        case STMT_MATCH: {
            ExecResult val_res = eval_expr(stmt->as.match_stmt.value, env);
            if (val_res.is_throwing) return val_res;
            Value match_val = val_res.value;
            AST_GC_PUSH(match_val);

            for (int i = 0; i < stmt->as.match_stmt.case_count; i++) {
                CaseClause* clause = stmt->as.match_stmt.cases[i];
                Expr* pat = clause->pattern;
                int matched = 0;
                Env* match_scope = env_create(env);  // scope for binding captures

                // ── Wildcard: _ ───────────────────────────────────────────
                if (pat->type == EXPR_VARIABLE
                        && pat->as.variable.name.length == 1
                        && pat->as.variable.name.start[0] == '_') {
                    matched = 1;
                }
                // ── ADT destructure: Some(x), Ok(v), Err(e), None ─────────
                else if (pat->type == EXPR_CALL
                        && pat->as.call.callee->type == EXPR_VARIABLE) {
                    Token fname = pat->as.call.callee->as.variable.name;
                    int is_some = (fname.length == 4 && strncmp(fname.start, "Some", 4) == 0);
                    int is_ok   = (fname.length == 2 && strncmp(fname.start, "Ok",   2) == 0);
                    int is_err  = (fname.length == 3 && strncmp(fname.start, "Err",  3) == 0);
                    int is_none = (fname.length == 4 && strncmp(fname.start, "None", 4) == 0);
                    if (is_some || is_ok || is_err || is_none) {
                        if (is_none) {
                            matched = IS_NIL(match_val);
                        } else {
                            const char* expected_tag =
                                is_some ? "option.some" :
                                is_ok   ? "result.ok"   :
                                          "result.err";
                            if (IS_DICT(match_val)) {
                                Value tag = dict_get(&match_val, "__type");
                                if (IS_STRING(tag) && strcmp(AS_STRING(tag), expected_tag) == 0) {
                                    matched = 1;
                                    // Bind inner value if pattern has arg
                                    if (pat->as.call.arg_count == 1
                                            && pat->as.call.args[0]->type == EXPR_VARIABLE) {
                                        Token bind = pat->as.call.args[0]->as.variable.name;
                                        Value inner = dict_get(&match_val, "__val");
                                        env_define(match_scope, bind.start, bind.length, inner);
                                    }
                                }
                            }
                        }
                    }
                }
                // ── P13: User-defined ADT destructure: Shape.Circle(r), Result.Ok(v) ──
                else if (pat->type == EXPR_CALL
                        && pat->as.call.callee->type == EXPR_GET) {
                    // Pattern is TypeName.Variant(bindings...)
                    // Match if match_val.__type == "TypeName.Variant"
                    Expr* get_expr = pat->as.call.callee;
                    Token prop = get_expr->as.get.property;
                    // Evaluate the object (enum type) to get its name
                    ExecResult obj_res = eval_expr(get_expr->as.get.object, env);
                    if (!obj_res.is_throwing && IS_DICT(obj_res.value)) {
                        Value enum_name_val = dict_get(&obj_res.value, "__name__");
                        if (IS_STRING(enum_name_val) && IS_DICT(match_val)) {
                            // Build expected type tag: "EnumName.VariantName"
                            char expected_type[256];
                            snprintf(expected_type, sizeof(expected_type), "%s.%.*s",
                                     AS_STRING(enum_name_val), prop.length, prop.start);
                            Value match_type = dict_get(&match_val, "__type");
                            if (IS_STRING(match_type) && strcmp(AS_STRING(match_type), expected_type) == 0) {
                                matched = 1;
                                // Bind destructured fields by name
                                // The pattern args are variable names that correspond to field names
                                Value fields_val = dict_get(&obj_res.value, (char*)prop.start);
                                // Get field names from the ctor info
                                char _vprop[128];
                                int _vpl = prop.length < 127 ? prop.length : 127;
                                memcpy(_vprop, prop.start, _vpl);
                                _vprop[_vpl] = '\0';
                                Value ctor_info = dict_get(&obj_res.value, _vprop);
                                Value field_names_arr = IS_DICT(ctor_info) ? dict_get(&ctor_info, "__fields") : val_nil();
                                
                                for (int a = 0; a < pat->as.call.arg_count; a++) {
                                    if (pat->as.call.args[a]->type == EXPR_VARIABLE) {
                                        Token bind = pat->as.call.args[a]->as.variable.name;
                                        Value bound_val = val_nil();
                                        // Try to get the field by name from match_val
                                        if (IS_ARRAY(field_names_arr) && a < field_names_arr.as.array->count) {
                                            const char* fname = AS_STRING(field_names_arr.as.array->elements[a]);
                                            bound_val = dict_get(&match_val, fname);
                                        }
                                        // Fallback: single-field variant uses __val
                                        if (IS_NIL(bound_val) && pat->as.call.arg_count == 1) {
                                            bound_val = dict_get(&match_val, "__val");
                                        }
                                        env_define(match_scope, bind.start, bind.length, bound_val);
                                    }
                                }
                            }
                        }
                    }
                }
                // ── P13: Simple ADT variant match: Shape.Point (no args) ──────
                else if (pat->type == EXPR_GET && IS_DICT(match_val)) {
                    Token prop = pat->as.get.property;
                    ExecResult obj_res = eval_expr(pat->as.get.object, env);
                    if (!obj_res.is_throwing && IS_DICT(obj_res.value)) {
                        Value enum_name_val = dict_get(&obj_res.value, "__name__");
                        if (IS_STRING(enum_name_val)) {
                            char expected_type[256];
                            snprintf(expected_type, sizeof(expected_type), "%s.%.*s",
                                     AS_STRING(enum_name_val), prop.length, prop.start);
                            Value match_type = dict_get(&match_val, "__type");
                            if (IS_STRING(match_type) && strcmp(AS_STRING(match_type), expected_type) == 0) {
                                matched = 1;
                            }
                        }
                    }
                }
                // ── Range pattern: lo..hi or lo..=hi ─────────────────────
                else if (pat->type == EXPR_RANGE) {
                    ExecResult lo_res = eval_expr(pat->as.range.low, env);
                    ExecResult hi_res = eval_expr(pat->as.range.high, env);
                    if (!lo_res.is_throwing && !hi_res.is_throwing
                            && IS_NUMERIC(match_val)
                            && IS_NUMERIC(lo_res.value) && IS_NUMERIC(hi_res.value)) {
                        double v  = NUMERIC_AS_DOUBLE(match_val);
                        double lo = NUMERIC_AS_DOUBLE(lo_res.value);
                        double hi = NUMERIC_AS_DOUBLE(hi_res.value);
                        matched = (v >= lo) && (pat->as.range.inclusive ? v <= hi : v < hi);
                    }
                }
                // ── Binding capture: plain identifier (not _ ) ────────────
                else if (pat->type == EXPR_VARIABLE) {
                    Token bind = pat->as.variable.name;
                    env_define(match_scope, bind.start, bind.length, match_val);
                    matched = 1;
                }
                // ── Literal equality ──────────────────────────────────────
                else {
                    ExecResult pat_res = eval_expr(pat, env);
                    if (pat_res.is_throwing) { AST_GC_POP(); return pat_res; }
                    matched = values_equal(match_val, pat_res.value);
                }

                if (!matched) continue;

                // Guard check
                if (clause->guard) {
                    ExecResult guard_res = eval_expr(clause->guard, match_scope);
                    if (guard_res.is_throwing) { AST_GC_POP(); return guard_res; }
                    if (!is_truthy(guard_res.value)) continue;
                }

                ExecResult res = interpret(clause->body, match_scope);
                AST_GC_POP();
                return res;
            }

            // No case matched — run default if present
            if (stmt->as.match_stmt.default_case != NULL) {
                ExecResult res = interpret(stmt->as.match_stmt.default_case, env);
                AST_GC_POP();
                return res;
            }
            // P14: Firefly — warn when no case matched and no default
            {
                FireflyLoc loc = firefly_loc_from_expr(stmt->as.match_stmt.value);
                firefly_set_code("W002");
                firefly_report(FIREFLY_WARNING, loc,
                              "non-exhaustive match — no case matched and no default");
                firefly_explain("The match value didn't match any case pattern.");
                firefly_advice("Add a 'default:' case to handle unmatched values.");
                firefly_end();
            }
            AST_GC_POP();
            return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
        }

        case STMT_DEFER:
            // Defer is handled at the block level — collect and run on scope exit.
            // When encountered standalone, just execute immediately (fallback).
            return interpret(stmt->as.defer.statement, env);

        // Phase 17: comptime block — in interpreter, just execute the body normally
        case STMT_COMPTIME:
            return interpret(stmt->as.comptime.body, env);

        // Phase 17: macro definition — register macro as a function in environment
        case STMT_MACRO_DEF: {
            // In interpreter mode, macros are treated as regular functions.
            // Re-use the macro_def's ProcStmt-compatible fields directly
            // by wrapping them in a val_function (which uses gc_alloc).
            Token name = stmt->as.macro_def.name;
            ProcStmt* proc = SAGE_ALLOC(sizeof(ProcStmt));
            proc->name = name;
            proc->params = stmt->as.macro_def.params;
            proc->param_types = NULL;
            proc->defaults = NULL;
            proc->param_count = stmt->as.macro_def.param_count;
            proc->required_count = stmt->as.macro_def.param_count;
            proc->return_type = NULL;
            proc->type_params = NULL;
            proc->type_param_count = 0;
            proc->doc = NULL;
            proc->body = stmt->as.macro_def.body;
            // Use val_function which allocates via gc_alloc (GC-tracked)
            Value func_val = val_function(proc, env);
            env_define_const(env, name.start, name.length, func_val);
            return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
        }
    }
    return (ExecResult){ val_nil(), 0, 0, 0, 0, val_nil(), 0, NULL };
}

// Public wrapper for eval_expr (used by vm.c for default arg evaluation)
ExecResult eval_expr_public(Expr* expr, Env* env) {
    return eval_expr(expr, env);
}
