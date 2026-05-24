// src/stdlib.c - Native standard library modules
//
// Provides C-backed implementations for: math, io, string, sys, thread
// These are registered as pre-loaded modules in the module cache,
// so `import math` finds the native version before any .sage file.

#define _DEFAULT_SOURCE
#include "module.h"
#include "lilybox.h"
#include "value.h"

// P8: LilyKnight enforcement for stdlib functions
// Returns val_nil() on violation (stdlib functions can't return ExecResult)
#define STDLIB_LK_CHECK_FS_READ(path) do { \
    if (lk_current_sandbox) { \
        LKViolation _v = lk_check_fs_read(lk_current_sandbox, path); \
        if (_v != LK_OK) { \
            lk_log_violation(lk_current_sandbox, _v, path); \
            fprintf(stderr, "%s\n", lk_violation_str(_v)); \
            return val_nil(); \
        } \
    } \
} while(0)

#define STDLIB_LK_CHECK_FS_WRITE(path) do { \
    if (lk_current_sandbox) { \
        LKViolation _v = lk_check_fs_write(lk_current_sandbox, path); \
        if (_v != LK_OK) { \
            lk_log_violation(lk_current_sandbox, _v, path); \
            fprintf(stderr, "%s\n", lk_violation_str(_v)); \
            return val_nil(); \
        } \
    } \
} while(0)

#define STDLIB_LK_CHECK_EXEC(cmd) do { \
    if (lk_current_sandbox) { \
        LKViolation _v = lk_check_proc_exec(lk_current_sandbox, cmd); \
        if (_v != LK_OK) { \
            lk_log_violation(lk_current_sandbox, _v, cmd); \
            fprintf(stderr, "%s\n", lk_violation_str(_v)); \
            return val_nil(); \
        } \
    } \
} while(0)

#define STDLIB_LK_CHECK_FFI(lib) do { \
    if (lk_current_sandbox) { \
        LKViolation _v = lk_check_ffi(lk_current_sandbox, lib); \
        if (_v != LK_OK) { \
            lk_log_violation(lk_current_sandbox, _v, lib); \
            fprintf(stderr, "%s\n", lk_violation_str(_v)); \
            return val_nil(); \
        } \
    } \
} while(0)
#include "env.h"
#include "gc.h"
#include "interpreter.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include "sage_thread.h"

// ============================================================================
// Helper: Create a native module (pre-loaded, no .sage file needed)
// ============================================================================

Module* create_native_module(ModuleCache* cache, const char* name) {
    Module* m = SAGE_ALLOC(sizeof(Module));
    m->name = SAGE_STRDUP(name);
    m->path = NULL;
    m->source = NULL;
    m->ast = NULL;
    m->ast_tail = NULL;
    m->env = env_create(g_global_env);
    m->is_loaded = true;
    m->is_loading = false;
    m->next = cache->modules;
    cache->modules = m;
    return m;
}

#include "program.h"
#include "vm.h"
#include "parser.h"

extern Stmt* parse_program(const char* source, const char* input_path);

// ============================================================================
// VM MODULE
// ============================================================================

static Value vm_compile_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return val_nil();
    const char* source = AS_STRING(args[0]);

    // Save lexer and parser state to allow nested parsing (e.g. from within a running script)
    LexerState l_state = lexer_get_state();
    ParserState p_state = parser_get_state();

    Stmt* ast = parse_program(source, "<vm-compile>");

    // Restore lexer and parser state
    lexer_set_state(l_state);
    parser_set_state(p_state);

    if (!ast) return val_nil();

    BytecodeProgram* program = SAGE_ALLOC(sizeof(BytecodeProgram));
    bytecode_program_init(program);

    char error[256];
    if (!bytecode_compile_program(program, ast, BYTECODE_COMPILE_HYBRID, error, sizeof(error))) {
        fprintf(stderr, "VM Compile Error: %s\n", error);
        free_stmt(ast);
        bytecode_program_free(program);
        free(program);
        return val_nil();
    }

    free_stmt(ast);
    return val_vm_program(program);
}

static Value vm_execute_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_VM_PROGRAM(args[0])) return val_nil();
    BytecodeProgram* program = AS_PROGRAM(args[0]);

    Env* env = env_create(g_global_env);
    
    // If a dictionary is provided, expose it as 'state' variable
    if (argCount >= 2 && IS_DICT(args[1])) {
        env_define_const(env, "state", 5, args[1]);
    }

    ExecResult res = vm_execute_program(program, env);
    
    return res.value;
}

extern int bytecode_program_write_file(const BytecodeProgram* program, const char* output_path,
                                char* error, size_t error_size);
extern int bytecode_program_read_file(BytecodeProgram* program, const char* input_path,
                               char* error, size_t error_size);

// Internal version of write/read that takes FILE* would be better, but we'll use temp files for now
// to avoid refactoring the core VM artifact logic which is heavily tied to file IO.
// Actually, let's just use a string-based buffer if possible.
// Wait, I'll just refactor bytecode_program_write_file in program.c to use a FILE* helper.

static Value vm_serialize_native(int argCount, Value* args) {
    if (argCount < 1 || args[0].type != VAL_POINTER) return val_nil();
    BytecodeProgram* program = (BytecodeProgram*)args[0].as.pointer->ptr;

    char tmp_path[] = "/tmp/moss_vm_XXXXXX.mvm";
    int fd = mkstemps(tmp_path, 4);
    if (fd < 0) return val_nil();
    close(fd);

    char error[256];
    if (!bytecode_program_write_file(program, tmp_path, error, sizeof(error))) {
        unlink(tmp_path);
        return val_nil();
    }

    STDLIB_LK_CHECK_FS_READ(tmp_path);
    FILE* f = fopen(tmp_path, "rb");
    if (!f) { unlink(tmp_path); return val_nil(); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    unsigned char* data = SAGE_ALLOC((size_t)size);
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        unlink(tmp_path);
        return val_nil();
    }
    fclose(f);
    unlink(tmp_path);

    return val_bytes(data, (int)size);
}

static Value vm_deserialize_native(int argCount, Value* args) {
    if (argCount < 1 || args[0].type != VAL_BYTES) return val_nil();
    BytesValue* bv = args[0].as.bytes;

    char tmp_path[] = "/tmp/moss_vm_XXXXXX.mvm";
    int fd = mkstemps(tmp_path, 4);
    if (fd < 0) return val_nil();
    
    if (write(fd, bv->data, (size_t)bv->length) != bv->length) {
        close(fd);
        unlink(tmp_path);
        return val_nil();
    }
    close(fd);

    BytecodeProgram* program = SAGE_ALLOC(sizeof(BytecodeProgram));
    bytecode_program_init(program);

    char error[256];
    if (!bytecode_program_read_file(program, tmp_path, error, sizeof(error))) {
        fprintf(stderr, "VM Deserialize Error: %s\n", error);
        bytecode_program_free(program);
        free(program);
        unlink(tmp_path);
        return val_nil();
    }
    unlink(tmp_path);

    return val_pointer(program, sizeof(BytecodeProgram), 1);
}

Module* create_vm_module(ModuleCache* cache) {
    Module* m = create_native_module(cache, "vm");
    Environment* e = m->env;

    env_define_const(e, "compile", 7, val_native(vm_compile_native));
    env_define_const(e, "execute", 7, val_native(vm_execute_native));
    env_define_const(e, "serialize", 9, val_native(vm_serialize_native));
    env_define_const(e, "deserialize", 11, val_native(vm_deserialize_native));

    return m;
}

// ============================================================================
// MATH MODULE
// ============================================================================

static Value math_sin_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_nil();
    return val_number(sin(NUMERIC_AS_DOUBLE(args[0])));
}

static Value math_cos_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_nil();
    return val_number(cos(NUMERIC_AS_DOUBLE(args[0])));
}

static Value math_tan_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_nil();
    return val_number(tan(NUMERIC_AS_DOUBLE(args[0])));
}

static Value math_asin_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_nil();
    return val_number(asin(NUMERIC_AS_DOUBLE(args[0])));
}

static Value math_acos_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_nil();
    return val_number(acos(NUMERIC_AS_DOUBLE(args[0])));
}

static Value math_atan_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_nil();
    return val_number(atan(NUMERIC_AS_DOUBLE(args[0])));
}

static Value math_atan2_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_NUMERIC(args[0]) || !IS_NUMERIC(args[1])) return val_nil();
    return val_number(atan2(NUMERIC_AS_DOUBLE(args[0]), NUMERIC_AS_DOUBLE(args[1])));
}

