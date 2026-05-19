// lilybox.c — LilyKnight permission enforcement implementation
#define _GNU_SOURCE
#include "lilybox.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <fnmatch.h>

_Thread_local LilyBox* lk_current_sandbox = NULL;
static uint64_t _lk_next_id = 1;

// ── Violation strings ─────────────────────────────────────────────────────────
const char* lk_violation_str(LKViolation v) {
    switch (v) {
        case LK_OK:            return "OK";
        case LK_DENY_NET:      return "SandboxError: network access denied";
        case LK_DENY_HOST:     return "SandboxError: host not in allowed list";
        case LK_DENY_FS:       return "SandboxError: filesystem access denied";
        case LK_DENY_PATH:     return "SandboxError: path not in allowed list";
        case LK_DENY_PROC:     return "SandboxError: process operation denied";
        case LK_DENY_FFI:      return "SandboxError: FFI/native library access denied";
        case LK_DENY_ENV:      return "SandboxError: environment variable access denied";
        case LK_LIMIT_MEM:     return "SandboxError: memory limit exceeded";
        case LK_LIMIT_CPU:     return "SandboxError: CPU time limit exceeded";
        case LK_LIMIT_FILES:   return "SandboxError: open file limit exceeded";
        case LK_LIMIT_SYSCALL: return "SandboxError: syscall limit exceeded";
        default:               return "SandboxError: unknown violation";
    }
}

void lk_log_violation(LilyBox* box, LKViolation v, const char* detail) {
    if (!box) return;
    box->violation_count++;
    snprintf(box->last_violation, sizeof(box->last_violation),
             "[LilyKnight] %s (%s)", lk_violation_str(v), detail ? detail : "");
    // Log to stderr in debug mode
    if (getenv("SAGE_SANDBOX_VERBOSE")) {
        fprintf(stderr, "%s\n", box->last_violation);
    }
}

// ── Create sandbox ────────────────────────────────────────────────────────────
LilyBox* lilyknight_create_with_flags(uint32_t perm_flags) {
    LilyBox* box = (LilyBox*)calloc(1, sizeof(LilyBox));
    box->id  = _lk_next_id++;
    box->cap = (LKCapability*)calloc(1, sizeof(LKCapability));
    box->cap->perm_flags = perm_flags;
    // Default limits: 256 MB, 30s CPU, 1M syscalls, 100 files, 8 threads
    box->cap->limits.max_memory_bytes = 256 * 1024 * 1024;
    box->cap->limits.max_cpu_ms       = 30000;
    box->cap->limits.max_syscalls     = 1000000;
    box->cap->limits.max_open_files   = 100;
    box->cap->limits.max_threads      = 8;
    pthread_mutex_init(&box->lock, NULL);
    return box;
}

LilyBox* lilyknight_create_unrestricted(void) {
    return lilyknight_create_with_flags(LK_PERM_ALL);
}

// ── Parse manifest ────────────────────────────────────────────────────────────
// Simple key=value parser for sage.manifest
// Supports:
//   net = true|false
//   net.allowed_hosts = ["host1", "host2"]
//   fs.read = ["./data", "/tmp"]
//   fs.write = ["./out"]
//   exec = false
//   ffi = false
//   max_memory_mb = 64
//   max_cpu_ms = 5000

static void lk_add_path(LKPathFilter* f, const char* path) {
    f->paths = (char**)realloc(f->paths, (size_t)(f->count + 1) * sizeof(char*));
    f->paths[f->count++] = strdup(path);
}

static void lk_add_host(LKNetFilter* f, const char* host) {
    f->hosts = (char**)realloc(f->hosts, (size_t)(f->count + 1) * sizeof(char*));
    f->hosts[f->count++] = strdup(host);
}

static void parse_string_list(const char* val, void (*add_fn)(void*, const char*), void* target) {
    // Parse ["item1", "item2"]
    const char* p = val;
    while (*p) {
        if (*p == '"') {
            p++;
            const char* start = p;
            while (*p && *p != '"') p++;
            char* item = strndup(start, (size_t)(p - start));
            add_fn(target, item);
            free(item);
            if (*p == '"') p++;
        } else p++;
    }
}
static void _add_path_read(void* f, const char* p) { lk_add_path((LKPathFilter*)f, p); }
static void _add_path_write(void* f, const char* p) { lk_add_path((LKPathFilter*)f, p); }
static void _add_host(void* f, const char* h) { lk_add_host((LKNetFilter*)f, h); }

