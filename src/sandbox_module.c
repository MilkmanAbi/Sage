// sandbox_module.c — Exposes LilyKnight to Sage as `import sandbox`
//
// sandbox.create(manifest_path) -> sandbox_handle
// sandbox.create_flags(flags...) -> sandbox_handle
// sandbox.enter(handle)          -> bool
// sandbox.exit()
// sandbox.is_sandboxed()         -> bool
// sandbox.allow(handle, perm)    -> bool
// sandbox.check(handle, perm)    -> bool
// sandbox.stats(handle)          -> dict
// sandbox.last_violation()       -> str | nil
//
// Permissions as string constants:
//   sandbox.NET, sandbox.FS_READ, sandbox.FS_WRITE, sandbox.PROC, sandbox.FFI
//
// Usage:
//   import sandbox
//   let box = sandbox.create("plugin.manifest")
//   sandbox.enter(box)
//   # ... run sandboxed code ...
//   sandbox.exit()

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include "value.h"
#include "module.h"
#include "lilybox.h"

// ── Box handle value ──────────────────────────────────────────────────────────
static Value make_box_val(LilyBox* box) {
    gc_pin();
    Value d = val_dict();
    char ptr_str[32];
    snprintf(ptr_str, sizeof(ptr_str), "%p", (void*)box);
    dict_set(&d, "__type", val_string("sandbox"));
    dict_set(&d, "__ptr",  val_string(ptr_str));
    dict_set(&d, "__id",   val_number((double)box->id));
    gc_unpin();
    return d;
}

static LilyBox* get_box(Value* v) {
    if (!IS_DICT(*v)) return NULL;
    Value t = dict_get(v, "__type");
    if (!IS_STRING(t) || strcmp(AS_STRING(t), "sandbox") != 0) return NULL;
    Value ptr = dict_get(v, "__ptr");
    if (!IS_STRING(ptr)) return NULL;
    void* p = NULL;
    sscanf(AS_STRING(ptr), "%p", &p);
    return (LilyBox*)p;
}

static uint32_t parse_perm_string(const char* s) {
    if (!s) return 0;
    if (strcmp(s, "net")      == 0) return LK_PERM_NET;
    if (strcmp(s, "net.send") == 0) return LK_PERM_NET_SEND;
    if (strcmp(s, "net.recv") == 0) return LK_PERM_NET_RECV;
    if (strcmp(s, "net.bind") == 0) return LK_PERM_NET_BIND;
    if (strcmp(s, "net.connect")==0)return LK_PERM_NET_CONNECT;
    if (strcmp(s, "fs")       == 0) return LK_PERM_FS;
    if (strcmp(s, "fs.read")  == 0) return LK_PERM_FS_READ;
    if (strcmp(s, "fs.write") == 0) return LK_PERM_FS_WRITE;
    if (strcmp(s, "fs.exec")  == 0) return LK_PERM_FS_EXEC;
    if (strcmp(s, "proc")     == 0) return LK_PERM_PROC;
    if (strcmp(s, "ffi")      == 0) return LK_PERM_FFI;
    if (strcmp(s, "env")      == 0) return LK_PERM_ENV_READ | LK_PERM_ENV_WRITE;
    if (strcmp(s, "ipc")      == 0) return LK_PERM_IPC;
    if (strcmp(s, "all")      == 0) return LK_PERM_ALL;
    return 0;
}

// sandbox.create(manifest_path) -> sandbox_handle
static Value sandbox_create_native(int argc, Value* args) {
    const char* path = (argc >= 1 && IS_STRING(args[0])) ? AS_STRING(args[0]) : NULL;
    LilyBox* box = lilyknight_create(path);
    if (!box) return val_nil();
    return make_box_val(box);
}

// sandbox.create_restricted(perm_string...) -> handle
// e.g. sandbox.create_restricted("net", "fs.read") 
static Value sandbox_create_restricted_native(int argc, Value* args) {
    uint32_t flags = LK_PERM_NONE;
    for (int i = 0; i < argc; i++) {
        if (IS_STRING(args[i])) flags |= parse_perm_string(AS_STRING(args[i]));
    }
    LilyBox* box = lilyknight_create_with_flags(flags);
    return box ? make_box_val(box) : val_nil();
}