static Value math_random_native(int argCount, Value* args) {
    return val_number((double)rand() / (double)RAND_MAX);
}

static Value math_sqrt_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_nil();
    return val_number(sqrt(NUMERIC_AS_DOUBLE(args[0])));
}

static Value math_pow_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_NUMERIC(args[0]) || !IS_NUMERIC(args[1])) return val_nil();
    return val_number(pow(NUMERIC_AS_DOUBLE(args[0]), NUMERIC_AS_DOUBLE(args[1])));
}

static Value math_log_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_nil();
    return val_number(log(NUMERIC_AS_DOUBLE(args[0])));
}

static Value math_log10_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_nil();
    return val_number(log10(NUMERIC_AS_DOUBLE(args[0])));
}

static Value math_exp_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_nil();
    return val_number(exp(NUMERIC_AS_DOUBLE(args[0])));
}

static Value math_floor_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_nil();
    return val_number(floor(NUMERIC_AS_DOUBLE(args[0])));
}

static Value math_ceil_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_nil();
    return val_number(ceil(NUMERIC_AS_DOUBLE(args[0])));
}

static Value math_round_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_nil();
    return val_number(round(NUMERIC_AS_DOUBLE(args[0])));
}

static Value math_abs_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_nil();
    if (IS_INT(args[0])) { int64_t v = AS_INT(args[0]); return val_int(v < 0 ? -v : v); }
    return val_number(fabs(NUMERIC_AS_DOUBLE(args[0])));
}

static Value math_fmod_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_NUMERIC(args[0]) || !IS_NUMERIC(args[1])) return val_nil();
    if (IS_INT(args[0]) && IS_INT(args[1])) {
        int64_t b = AS_INT(args[1]); return b != 0 ? val_int(AS_INT(args[0]) % b) : val_int(0);
    }
    return val_number(fmod(NUMERIC_AS_DOUBLE(args[0]), NUMERIC_AS_DOUBLE(args[1])));
}

static Value math_min_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_NUMERIC(args[0]) || !IS_NUMERIC(args[1])) return val_nil();
    if (IS_INT(args[0]) && IS_INT(args[1])) {
        int64_t a = AS_INT(args[0]), b = AS_INT(args[1]); return val_int(a < b ? a : b);
    }
    double a = NUMERIC_AS_DOUBLE(args[0]), b = NUMERIC_AS_DOUBLE(args[1]);
    return val_number(a < b ? a : b);
}

static Value math_max_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_NUMERIC(args[0]) || !IS_NUMERIC(args[1])) return val_nil();
    if (IS_INT(args[0]) && IS_INT(args[1])) {
        int64_t a = AS_INT(args[0]), b = AS_INT(args[1]); return val_int(a > b ? a : b);
    }
    double a = NUMERIC_AS_DOUBLE(args[0]), b = NUMERIC_AS_DOUBLE(args[1]);
    return val_number(a > b ? a : b);
}

static Value math_clamp_native(int argCount, Value* args) {
    if (argCount < 3 || !IS_NUMERIC(args[0]) || !IS_NUMERIC(args[1]) || !IS_NUMERIC(args[2]))
        return val_nil();
    if (IS_INT(args[0]) && IS_INT(args[1]) && IS_INT(args[2])) {
        int64_t v = AS_INT(args[0]), lo = AS_INT(args[1]), hi = AS_INT(args[2]);
        if (v < lo) return val_int(lo);
        if (v > hi) return val_int(hi);
        return val_int(v);
    }
    double v = NUMERIC_AS_DOUBLE(args[0]);
    double lo = NUMERIC_AS_DOUBLE(args[1]);
    double hi = NUMERIC_AS_DOUBLE(args[2]);
    if (v < lo) return val_number(lo);
    if (v > hi) return val_number(hi);
    return val_number(v);
}

static Value math_isnan_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_bool(0);
    return val_bool(isnan(NUMERIC_AS_DOUBLE(args[0])));
}

static Value math_isinf_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_bool(0);
    return val_bool(isinf(NUMERIC_AS_DOUBLE(args[0])));
}

Module* create_math_module(ModuleCache* cache) {
    Module* m = create_native_module(cache, "_math");
    Environment* e = m->env;

    // Trig
    env_define_const(e, "sin", 3, val_native(math_sin_native));
    env_define_const(e, "cos", 3, val_native(math_cos_native));
    env_define_const(e, "tan", 3, val_native(math_tan_native));
    env_define_const(e, "asin", 4, val_native(math_asin_native));
    env_define_const(e, "acos", 4, val_native(math_acos_native));
    env_define_const(e, "atan", 4, val_native(math_atan_native));
    env_define_const(e, "atan2", 5, val_native(math_atan2_native));

    // Powers & roots
    env_define_const(e, "sqrt", 4, val_native(math_sqrt_native));
    env_define_const(e, "pow", 3, val_native(math_pow_native));
    env_define_const(e, "log", 3, val_native(math_log_native));
    env_define_const(e, "log10", 5, val_native(math_log10_native));
    env_define_const(e, "exp", 3, val_native(math_exp_native));

    // Rounding
    env_define_const(e, "floor", 5, val_native(math_floor_native));
    env_define_const(e, "ceil", 4, val_native(math_ceil_native));
    env_define_const(e, "round", 5, val_native(math_round_native));
    env_define_const(e, "abs", 3, val_native(math_abs_native));
    env_define_const(e, "fmod", 4, val_native(math_fmod_native));

    // Min/max/clamp
    env_define_const(e, "min", 3, val_native(math_min_native));
    env_define_const(e, "max", 3, val_native(math_max_native));
    env_define_const(e, "clamp", 5, val_native(math_clamp_native));

    // Checks
    env_define_const(e, "isnan", 5, val_native(math_isnan_native));
    env_define_const(e, "isinf", 5, val_native(math_isinf_native));

    // Constants
    env_define_const(e, "pi", 2, val_number(3.14159265358979323846));
    env_define_const(e, "PI", 2, val_number(3.14159265358979323846));  // Uppercase alias
    env_define_const(e, "e", 1, val_number(2.71828182845904523536));
    env_define_const(e, "inf", 3, val_number(INFINITY));
    env_define_const(e, "nan", 3, val_number(NAN));
    env_define_const(e, "tau", 3, val_number(6.28318530717958647692));

    // Random
    env_define_const(e, "random", 6, val_native(math_random_native));

    return m;
}

// ============================================================================
// IO MODULE
// ============================================================================

static Value io_readfile_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return val_nil();
    const char* path = AS_STRING(args[0]);
    // LilyKnight: check fs.read permission
    if (lk_current_sandbox) {
        LKViolation _lkv = lk_check_fs_read(lk_current_sandbox, path);
        if (_lkv != LK_OK) { lk_log_violation(lk_current_sandbox, _lkv, path); return val_nil(); }
    }
    FILE* f = fopen(path, "rb");
    if (!f) return val_nil();

    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    if (length < 0) { fclose(f); return val_nil(); }
    fseek(f, 0, SEEK_SET);

    char* buf = SAGE_ALLOC((size_t)length + 1);
    size_t read = fread(buf, 1, (size_t)length, f);
    buf[read] = '\0';
    fclose(f);

    return val_string_take(buf);
}

static Value io_writefile_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) return val_bool(0);
    const char* path = AS_STRING(args[0]);
    const char* content = AS_STRING(args[1]);
    // LilyKnight: check fs.write permission
    if (lk_current_sandbox) {
        LKViolation _lkv = lk_check_fs_write(lk_current_sandbox, path);
        if (_lkv != LK_OK) { lk_log_violation(lk_current_sandbox, _lkv, path); return val_bool(0); }
    }

    FILE* f = fopen(path, "wb");
    if (!f) return val_bool(0);

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);

    return val_bool(written == len);
}

static Value io_writebytes_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[0]) || !IS_ARRAY(args[1])) return val_bool(0);
    const char* path = AS_STRING(args[0]);
    ArrayValue* arr = AS_ARRAY(args[1]);

    STDLIB_LK_CHECK_FS_WRITE(path);
    FILE* f = fopen(path, "wb");
    if (!f) return val_bool(0);

    unsigned char* buf = malloc((size_t)arr->count);
    for (int i = 0; i < arr->count; i++) {
        if (IS_NUMERIC(arr->elements[i])) {
            buf[i] = (unsigned char)NUMERIC_AS_DOUBLE(arr->elements[i]);
        } else {
            buf[i] = 0;
        }
    }

    size_t written = fwrite(buf, 1, (size_t)arr->count, f);
    free(buf);
    fclose(f);

    return val_bool(written == (size_t)arr->count);
}