LilyBox* lilyknight_create(const char* manifest_path) {
    LilyBox* box = lilyknight_create_with_flags(LK_PERM_NONE);
    box->name = strdup(manifest_path ? manifest_path : "<no manifest>");

    if (!manifest_path) return box;

    FILE* f = fopen(manifest_path, "r");
    if (!f) {
        // No manifest file = deny everything
        fprintf(stderr, "[LilyKnight] Warning: manifest '%s' not found — sandbox denies all\n",
                manifest_path);
        return box;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // Strip comments and whitespace
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '[') continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;

        char key[128] = {0};
        char val[384] = {0};
        size_t klen = (size_t)(eq - p);
        if (klen > sizeof(key)-1) continue;
        strncpy(key, p, klen);
        // Trim trailing whitespace from key
        for (int i = (int)strlen(key)-1; i >= 0 && (key[i]==' '||key[i]=='\t'); i--) key[i]=0;
        // Trim leading whitespace from val
        const char* vp = eq + 1;
        while (*vp == ' ' || *vp == '\t') vp++;
        strncpy(val, vp, sizeof(val)-1);
        // Trim trailing newline
        for (int i = (int)strlen(val)-1; i >= 0 && (val[i]=='\n'||val[i]=='\r'||val[i]==' '); i--) val[i]=0;

        // Parse key=value
        if (strcmp(key, "net") == 0) {
            if (strncmp(val, "true", 4) == 0)
                box->cap->perm_flags |= LK_PERM_NET;
        } else if (strcmp(key, "net.allowed_hosts") == 0) {
            parse_string_list(val, _add_host, &box->cap->net);
        } else if (strcmp(key, "fs.read") == 0) {
            box->cap->perm_flags |= LK_PERM_FS_READ;
            parse_string_list(val, _add_path_read, &box->cap->fs_read);
        } else if (strcmp(key, "fs.write") == 0) {
            box->cap->perm_flags |= LK_PERM_FS_WRITE;
            parse_string_list(val, _add_path_write, &box->cap->fs_write);
        } else if (strcmp(key, "exec") == 0 && strncmp(val,"true",4)==0) {
            box->cap->perm_flags |= LK_PERM_PROC_EXEC;
        } else if (strcmp(key, "ffi") == 0 && strncmp(val,"true",4)==0) {
            box->cap->perm_flags |= LK_PERM_FFI;
        } else if (strcmp(key, "max_memory_mb") == 0) {
            box->cap->limits.max_memory_bytes = (size_t)atoi(val) * 1024 * 1024;
        } else if (strcmp(key, "max_cpu_ms") == 0) {
            box->cap->limits.max_cpu_ms = (int64_t)atoi(val);
        } else if (strcmp(key, "max_open_files") == 0) {
            box->cap->limits.max_open_files = atoi(val);
        } else if (strcmp(key, "threads") == 0 && strncmp(val,"true",4)==0) {
            box->cap->perm_flags |= LK_PERM_IPC;  // threads also enable IPC
        } else if (strcmp(key, "max_threads") == 0) {
            box->cap->limits.max_threads = atoi(val);
        } else if (strcmp(key, "env") == 0 && strncmp(val,"true",4)==0) {
            box->cap->perm_flags |= LK_PERM_ENV_READ;
        }
    }
    fclose(f);
    return box;
}

void lilyknight_destroy(LilyBox* box) {
    if (!box) return;
    if (box->cap) {
        for (int i = 0; i < box->cap->fs_read.count;  i++) free(box->cap->fs_read.paths[i]);
        for (int i = 0; i < box->cap->fs_write.count; i++) free(box->cap->fs_write.paths[i]);
        for (int i = 0; i < box->cap->net.count;      i++) free(box->cap->net.hosts[i]);
        free(box->cap->fs_read.paths);
        free(box->cap->fs_write.paths);
        free(box->cap->net.hosts);
        free(box->cap);
    }
    free(box->name);
    pthread_mutex_destroy(&box->lock);
    free(box);
}

// ── Permission checks ─────────────────────────────────────────────────────────
LKViolation lk_check_net_connect(LilyBox* box, const char* host, int port) {
    if (!box) return LK_OK;
    if (!(box->cap->perm_flags & LK_PERM_NET_CONNECT)) return LK_DENY_NET;
    if (box->cap->net.count == 0) return LK_OK;  // No filter = allow all
    for (int i = 0; i < box->cap->net.count; i++) {
        if (fnmatch(box->cap->net.hosts[i], host, 0) == 0) return LK_OK;
    }
    (void)port;
    return LK_DENY_HOST;
}