// sandbox.enter(handle) — set this thread's sandbox context
static Value sandbox_enter_native(int argc, Value* args) {
    if (argc < 1) return val_bool(0);
    LilyBox* box = get_box(&args[0]);
    if (!box) return val_bool(0);
    lk_current_sandbox = box;
    return val_bool(1);
}

// sandbox.exit() — remove sandbox context for this thread
static Value sandbox_exit_native(int argc, Value* args) {
    (void)argc; (void)args;
    lk_current_sandbox = NULL;
    return val_nil();
}

// sandbox.is_sandboxed() -> bool
static Value sandbox_is_sandboxed_native(int argc, Value* args) {
    (void)argc; (void)args;
    return val_bool(lk_current_sandbox != NULL);
}

// sandbox.check(handle, perm_string) -> bool
static Value sandbox_check_native(int argc, Value* args) {
    if (argc < 2) return val_bool(0);
    LilyBox* box = get_box(&args[0]);
    if (!box) return val_bool(0);
    if (!IS_STRING(args[1])) return val_bool(0);
    uint32_t perm = parse_perm_string(AS_STRING(args[1]));
    return val_bool((box->cap->perm_flags & perm) == perm);
}

// sandbox.stats(handle) -> dict
static Value sandbox_stats_native(int argc, Value* args) {
    gc_pin();
    Value d = val_dict();
    if (argc >= 1) {
        LilyBox* box = get_box(&args[0]);
        if (box) {
            dict_set(&d, "id",              val_number((double)box->id));
            dict_set(&d, "bytes_allocated", val_number((double)box->bytes_allocated));
            dict_set(&d, "violations",      val_number((double)box->violation_count));
            dict_set(&d, "last_violation",  val_string(box->last_violation));
            dict_set(&d, "perm_flags",      val_number((double)box->cap->perm_flags));
        }
    }
    gc_unpin();
    return d;
}

// sandbox.last_violation() -> str | nil
static Value sandbox_last_violation_native(int argc, Value* args) {
    (void)argc; (void)args;
    if (!lk_current_sandbox || lk_current_sandbox->violation_count == 0) return val_nil();
    return val_string(lk_current_sandbox->last_violation);
}

// sandbox.destroy(handle)
static Value sandbox_destroy_native(int argc, Value* args) {
    if (argc < 1) return val_nil();
    LilyBox* box = get_box(&args[0]);
    if (box) {
        if (lk_current_sandbox == box) lk_current_sandbox = NULL;
        lilyknight_destroy(box);
        // Invalidate the handle
        if (IS_DICT(args[0])) dict_set(&args[0], "__ptr", val_number(0));
    }
    return val_nil();
}

Module* create_sandbox_module(ModuleCache* cache) {
    Module* m = create_native_module(cache, "sandbox");
    Environment* e = m->env;
    env_define_const(e, "create",           6,  val_native(sandbox_create_native));
    env_define_const(e, "create_restricted",17, val_native(sandbox_create_restricted_native));
    env_define_const(e, "enter",            5,  val_native(sandbox_enter_native));
    env_define_const(e, "exit",             4,  val_native(sandbox_exit_native));
    env_define_const(e, "is_sandboxed",     12, val_native(sandbox_is_sandboxed_native));
    env_define_const(e, "check",            5,  val_native(sandbox_check_native));
    env_define_const(e, "stats",            5,  val_native(sandbox_stats_native));
    env_define_const(e, "last_violation",   14, val_native(sandbox_last_violation_native));
    env_define_const(e, "destroy",          7,  val_native(sandbox_destroy_native));
    // Permission string constants
    env_define_const(e, "NET",      3, val_string("net"));
    env_define_const(e, "FS_READ",  7, val_string("fs.read"));
    env_define_const(e, "FS_WRITE", 8, val_string("fs.write"));
    env_define_const(e, "PROC",     4, val_string("proc"));
    env_define_const(e, "FFI",      3, val_string("ffi"));
    env_define_const(e, "ALL",      3, val_string("all"));
    return m;
}