static Value io_appendbytes_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[0]) || !IS_ARRAY(args[1])) return val_bool(0);
    const char* path = AS_STRING(args[0]);
    ArrayValue* arr = AS_ARRAY(args[1]);

    STDLIB_LK_CHECK_FS_WRITE(path);
    FILE* f = fopen(path, "ab");
    if (!f) return val_bool(0);

    unsigned char* buf = malloc((size_t)arr->count);
    for (int i = 0; i < arr->count; i++) {
        if (IS_NUMERIC(arr->elements[i])) {
            buf[i] = (unsigned char)NUMERIC_AS_DOUBLE(arr->elements[i]);
        } else {
            buf[i] = 0;
        }
    }

    size_t written = fwrite(buf, 1, (size_t)arr->count, f);
    free(buf);
    fclose(f);

    return val_bool(written == (size_t)arr->count);
}

static Value io_appendfile_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) return val_bool(0);
    const char* path = AS_STRING(args[0]);
    const char* content = AS_STRING(args[1]);

    STDLIB_LK_CHECK_FS_WRITE(path);
    FILE* f = fopen(path, "ab");
    if (!f) return val_bool(0);

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);

    return val_bool(written == len);
}

static Value io_exists_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return val_bool(0);
    struct stat st;
    return val_bool(stat(AS_STRING(args[0]), &st) == 0);
}

static Value io_remove_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return val_bool(0);
    return val_bool(remove(AS_STRING(args[0])) == 0);
}

static Value io_isdir_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return val_bool(0);
    struct stat st;
    if (stat(AS_STRING(args[0]), &st) != 0) return val_bool(0);
    return val_bool(S_ISDIR(st.st_mode));
}

static Value io_mkdir_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return val_bool(0);
    int status = mkdir(AS_STRING(args[0]), 0777);
    return val_bool(status == 0);
}

static Value io_filesize_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return val_int(-1);
    struct stat st;
    if (stat(AS_STRING(args[0]), &st) != 0) return val_int(-1);
    return val_number((double)st.st_size);
}

// io.readbytes(path) -> array of byte values (0-255)
static Value io_readbytes_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return val_nil();
    STDLIB_LK_CHECK_FS_READ(AS_STRING(args[0]));
    FILE* f = fopen(AS_STRING(args[0]), "rb");
    if (!f) return val_nil();
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    if (length < 0 || length > 100*1024*1024) { fclose(f); return val_nil(); } // 100MB max
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = malloc((size_t)length);
    size_t read = fread(buf, 1, (size_t)length, f);
    fclose(f);
    // Create array of byte values
    Value out_val = val_array();
    ArrayValue* arr = out_val.as.array;
    arr->count = (int)read;
    arr->capacity = (int)read;
    arr->elements = SAGE_ALLOC(sizeof(Value) * read);
    gc_track_external_allocation(sizeof(Value) * (size_t)read);
    for (size_t i = 0; i < read; i++) {
        arr->elements[i] = val_number((double)buf[i]);
    }
    free(buf);
    return out_val;
}

// io.listdir(path) -> array of filename strings
static Value io_listdir_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return val_nil();
    DIR* d = opendir(AS_STRING(args[0]));
    if (!d) return val_nil();
    Value out_val = val_array();
    ArrayValue* arr = out_val.as.array;
    arr->count = 0;
    arr->capacity = 32;
    arr->elements = SAGE_ALLOC(sizeof(Value) * 32);
    gc_track_external_allocation(sizeof(Value) * 32);
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue; // skip hidden/. /..
        if (arr->count >= arr->capacity) {
            size_t old_cap = (size_t)arr->capacity;
            arr->capacity *= 2;
            arr->elements = SAGE_REALLOC(arr->elements, sizeof(Value) * (size_t)arr->capacity);
            gc_track_external_resize(sizeof(Value) * old_cap, sizeof(Value) * (size_t)arr->capacity);
        }
        arr->elements[arr->count++] = val_string(entry->d_name);
    }
    closedir(d);
    return out_val;
}

Module* create_io_module(ModuleCache* cache) {
    Module* m = create_native_module(cache, "io");
    Environment* e = m->env;

    env_define_const(e, "readfile", 8, val_native(io_readfile_native));
    env_define_const(e, "writefile", 9, val_native(io_writefile_native));
    env_define_const(e, "appendfile", 10, val_native(io_appendfile_native));
    env_define_const(e, "writebytes", 10, val_native(io_writebytes_native));
    env_define_const(e, "appendbytes", 11, val_native(io_appendbytes_native));
    env_define_const(e, "exists", 6, val_native(io_exists_native));
    env_define_const(e, "remove", 6, val_native(io_remove_native));
    env_define_const(e, "isdir", 5, val_native(io_isdir_native));
    env_define_const(e, "mkdir", 5, val_native(io_mkdir_native));
    env_define_const(e, "filesize", 8, val_native(io_filesize_native));
    env_define_const(e, "readbytes", 9, val_native(io_readbytes_native));
    env_define_const(e, "listdir", 7, val_native(io_listdir_native));

    return m;
}

// ============================================================================
// STRING MODULE
// ============================================================================

static Value str_find_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) return val_int(-1);
    const char* haystack = AS_STRING(args[0]);
    const char* needle = AS_STRING(args[1]);
    const char* p = strstr(haystack, needle);
    if (!p) return val_int(-1);
    return val_number((double)(p - haystack));
}

static Value str_rfind_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) return val_int(-1);
    const char* haystack = AS_STRING(args[0]);
    const char* needle = AS_STRING(args[1]);
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) return val_int(-1);
    for (size_t i = hlen - nlen + 1; i > 0; i--) {
        if (memcmp(haystack + i - 1, needle, nlen) == 0)
            return val_number((double)(i - 1));
    }
    return val_int(-1);
}

static Value str_startswith_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) return val_bool(0);
    const char* s = AS_STRING(args[0]);
    const char* prefix = AS_STRING(args[1]);
    size_t plen = strlen(prefix);
    return val_bool(strncmp(s, prefix, plen) == 0);
}

static Value str_endswith_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) return val_bool(0);
    const char* s = AS_STRING(args[0]);
    const char* suffix = AS_STRING(args[1]);
    size_t slen = strlen(s);
    size_t xlen = strlen(suffix);
    if (xlen > slen) return val_bool(0);
    return val_bool(memcmp(s + slen - xlen, suffix, xlen) == 0);
}

static Value str_contains_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) return val_bool(0);
    return val_bool(strstr(AS_STRING(args[0]), AS_STRING(args[1])) != NULL);
}

static Value str_char_at_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[0]) || !IS_NUMERIC(args[1])) return val_nil();
    const char* s = AS_STRING(args[0]);
    int idx = (int)NUMERIC_AS_INT(args[1]);
    int slen = (int)strlen(s);
    if (idx < 0 || idx >= slen) return val_nil();
    char buf[2] = { s[idx], '\0' };
    return val_string(buf);
}

static Value str_ord_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return val_nil();
    const char* s = AS_STRING(args[0]);
    if (s[0] == '\0') return val_nil();
    return val_number((double)(unsigned char)s[0]);
}

static Value str_chr_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_nil();
    int code = (int)NUMERIC_AS_INT(args[0]);
    if (code < 0 || code > 127) return val_nil();
    char buf[2] = { (char)code, '\0' };
    return val_string(buf);
}

static Value str_repeat_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[0]) || !IS_NUMERIC(args[1])) return val_nil();
    const char* s = AS_STRING(args[0]);
    int count = (int)NUMERIC_AS_INT(args[1]);
    if (count <= 0) return val_string("");
    size_t slen = strlen(s);
    if (slen > 0 && (size_t)count > (64 * 1024 * 1024) / slen) return val_nil();  // overflow guard
    size_t total = slen * (size_t)count;
    char* buf = SAGE_ALLOC(total + 1);
    for (int i = 0; i < count; i++) {
        memcpy(buf + (size_t)i * slen, s, slen);
    }
    buf[total] = '\0';
    return val_string_take(buf);
}

static Value str_count_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) return val_int(0);
    const char* s = AS_STRING(args[0]);
    const char* sub = AS_STRING(args[1]);
    size_t sublen = strlen(sub);
    if (sublen == 0) return val_int(0);
    int count = 0;
    const char* p = s;
    while ((p = strstr(p, sub)) != NULL) {
        count++;
        p += sublen;
    }
    return val_number((double)count);
}