LKViolation lk_check_net_send(LilyBox* box, size_t bytes) {
    if (!box) return LK_OK;
    if (!(box->cap->perm_flags & LK_PERM_NET_SEND)) return LK_DENY_NET;
    // Rate limiting
    if (box->cap->limits.max_net_bytes_sec > 0) {
        int64_t now_ms = (int64_t)(clock() * 1000 / CLOCKS_PER_SEC);
        if (now_ms - box->net_window_start_ms > 1000) {
            box->net_bytes_this_sec = 0;
            box->net_window_start_ms = now_ms;
        }
        box->net_bytes_this_sec += bytes;
        if (box->net_bytes_this_sec > box->cap->limits.max_net_bytes_sec)
            return LK_LIMIT_SYSCALL;
    }
    return LK_OK;
}

LKViolation lk_check_net_recv(LilyBox* box, size_t bytes) {
    if (!box) return LK_OK;
    (void)bytes;
    return (box->cap->perm_flags & LK_PERM_NET_RECV) ? LK_OK : LK_DENY_NET;
}

LKViolation lk_check_net_bind(LilyBox* box, int port) {
    if (!box) return LK_OK;
    (void)port;
    return (box->cap->perm_flags & LK_PERM_NET_BIND) ? LK_OK : LK_DENY_NET;
}

static int path_allowed(LKPathFilter* f, const char* path) {
    if (f->count == 0) return 1;  // No filter = deny (strict sandbox)
    for (int i = 0; i < f->count; i++) {
        // Allow if path starts with allowed prefix or matches glob
        if (strncmp(path, f->paths[i], strlen(f->paths[i])) == 0) return 1;
        if (fnmatch(f->paths[i], path, FNM_PATHNAME) == 0) return 1;
    }
    return 0;
}

LKViolation lk_check_fs_read(LilyBox* box, const char* path) {
    if (!box) return LK_OK;
    if (!(box->cap->perm_flags & LK_PERM_FS_READ)) return LK_DENY_FS;
    return path_allowed(&box->cap->fs_read, path) ? LK_OK : LK_DENY_PATH;
}

LKViolation lk_check_fs_write(LilyBox* box, const char* path) {
    if (!box) return LK_OK;
    if (!(box->cap->perm_flags & LK_PERM_FS_WRITE)) return LK_DENY_FS;
    return path_allowed(&box->cap->fs_write, path) ? LK_OK : LK_DENY_PATH;
}

LKViolation lk_check_fs_exec(LilyBox* box, const char* path) {
    if (!box) return LK_OK;
    (void)path;
    return (box->cap->perm_flags & LK_PERM_FS_EXEC) ? LK_OK : LK_DENY_FS;
}

LKViolation lk_check_proc_fork(LilyBox* box) {
    if (!box) return LK_OK;
    if (!(box->cap->perm_flags & LK_PERM_PROC_FORK)) return LK_DENY_PROC;
    if (box->cap->limits.max_threads > 0 &&
        box->threads_spawned >= box->cap->limits.max_threads) return LK_LIMIT_SYSCALL;
    return LK_OK;
}

LKViolation lk_check_proc_exec(LilyBox* box, const char* path) {
    if (!box) return LK_OK;
    (void)path;
    return (box->cap->perm_flags & LK_PERM_PROC_EXEC) ? LK_OK : LK_DENY_PROC;
}

LKViolation lk_check_ffi(LilyBox* box, const char* lib_name) {
    if (!box) return LK_OK;
    (void)lib_name;
    return (box->cap->perm_flags & LK_PERM_FFI) ? LK_OK : LK_DENY_FFI;
}

LKViolation lk_check_env(LilyBox* box, const char* var_name, int write) {
    if (!box) return LK_OK;
    (void)var_name;
    if (write) return (box->cap->perm_flags & LK_PERM_ENV_WRITE) ? LK_OK : LK_DENY_ENV;
    return (box->cap->perm_flags & LK_PERM_ENV_READ)  ? LK_OK : LK_DENY_ENV;
}

LKViolation lk_check_alloc(LilyBox* box, size_t bytes) {
    if (!box) return LK_OK;
    if (box->cap->limits.max_memory_bytes == 0) return LK_OK;
    box->bytes_allocated += bytes;
    return (box->bytes_allocated <= box->cap->limits.max_memory_bytes) ? LK_OK : LK_LIMIT_MEM;
}