static Value str_substr_native(int argCount, Value* args) {
    if (argCount < 3 || !IS_STRING(args[0]) || !IS_NUMERIC(args[1]) || !IS_NUMERIC(args[2]))
        return val_nil();
    const char* s = AS_STRING(args[0]);
    int start = (int)NUMERIC_AS_INT(args[1]);
    int length = (int)NUMERIC_AS_INT(args[2]);
    int slen = (int)strlen(s);
    if (start < 0) start = 0;
    if (start >= slen) return val_string("");
    if (length < 0) length = 0;
    if (start + length > slen) length = slen - start;
    char* buf = SAGE_ALLOC((size_t)length + 1);
    memcpy(buf, s + start, (size_t)length);
    buf[length] = '\0';
    return val_string_take(buf);
}

static Value str_reverse_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return val_nil();
    const char* s = AS_STRING(args[0]);
    size_t slen = strlen(s);
    char* buf = SAGE_ALLOC(slen + 1);
    for (size_t i = 0; i < slen; i++) {
        buf[i] = s[slen - 1 - i];
    }
    buf[slen] = '\0';
    return val_string_take(buf);
}

Module* create_string_module(ModuleCache* cache) {
    Module* m = create_native_module(cache, "string");
    Environment* e = m->env;

    env_define_const(e, "find", 4, val_native(str_find_native));
    env_define_const(e, "rfind", 5, val_native(str_rfind_native));
    env_define_const(e, "startswith", 10, val_native(str_startswith_native));
    env_define_const(e, "endswith", 8, val_native(str_endswith_native));
    env_define_const(e, "contains", 8, val_native(str_contains_native));
    env_define_const(e, "char_at", 7, val_native(str_char_at_native));
    env_define_const(e, "ord", 3, val_native(str_ord_native));
    env_define_const(e, "chr", 3, val_native(str_chr_native));
    env_define_const(e, "repeat", 6, val_native(str_repeat_native));
    env_define_const(e, "count", 5, val_native(str_count_native));
    env_define_const(e, "substr", 6, val_native(str_substr_native));
    env_define_const(e, "reverse", 7, val_native(str_reverse_native));

    return m;
}

// ============================================================================
// SYS MODULE
// ============================================================================

static int s_argc = 0;
static const char** s_argv = NULL;

void sage_set_args(int argc, const char** argv) {
    s_argc = argc;
    s_argv = argv;
}

static Value sys_args_native(int argCount, Value* args) {
    (void)argCount; (void)args;
    gc_pin();
    Value arr = val_array();
    for (int i = 0; i < s_argc; i++) {
        array_push(&arr, val_string(s_argv[i]));
    }
    gc_unpin();
    return arr;
}

static Value sys_exit_native(int argCount, Value* args) {
    int code = 0;
    if (argCount >= 1 && IS_NUMERIC(args[0])) {
        code = (int)NUMERIC_AS_INT(args[0]);
    }
    exit(code);
    return val_nil(); // unreachable
}

static Value sys_platform_native(int argCount, Value* args) {
    (void)argCount; (void)args;
#if defined(__linux__)
    return val_string("linux");
#elif defined(__APPLE__)
    return val_string("darwin");
#elif defined(_WIN32)
    return val_string("windows");
#else
    return val_string("unknown");
#endif
}

static Value sys_getenv_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return val_nil();
    const char* val = getenv(AS_STRING(args[0]));
    if (!val) return val_nil();
    return val_string(val);
}

static Value sys_clock_native(int argCount, Value* args) {
    (void)argCount; (void)args;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return val_number((double)tv.tv_sec + (double)tv.tv_usec / 1000000.0);
}

static Value sys_sleep_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_nil();
    double seconds = NUMERIC_AS_DOUBLE(args[0]);
    if (seconds > 0) {
        struct timespec ts;
        ts.tv_sec = (time_t)seconds;
        ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
    }
    return val_nil();
}

static Value sys_exec_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return val_nil();
    const char* cmd = AS_STRING(args[0]);
    STDLIB_LK_CHECK_EXEC(cmd);
    int result = system(cmd);
    return val_int(result);
}

Module* create_sys_module(ModuleCache* cache) {
    Module* m = create_native_module(cache, "sys");
    Environment* e = m->env;

    // args is a function that returns the array (so it's always current)
    env_define_const(e, "args", 4, val_native(sys_args_native));
    env_define_const(e, "exit", 4, val_native(sys_exit_native));
    env_define_const(e, "getenv", 6, val_native(sys_getenv_native));
    env_define_const(e, "clock", 5, val_native(sys_clock_native));
    env_define_const(e, "sleep", 5, val_native(sys_sleep_native));
    env_define_const(e, "exec", 4, val_native(sys_exec_native));

    // Constants
    env_define_const(e, "version", 7, val_string(SAGE_VERSION_STR));
    {
        Value plat = sys_platform_native(0, NULL);
        env_define_const(e, "platform", 8, plat);
    }

    return m;
}

// ============================================================================
// FAT MODULE
// ============================================================================

typedef struct {
    int valid;
    int fat_bits;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sector_count;
    uint32_t fat_count;
    uint32_t root_entry_count;
    uint32_t total_sectors;
    uint32_t sectors_per_fat;
    uint32_t root_dir_sectors;
    uint32_t first_data_sector;
    uint32_t data_sector_count;
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint32_t media_descriptor;
    char fs_type_label[16];
    char volume_label[16];
} FatBootInfo;

static uint16_t fat_le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t fat_le32(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int fat_is_pow2(uint32_t x) {
    return x != 0 && (x & (x - 1)) == 0;
}

static void fat_copy_trimmed(char* dst, size_t dst_cap, const uint8_t* src, size_t src_len) {
    if (dst_cap == 0) return;
    size_t n = src_len;
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\0')) {
        n--;
    }
    if (n >= dst_cap) n = dst_cap - 1;
    for (size_t i = 0; i < n; i++) {
        dst[i] = (char)src[i];
    }
    dst[n] = '\0';
}

static int fat_prefix_eq(const char* s, const char* prefix) {
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

static void fat_parse_boot_info(const uint8_t* bytes, size_t len, FatBootInfo* info) {
    memset(info, 0, sizeof(*info));
    if (bytes == NULL || len < 64) {
        return;
    }

    info->bytes_per_sector = fat_le16(bytes + 11);
    info->sectors_per_cluster = bytes[13];
    info->reserved_sector_count = fat_le16(bytes + 14);
    info->fat_count = bytes[16];
    info->root_entry_count = fat_le16(bytes + 17);
    uint32_t total16 = fat_le16(bytes + 19);
    info->media_descriptor = bytes[21];
    uint32_t fat16 = fat_le16(bytes + 22);
    uint32_t total32 = fat_le32(bytes + 32);
    uint32_t fat32 = fat_le32(bytes + 36);
    info->root_cluster = fat_le32(bytes + 44);

    info->total_sectors = (total16 != 0) ? total16 : total32;
    info->sectors_per_fat = (fat16 != 0) ? fat16 : fat32;

    if (len >= 90) {
        if (info->root_entry_count == 0) {
            fat_copy_trimmed(info->volume_label, sizeof(info->volume_label), bytes + 71, 11);
            fat_copy_trimmed(info->fs_type_label, sizeof(info->fs_type_label), bytes + 82, 8);
        } else {
            fat_copy_trimmed(info->volume_label, sizeof(info->volume_label), bytes + 43, 11);
            fat_copy_trimmed(info->fs_type_label, sizeof(info->fs_type_label), bytes + 54, 8);
        }
    }

    if (!fat_is_pow2(info->bytes_per_sector) || info->bytes_per_sector < 128 || info->bytes_per_sector > 4096) {
        return;
    }
    if (!fat_is_pow2(info->sectors_per_cluster) || info->sectors_per_cluster == 0 || info->sectors_per_cluster > 128) {
        return;
    }
    if (info->reserved_sector_count == 0 || info->fat_count == 0 || info->sectors_per_fat == 0 || info->total_sectors == 0) {
        return;
    }

    uint32_t root_dir_sectors = (uint32_t)(((uint64_t)info->root_entry_count * 32u + (info->bytes_per_sector - 1u)) / info->bytes_per_sector);
    uint64_t first_data = (uint64_t)info->reserved_sector_count + ((uint64_t)info->fat_count * info->sectors_per_fat) + root_dir_sectors;
    if (first_data >= info->total_sectors) {
        return;
    }
    uint32_t data_sectors = (uint32_t)(info->total_sectors - first_data);
    uint32_t cluster_count = data_sectors / info->sectors_per_cluster;

    info->root_dir_sectors = root_dir_sectors;
    info->first_data_sector = (uint32_t)first_data;
    info->data_sector_count = data_sectors;
    info->cluster_count = cluster_count;

    // Include FAT8 for very small data regions as requested.
    if (cluster_count < 16) info->fat_bits = 8;
    else if (cluster_count < 4085) info->fat_bits = 12;
    else if (cluster_count < 65525) info->fat_bits = 16;
    else info->fat_bits = 32;

    // Let explicit filesystem labels refine the guess.
    if (fat_prefix_eq(info->fs_type_label, "FAT8")) info->fat_bits = 8;
    else if (fat_prefix_eq(info->fs_type_label, "FAT12")) info->fat_bits = 12;
    else if (fat_prefix_eq(info->fs_type_label, "FAT16")) info->fat_bits = 16;
    else if (fat_prefix_eq(info->fs_type_label, "FAT32")) info->fat_bits = 32;

    if (info->fat_bits == 32 && info->root_cluster == 0) {
        info->root_cluster = 2;
    }

    info->valid = 1;
}

static const char* fat_type_name_bits(int bits) {
    switch (bits) {
        case 8: return "FAT8";
        case 12: return "FAT12";
        case 16: return "FAT16";
        case 32: return "FAT32";
        default: return "UNKNOWN";
    }
}

static Value fat_boot_info_to_value(const FatBootInfo* info) {
    Value out = val_dict();
    dict_set(&out, "valid", val_bool(info->valid));
    dict_set(&out, "fat_bits", val_number((double)info->fat_bits));
    dict_set(&out, "fat_type", val_string(fat_type_name_bits(info->fat_bits)));
    dict_set(&out, "bytes_per_sector", val_number((double)info->bytes_per_sector));
    dict_set(&out, "sectors_per_cluster", val_number((double)info->sectors_per_cluster));
    dict_set(&out, "reserved_sector_count", val_number((double)info->reserved_sector_count));
    dict_set(&out, "fat_count", val_number((double)info->fat_count));
    dict_set(&out, "root_entry_count", val_number((double)info->root_entry_count));
    dict_set(&out, "total_sectors", val_number((double)info->total_sectors));
    dict_set(&out, "sectors_per_fat", val_number((double)info->sectors_per_fat));
    dict_set(&out, "root_dir_sectors", val_number((double)info->root_dir_sectors));
    dict_set(&out, "first_data_sector", val_number((double)info->first_data_sector));
    dict_set(&out, "data_sector_count", val_number((double)info->data_sector_count));
    dict_set(&out, "cluster_count", val_number((double)info->cluster_count));
    dict_set(&out, "root_cluster", val_number((double)info->root_cluster));
    dict_set(&out, "media_descriptor", val_number((double)info->media_descriptor));
    dict_set(&out, "fs_type_label", val_string(info->fs_type_label));
    dict_set(&out, "volume_label", val_string(info->volume_label));
    return out;
}

static Value fat_parse_boot_sector_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_ARRAY(args[0])) return val_nil();
    ArrayValue* arr = AS_ARRAY(args[0]);
    if (arr == NULL || arr->count <= 0 || arr->count > 1024 * 1024) return val_nil();

    uint8_t* bytes = SAGE_ALLOC((size_t)arr->count);
    for (int i = 0; i < arr->count; i++) {
        Value v = arr->elements[i];
        if (!IS_NUMERIC(v)) {
            free(bytes);
            return val_nil();
        }
        double d = NUMERIC_AS_DOUBLE(v);
        if (d < 0.0 || d > 255.0) {
            free(bytes);
            return val_nil();
        }
        bytes[i] = (uint8_t)((unsigned int)d & 0xffu);
    }

    FatBootInfo info;
    fat_parse_boot_info(bytes, (size_t)arr->count, &info);
    free(bytes);
    return fat_boot_info_to_value(&info);
}

static Value fat_probe_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return val_nil();
    const char* path = AS_STRING(args[0]);
    STDLIB_LK_CHECK_FS_READ(path);
    FILE* f = fopen(path, "rb");
    if (!f) return val_nil();

    uint8_t sector[4096];
    size_t nread = fread(sector, 1, sizeof(sector), f);
    fclose(f);

    FatBootInfo info;
    fat_parse_boot_info(sector, nread, &info);
    Value out = fat_boot_info_to_value(&info);
    dict_set(&out, "source_path", val_string(path));
    return out;
}

static int fat_dict_number(Value* dict, const char* key, double* out) {
    if (dict == NULL || out == NULL || !IS_DICT(*dict)) return 0;
    Value v = dict_get(dict, key);
    if (!IS_NUMERIC(v)) return 0;
    *out = NUMERIC_AS_DOUBLE(v);
    return 1;
}

static Value fat_cluster_to_lba_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_DICT(args[0]) || !IS_NUMERIC(args[1])) return val_nil();
    double first_data = 0.0;
    double spc = 0.0;
    if (!fat_dict_number(&args[0], "first_data_sector", &first_data) ||
        !fat_dict_number(&args[0], "sectors_per_cluster", &spc)) {
        return val_nil();
    }
    double cluster = NUMERIC_AS_DOUBLE(args[1]);
    if (cluster < 2.0 || spc <= 0.0) return val_nil();
    double lba = first_data + (cluster - 2.0) * spc;
    return val_int(lba);
}

static Value fat_fat_entry_offset_native(int argCount, Value* args) {
    if (argCount < 2 || !IS_DICT(args[0]) || !IS_NUMERIC(args[1])) return val_nil();
    double fat_bits = 0.0;
    if (!fat_dict_number(&args[0], "fat_bits", &fat_bits)) return val_nil();
    double cluster = NUMERIC_AS_DOUBLE(args[1]);
    if (cluster < 0.0) return val_nil();

    uint32_t bits = (uint32_t)fat_bits;
    uint32_t cl = (uint32_t)cluster;
    uint32_t offset = 0;
    if (bits == 12) {
        offset = (cl * 3u) / 2u;
    } else {
        offset = (uint32_t)(((uint64_t)cl * (uint64_t)bits) / 8u);
    }

    Value out = val_dict();
    dict_set(&out, "byte_offset", val_number((double)offset));
    dict_set(&out, "is_odd", val_bool((bits == 12) ? (cl & 1u) : 0));
    dict_set(&out, "fat_bits", val_number((double)bits));
    return out;
}

Module* create_fat_module(ModuleCache* cache) {
    Module* m = create_native_module(cache, "fat");
    Environment* e = m->env;

    env_define_const(e, "parse_boot_sector", 17, val_native(fat_parse_boot_sector_native));
    env_define_const(e, "probe", 5, val_native(fat_probe_native));
    env_define_const(e, "cluster_to_lba", 14, val_native(fat_cluster_to_lba_native));
    env_define_const(e, "fat_entry_offset", 16, val_native(fat_fat_entry_offset_native));

    env_define_const(e, "FAT8", 4, val_int(8));
    env_define_const(e, "FAT12", 5, val_int(12));
    env_define_const(e, "FAT16", 5, val_int(16));
    env_define_const(e, "FAT32", 5, val_int(32));

    return m;
}

// ============================================================================
// THREAD MODULE
// ============================================================================

// Thread entry data
typedef struct {
    FunctionValue* func;
    Value* args;
    int arg_count;
    Value result;
} SageThreadData;

static void* sage_thread_entry(void* data) {
    SageThreadData* td = (SageThreadData*)data;
    ProcStmt* proc = (ProcStmt*)td->func->proc;

    // Register this thread for GC
    ThreadState ts;
    memset(&ts, 0, sizeof(ThreadState));
    ts.thread_id = sage_thread_id();
    gc_register_thread(&ts);

    // Create execution scope from function closure
    gc_lock();
    Env* scope = env_create(td->func->closure);
    for (int i = 0; i < td->arg_count && i < proc->param_count; i++) {
        Token paramName = proc->params[i];
        env_define_const(scope, paramName.start, paramName.length, td->args[i]);
    }
    gc_unlock();

    // Execute the function body
    ExecResult res = interpret(proc->body, scope);
    td->result = res.value;

    gc_unregister_thread(&ts);

    return NULL;
}

// thread.spawn(func, arg1, arg2, ...) -> thread handle
Value thread_spawn_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_FUNCTION(args[0])) {
        fprintf(stderr, "Runtime Error: thread.spawn requires a function argument.\n");
        return val_nil();
    }

    FunctionValue* func = args[0].as.function;
    if (func->proc == NULL || func->is_vm) {
        fprintf(stderr, "Runtime Error: thread.spawn requires a non-bytecode function.\n");
        return val_nil();
    }
    int nargs = argCount - 1;

    // Allocate thread data
    SageThreadData* td = SAGE_ALLOC(sizeof(SageThreadData));
    td->func = func;
    td->arg_count = nargs;
    td->result = val_nil();
    if (nargs > 0) {
        td->args = SAGE_ALLOC(sizeof(Value) * (size_t)nargs);
        memcpy(td->args, args + 1, sizeof(Value) * (size_t)nargs);
    } else {
        td->args = NULL;
    }

    // Create thread
    sage_thread_t* handle = SAGE_ALLOC(sizeof(sage_thread_t));
    int err = sage_thread_create(handle, sage_thread_entry, td);
    if (err != 0) {
        fprintf(stderr, "Runtime Error: Failed to create thread (error %d).\n", err);
        free(td->args);
        free(td);
        free(handle);
        return val_nil();
    }

    // Create thread value
    ThreadValue* tv = SAGE_ALLOC(sizeof(ThreadValue));
    tv->handle = handle;
    tv->data = td;
    tv->joined = 0;

    return val_thread(tv);
}

// thread.join(handle) -> result value
static Value thread_join_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_THREAD(args[0])) {
        fprintf(stderr, "Runtime Error: thread.join requires a thread handle.\n");
        return val_nil();
    }

    ThreadValue* tv = AS_THREAD(args[0]);
    if (tv->joined) {
        // Already joined, return cached result
        SageThreadData* td = (SageThreadData*)tv->data;
        return td->result;
    }

    sage_thread_t* handle = (sage_thread_t*)tv->handle;
    sage_thread_join(*handle, NULL);
    tv->joined = 1;

    SageThreadData* td = (SageThreadData*)tv->data;
    Value result = td->result;
    // Free thread resources
    free(td->args);
    free(td);
    free(handle);
    tv->data = NULL;
    tv->handle = NULL;
    return result;
}

// thread.mutex() -> mutex handle
static Value thread_mutex_native(int argCount, Value* args) {
    (void)argCount; (void)args;

    sage_mutex_t* mtx = SAGE_ALLOC(sizeof(sage_mutex_t));
    sage_mutex_init(mtx);

    MutexValue* mv = SAGE_ALLOC(sizeof(MutexValue));
    mv->handle = mtx;

    return val_mutex(mv);
}

// thread.lock(mutex)
static Value thread_lock_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_MUTEX(args[0])) {
        fprintf(stderr, "Runtime Error: thread.lock requires a mutex.\n");
        return val_nil();
    }
    sage_mutex_t* mtx = (sage_mutex_t*)AS_MUTEX(args[0])->handle;
    sage_mutex_lock(mtx);
    return val_nil();
}

// thread.unlock(mutex)
static Value thread_unlock_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_MUTEX(args[0])) {
        fprintf(stderr, "Runtime Error: thread.unlock requires a mutex.\n");
        return val_nil();
    }
    sage_mutex_t* mtx = (sage_mutex_t*)AS_MUTEX(args[0])->handle;
    sage_mutex_unlock(mtx);
    return val_nil();
}

// thread.sleep(seconds)
static Value thread_sleep_native(int argCount, Value* args) {
    if (argCount < 1 || !IS_NUMERIC(args[0])) return val_nil();
    double seconds = NUMERIC_AS_DOUBLE(args[0]);
    sage_sleep_secs(seconds);
    return val_nil();
}

// thread.id() -> current thread id as number
static Value thread_id_native(int argCount, Value* args) {
    (void)argCount; (void)args;
    return val_number((double)sage_thread_id());
}

Module* create_thread_module(ModuleCache* cache) {
    Module* m = create_native_module(cache, "thread");
    Environment* e = m->env;

    env_define_const(e, "spawn", 5, val_native(thread_spawn_native));
    env_define_const(e, "join", 4, val_native(thread_join_native));
    env_define_const(e, "mutex", 5, val_native(thread_mutex_native));
    env_define_const(e, "lock", 4, val_native(thread_lock_native));
    env_define_const(e, "unlock", 6, val_native(thread_unlock_native));
    env_define_const(e, "sleep", 5, val_native(thread_sleep_native));
    env_define_const(e, "id", 2, val_native(thread_id_native));

    return m;
}

// ============================================================================
// GC MODULE — gc.collect(), gc.pause(), gc.resume(), gc.step(n), gc.stats()
// ============================================================================

static Value gc_mod_collect(int argc, Value* args) {
    (void)argc; (void)args;
    gc_collect();
    return val_nil();
}

static Value gc_mod_pause(int argc, Value* args) {
    (void)argc; (void)args;
    gc_disable();
    return val_nil();
}

static Value gc_mod_resume(int argc, Value* args) {
    (void)argc; (void)args;
    gc_enable();
    return val_nil();
}

// gc.step(n) — run n units of incremental GC work (mark + sweep)
static Value gc_mod_step(int argc, Value* args) {
    int n = 100;
    if (argc >= 1 && IS_NUMERIC(args[0])) {
        n = (int)NUMERIC_AS_INT(args[0]);
        if (n <= 0) n = 1;
    }
    // Split n evenly between mark and sweep steps
    int half = n / 2 > 0 ? n / 2 : 1;
    if (!gc_mark_complete()) {
        gc_mark_step(half);
    } else if (!gc_sweep_complete()) {
        gc_sweep_step(half);
    } else {
        // Trigger a new cycle
        gc_begin_cycle();
        gc_mark_step(n);
    }
    return val_nil();
}

static Value gc_mod_stats(int argc, Value* args) {
    (void)argc; (void)args;
    GCStats stats = gc_get_stats();
    gc_pin();
    Value d = val_dict();
    dict_set(&d, "heap_used",     val_number((double)stats.current_bytes));
    dict_set(&d, "heap_total",    val_number((double)stats.bytes_allocated));
    dict_set(&d, "objects",       val_number((double)stats.num_objects));
    dict_set(&d, "collections",   val_number((double)stats.collections));
    dict_set(&d, "freed",         val_number((double)stats.objects_freed));
    dict_set(&d, "next_gc",       val_number((double)stats.next_gc));
    dict_set(&d, "next_gc_bytes", val_number((double)stats.next_gc_bytes));
    dict_set(&d, "max_pause_ns",  val_number((double)stats.max_pause_ns));
    dict_set(&d, "phase",         val_number((double)stats.phase));
    gc_unpin();
    return d;
}

// gc.mode() — returns "tracing", "arc", or "orc"
static Value gc_mod_mode(int argc, Value* args) {
    (void)argc; (void)args;
    if (gc.mode == GC_MODE_ORC) return val_string("orc");
    if (gc.mode == GC_MODE_ARC) return val_string("arc");
    return val_string("tracing");
}

// gc.is_paused() — true if GC is disabled
static Value gc_mod_is_paused(int argc, Value* args) {
    (void)argc; (void)args;
    return val_bool(!gc.enabled);
}

Module* create_gc_module(ModuleCache* cache) {
    Module* m = create_native_module(cache, "gc");
    Environment* e = m->env;
    env_define_const(e, "collect",   7, val_native(gc_mod_collect));
    env_define_const(e, "pause",     5, val_native(gc_mod_pause));
    env_define_const(e, "resume",    6, val_native(gc_mod_resume));
    env_define_const(e, "step",      4, val_native(gc_mod_step));
    env_define_const(e, "stats",     5, val_native(gc_mod_stats));
    env_define_const(e, "mode",      4, val_native(gc_mod_mode));
    env_define_const(e, "is_paused", 9, val_native(gc_mod_is_paused));
    return m;
}

// ============================================================================
// MEMORY MODULE — alloc(), free(), realloc(), memcopy(), memzero()
// For use in @manual and @unsafe blocks.
// ============================================================================

// mem.alloc(n) — allocate n bytes, returns an opaque handle (number = pointer)
static Value mem_alloc(int argc, Value* args) {
    if (argc < 1 || !IS_NUMERIC(args[0])) return val_nil();
    size_t n = (size_t)NUMERIC_AS_INT(args[0]);
    if (n == 0) n = 1;
    void* ptr = malloc(n);
    if (!ptr) return val_nil();
    // Store pointer as a number (uintptr_t). Safe for read-back on the same platform.
    return val_number((double)(uintptr_t)ptr);
}

// mem.free(ptr) — free a pointer obtained from mem.alloc
static Value mem_free(int argc, Value* args) {
    if (argc < 1 || !IS_NUMERIC(args[0])) return val_nil();
    void* ptr = (void*)(uintptr_t)(unsigned long long)NUMERIC_AS_DOUBLE(args[0]);
    free(ptr);
    return val_nil();
}

// mem.realloc(ptr, n) — resize an allocation
static Value mem_realloc(int argc, Value* args) {
    if (argc < 2 || !IS_NUMERIC(args[0]) || !IS_NUMERIC(args[1])) return val_nil();
    void* ptr = (void*)(uintptr_t)(unsigned long long)NUMERIC_AS_DOUBLE(args[0]);
    size_t n   = (size_t)NUMERIC_AS_INT(args[1]);
    void* new_ptr = realloc(ptr, n);
    if (!new_ptr) return val_nil();
    return val_number((double)(uintptr_t)new_ptr);
}

// mem.size_of(type_str) — size in bytes of a primitive type
static Value mem_size_of(int argc, Value* args) {
    if (argc < 1 || !IS_STRING(args[0])) return val_nil();
    const char* t = AS_STRING(args[0]);
    if      (strcmp(t, "i8")  == 0 || strcmp(t, "u8")  == 0 || strcmp(t, "byte") == 0) return val_int(1);
    else if (strcmp(t, "i16") == 0 || strcmp(t, "u16") == 0) return val_int(2);
    else if (strcmp(t, "i32") == 0 || strcmp(t, "u32") == 0 || strcmp(t, "f32") == 0) return val_int(4);
    else if (strcmp(t, "i64") == 0 || strcmp(t, "u64") == 0 || strcmp(t, "f64") == 0 ||
             strcmp(t, "int") == 0 || strcmp(t, "uint") == 0 || strcmp(t, "float") == 0) return val_int(8);
    else if (strcmp(t, "ptr") == 0) return val_int((int64_t)sizeof(void*));
    return val_nil();
}

// mem.write_u8(ptr, offset, val) — write a byte
static Value mem_write_u8(int argc, Value* args) {
    if (argc < 3 || !IS_NUMERIC(args[0]) || !IS_NUMERIC(args[1]) || !IS_NUMERIC(args[2])) return val_nil();
    uint8_t* ptr = (uint8_t*)(uintptr_t)(unsigned long long)NUMERIC_AS_DOUBLE(args[0]);
    int offset   = (int)NUMERIC_AS_INT(args[1]);
    ptr[offset]  = (uint8_t)NUMERIC_AS_DOUBLE(args[2]);
    return val_nil();
}

// mem.read_u8(ptr, offset) — read a byte
static Value mem_read_u8(int argc, Value* args) {
    if (argc < 2 || !IS_NUMERIC(args[0]) || !IS_NUMERIC(args[1])) return val_nil();
    uint8_t* ptr = (uint8_t*)(uintptr_t)(unsigned long long)NUMERIC_AS_DOUBLE(args[0]);
    int offset   = (int)NUMERIC_AS_INT(args[1]);
    return val_number((double)ptr[offset]);
}

// mem.write_u64(ptr, offset, val) — write 8 bytes
static Value mem_write_u64(int argc, Value* args) {
    if (argc < 3 || !IS_NUMERIC(args[0]) || !IS_NUMERIC(args[1]) || !IS_NUMERIC(args[2])) return val_nil();
    uint64_t* ptr = (uint64_t*)(uintptr_t)(unsigned long long)NUMERIC_AS_DOUBLE(args[0]);
    int offset    = (int)NUMERIC_AS_INT(args[1]);
    ptr[offset]   = (uint64_t)NUMERIC_AS_DOUBLE(args[2]);
    return val_nil();
}

// mem.read_u64(ptr, offset) — read 8 bytes
static Value mem_read_u64(int argc, Value* args) {
    if (argc < 2 || !IS_NUMERIC(args[0]) || !IS_NUMERIC(args[1])) return val_nil();
    uint64_t* ptr = (uint64_t*)(uintptr_t)(unsigned long long)NUMERIC_AS_DOUBLE(args[0]);
    int offset    = (int)NUMERIC_AS_INT(args[1]);
    return val_number((double)ptr[offset]);
}

// mem.copy(dst, src, n) — copy n bytes from src to dst
static Value mem_copy(int argc, Value* args) {
    if (argc < 3 || !IS_NUMERIC(args[0]) || !IS_NUMERIC(args[1]) || !IS_NUMERIC(args[2])) return val_nil();
    void* dst = (void*)(uintptr_t)(unsigned long long)NUMERIC_AS_DOUBLE(args[0]);
    void* src = (void*)(uintptr_t)(unsigned long long)NUMERIC_AS_DOUBLE(args[1]);
    size_t n  = (size_t)NUMERIC_AS_INT(args[2]);
    memcpy(dst, src, n);
    return val_nil();
}

// mem.zero(ptr, n) — zero n bytes at ptr
static Value mem_zero(int argc, Value* args) {
    if (argc < 2 || !IS_NUMERIC(args[0]) || !IS_NUMERIC(args[1])) return val_nil();
    void* ptr = (void*)(uintptr_t)(unsigned long long)NUMERIC_AS_DOUBLE(args[0]);
    size_t n  = (size_t)NUMERIC_AS_INT(args[1]);
    memset(ptr, 0, n);
    return val_nil();
}

Module* create_mem_module(ModuleCache* cache) {
    Module* m = create_native_module(cache, "mem");
    Environment* e = m->env;
    env_define_const(e, "alloc",    5, val_native(mem_alloc));
    env_define_const(e, "free",     4, val_native(mem_free));
    env_define_const(e, "realloc",  7, val_native(mem_realloc));
    env_define_const(e, "size_of",  7, val_native(mem_size_of));
    env_define_const(e, "write_u8", 8, val_native(mem_write_u8));
    env_define_const(e, "read_u8",  7, val_native(mem_read_u8));
    env_define_const(e, "write_u64",9, val_native(mem_write_u64));
    env_define_const(e, "read_u64", 8, val_native(mem_read_u64));
    env_define_const(e, "copy",     4, val_native(mem_copy));
    env_define_const(e, "zero",     4, val_native(mem_zero));
    return m;
}

// ============================================================================
// CHANNEL MODULE — Channel<T>.new(), send(), recv(), close(), len()
// Thread-safe FIFO queue. Used for spawn-block communication.
// Implemented with pthreads mutex + condition variables.
// ============================================================================

#include <pthread.h>

#define CHANNEL_CAPACITY_DEFAULT 256

typedef struct ChannelNode {
    Value value;
    struct ChannelNode* next;
} ChannelNode;

typedef struct {
    ChannelNode* head;
    ChannelNode* tail;
    int count;
    int capacity;     // 0 = unbounded
    int closed;
    pthread_mutex_t mu;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} ChannelData;

static void channel_data_free(void* ptr) {
    ChannelData* ch = (ChannelData*)ptr;
    pthread_mutex_destroy(&ch->mu);
    pthread_cond_destroy(&ch->not_empty);
    pthread_cond_destroy(&ch->not_full);
    ChannelNode* n = ch->head;
    while (n) { ChannelNode* nx = n->next; free(n); n = nx; }
    free(ch);
}

static Value make_channel(int capacity) {
    ChannelData* ch = malloc(sizeof(ChannelData));
    ch->head = ch->tail = NULL;
    ch->count = 0;
    ch->capacity = capacity;
    ch->closed = 0;
    pthread_mutex_init(&ch->mu, NULL);
    pthread_cond_init(&ch->not_empty, NULL);
    pthread_cond_init(&ch->not_full, NULL);
    // Wrap in a dict with a special type tag
    gc_pin();
    Value d = val_dict();
    dict_set(&d, "__type",    val_string("channel"));
    dict_set(&d, "__ptr",     val_number((double)(uintptr_t)ch));
    dict_set(&d, "__cap",     val_number((double)capacity));
    gc_unpin();
    return d;
}

static ChannelData* get_channel(Value* v) {
    if (!IS_DICT(*v)) return NULL;
    Value tag = dict_get(v, "__type");
    if (!IS_STRING(tag) || strcmp(AS_STRING(tag), "channel") != 0) return NULL;
    Value ptr = dict_get(v, "__ptr");
    if (!IS_NUMERIC(ptr)) return NULL;
    return (ChannelData*)(uintptr_t)(unsigned long long)NUMERIC_AS_DOUBLE(ptr);
}

// Channel.new(capacity = 0) -> channel
static Value channel_new_native(int argc, Value* args) {
    int cap = 0;
    if (argc >= 1 && IS_NUMERIC(args[0])) cap = (int)NUMERIC_AS_INT(args[0]);
    return make_channel(cap);
}

// channel.send(ch, value) -> bool
static Value channel_send_native(int argc, Value* args) {
    if (argc < 2) return val_bool(0);
    ChannelData* ch = get_channel(&args[0]);
    if (!ch) return val_bool(0);

    pthread_mutex_lock(&ch->mu);
    if (ch->closed) { pthread_mutex_unlock(&ch->mu); return val_bool(0); }
    // Block if bounded and full
    while (ch->capacity > 0 && ch->count >= ch->capacity && !ch->closed) {
        pthread_cond_wait(&ch->not_full, &ch->mu);
    }
    if (ch->closed) { pthread_mutex_unlock(&ch->mu); return val_bool(0); }

    ChannelNode* node = malloc(sizeof(ChannelNode));
    node->value = args[1];
    node->next = NULL;
    if (ch->tail) ch->tail->next = node;
    else          ch->head = node;
    ch->tail = node;
    ch->count++;
    pthread_cond_signal(&ch->not_empty);
    pthread_mutex_unlock(&ch->mu);
    return val_bool(1);
}

// channel.recv(ch) -> value | nil (nil if closed and empty)
static Value channel_recv_native(int argc, Value* args) {
    if (argc < 1) return val_nil();
    ChannelData* ch = get_channel(&args[0]);
    if (!ch) return val_nil();

    pthread_mutex_lock(&ch->mu);
    while (ch->count == 0 && !ch->closed) {
        pthread_cond_wait(&ch->not_empty, &ch->mu);
    }
    if (ch->count == 0) { pthread_mutex_unlock(&ch->mu); return val_nil(); }

    ChannelNode* node = ch->head;
    Value val = node->value;
    ch->head = node->next;
    if (!ch->head) ch->tail = NULL;
    ch->count--;
    free(node);
    pthread_cond_signal(&ch->not_full);
    pthread_mutex_unlock(&ch->mu);
    return val;
}

// channel.try_recv(ch) -> Some(value) | None (non-blocking)
static Value channel_try_recv_native(int argc, Value* args) {
    if (argc < 1) return val_nil();
    ChannelData* ch = get_channel(&args[0]);
    if (!ch) return val_nil();

    pthread_mutex_lock(&ch->mu);
    if (ch->count == 0) { pthread_mutex_unlock(&ch->mu); return val_nil(); }

    ChannelNode* node = ch->head;
    Value val = node->value;
    ch->head = node->next;
    if (!ch->head) ch->tail = NULL;
    ch->count--;
    free(node);
    pthread_cond_signal(&ch->not_full);
    pthread_mutex_unlock(&ch->mu);

    // Return Some(val) as a tagged dict
    gc_pin();
    Value some = val_dict();
    dict_set(&some, "__type", val_string("option.some"));
    dict_set(&some, "__val",  val);
    gc_unpin();
    return some;
}

// channel.close(ch)
static Value channel_close_native(int argc, Value* args) {
    if (argc < 1) return val_nil();
    ChannelData* ch = get_channel(&args[0]);
    if (!ch) return val_nil();
    pthread_mutex_lock(&ch->mu);
    ch->closed = 1;
    pthread_cond_broadcast(&ch->not_empty);
    pthread_cond_broadcast(&ch->not_full);
    pthread_mutex_unlock(&ch->mu);
    return val_nil();
}

// channel.len(ch) -> int
static Value channel_len_native(int argc, Value* args) {
    if (argc < 1) return val_int(0);
    ChannelData* ch = get_channel(&args[0]);
    if (!ch) return val_int(0);
    pthread_mutex_lock(&ch->mu);
    int n = ch->count;
    pthread_mutex_unlock(&ch->mu);
    return val_number((double)n);
}

// channel.is_closed(ch) -> bool
static Value channel_is_closed_native(int argc, Value* args) {
    if (argc < 1) return val_bool(1);
    ChannelData* ch = get_channel(&args[0]);
    if (!ch) return val_bool(1);
    pthread_mutex_lock(&ch->mu);
    int closed = ch->closed;
    pthread_mutex_unlock(&ch->mu);
    return val_bool(closed);
}

Module* create_channel_module(ModuleCache* cache) {
    Module* m = create_native_module(cache, "channel");
    Environment* e = m->env;
    env_define_const(e, "new",       3, val_native(channel_new_native));
    env_define_const(e, "send",      4, val_native(channel_send_native));
    env_define_const(e, "recv",      4, val_native(channel_recv_native));
    env_define_const(e, "try_recv",  8, val_native(channel_try_recv_native));
    env_define_const(e, "close",     5, val_native(channel_close_native));
    env_define_const(e, "len",       3, val_native(channel_len_native));
    env_define_const(e, "is_closed", 9, val_native(channel_is_closed_native));
    return m;
}

// ============================================================================
// ATOMIC MODULE — atomic.new(n), atomic.add(a,n), atomic.load(a), atomic.cas(a,exp,new)
// ============================================================================

#include <stdatomic.h>

typedef struct { _Atomic long long value; } AtomicData;

static Value atomic_new_native(int argc, Value* args) {
    AtomicData* a = malloc(sizeof(AtomicData));
    atomic_store(&a->value, (long long)(argc > 0 && IS_NUMERIC(args[0]) ? NUMERIC_AS_DOUBLE(args[0]) : 0));
    gc_pin();
    Value d = val_dict();
    dict_set(&d, "__type", val_string("atomic"));
    dict_set(&d, "__ptr",  val_number((double)(uintptr_t)a));
    gc_unpin();
    return d;
}

static AtomicData* get_atomic(Value* v) {
    if (!IS_DICT(*v)) return NULL;
    Value tag = dict_get(v, "__type");
    if (!IS_STRING(tag) || strcmp(AS_STRING(tag), "atomic") != 0) return NULL;
    Value ptr = dict_get(v, "__ptr");
    return IS_NUMERIC(ptr) ? (AtomicData*)(uintptr_t)(unsigned long long)NUMERIC_AS_DOUBLE(ptr) : NULL;
}

static Value atomic_load_native(int argc, Value* args) {
    if (argc < 1) return val_int(0);
    AtomicData* a = get_atomic(&args[0]);
    return a ? val_number((double)atomic_load(&a->value)) : val_int(0);
}

static Value atomic_store_native(int argc, Value* args) {
    if (argc < 2 || !IS_NUMERIC(args[1])) return val_nil();
    AtomicData* a = get_atomic(&args[0]);
    if (a) atomic_store(&a->value, (long long)NUMERIC_AS_DOUBLE(args[1]));
    return val_nil();
}

static Value atomic_add_native(int argc, Value* args) {
    if (argc < 2 || !IS_NUMERIC(args[1])) return val_int(0);
    AtomicData* a = get_atomic(&args[0]);
    if (!a) return val_int(0);
    long long prev = atomic_fetch_add(&a->value, (long long)NUMERIC_AS_DOUBLE(args[1]));
    return val_number((double)prev);
}

static Value atomic_sub_native(int argc, Value* args) {
    if (argc < 2 || !IS_NUMERIC(args[1])) return val_int(0);
    AtomicData* a = get_atomic(&args[0]);
    if (!a) return val_int(0);
    long long prev = atomic_fetch_sub(&a->value, (long long)NUMERIC_AS_DOUBLE(args[1]));
    return val_number((double)prev);
}

static Value atomic_cas_native(int argc, Value* args) {
    if (argc < 3) return val_bool(0);
    AtomicData* a = get_atomic(&args[0]);
    if (!a) return val_bool(0);
    long long expected = (long long)NUMERIC_AS_DOUBLE(args[1]);
    long long desired  = (long long)NUMERIC_AS_DOUBLE(args[2]);
    int ok = atomic_compare_exchange_strong(&a->value, &expected, desired);
    return val_bool(ok);
}

Module* create_atomic_module(ModuleCache* cache) {
    Module* m = create_native_module(cache, "atomic");
    Environment* e = m->env;
    env_define_const(e, "new",   3, val_native(atomic_new_native));
    env_define_const(e, "load",  4, val_native(atomic_load_native));
    env_define_const(e, "store", 5, val_native(atomic_store_native));
    env_define_const(e, "add",   3, val_native(atomic_add_native));
    env_define_const(e, "sub",   3, val_native(atomic_sub_native));
    env_define_const(e, "cas",   3, val_native(atomic_cas_native));
    return m;
}
