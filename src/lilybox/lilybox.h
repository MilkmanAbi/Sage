// lilybox.h — LilyBox Sandboxed Execution + LilyKnight Permission System
//
// LilyBox: sandbox mode for MossVM. Code runs in a contained environment
//   with explicit permission grants. Safe by default, opt-in for power.
//
// LilyKnight: the permission enforcement layer inside LilyBox.
//   Enforces at bytecode dispatch level (not syscall — fast, inspectable).
//
//   Permission namespaces:
//     net.*        — network access (send/recv/bind/connect/dns)
//     fs.*         — file system access (read/write/exec per path)
//     proc.*       — process operations (fork/exec/signal)
//     ipc.*        — IPC (shmem/pipes/sockets)
//     gc.*         — GC control (pause/step/mode-change)
//     ffi.*        — foreign function calls (dlopen/dlsym)
//     env.*        — environment variable access
//     time.*       — clock access (can be restricted for determinism)
//
//   Each permission has:
//     - A boolean allow/deny
//     - An optional filter (e.g. net.allowed_hosts = ["api.example.com"])
//     - A rate limit (e.g. net.max_bytes_per_sec = 1048576)
//     - A resource cap (e.g. fs.max_open_files = 10)
//
// LilyKnight Capabilities (Rust-inspired, not Linux caps):
//   A capability token is an unforgeable handle granting specific access.
//   To call fs.open(), you need a FS capability. To send on a socket,
//   you need a NET capability. Capabilities are created at sandbox init
//   and passed explicitly — code can't manufacture them.
//
// Usage:
//   LilyBox* box = lilyknight_create_sandbox("plugin.manifest");
//   LilyResult res = lilyknight_exec(box, "plugin.sagec", args, arg_count);
//   lilyknight_destroy(box);
//
// sage.manifest (TOML-like):
//   [sandbox]
//   net = true
//   net.allowed_hosts = ["api.example.com"]
//   net.max_connections = 4
//   fs.read = ["./data", "/tmp"]
//   fs.write = ["./output"]
//   exec = false
//   ffi = false
//   max_memory_mb = 64
//   max_cpu_ms = 5000
//   max_syscalls = 100000
//
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

// ── Permission flags ──────────────────────────────────────────────────────────
#define LK_PERM_NONE         0x00000000
#define LK_PERM_NET_SEND     0x00000001
#define LK_PERM_NET_RECV     0x00000002
#define LK_PERM_NET_BIND     0x00000004
#define LK_PERM_NET_CONNECT  0x00000008
#define LK_PERM_NET_DNS      0x00000010
#define LK_PERM_NET          (LK_PERM_NET_SEND|LK_PERM_NET_RECV|LK_PERM_NET_BIND|LK_PERM_NET_CONNECT|LK_PERM_NET_DNS)
#define LK_PERM_FS_READ      0x00000100
#define LK_PERM_FS_WRITE     0x00000200
#define LK_PERM_FS_EXEC      0x00000400
#define LK_PERM_FS           (LK_PERM_FS_READ|LK_PERM_FS_WRITE|LK_PERM_FS_EXEC)
#define LK_PERM_PROC_FORK    0x00001000
#define LK_PERM_PROC_EXEC    0x00002000
#define LK_PERM_PROC_SIGNAL  0x00004000
#define LK_PERM_PROC         (LK_PERM_PROC_FORK|LK_PERM_PROC_EXEC|LK_PERM_PROC_SIGNAL)
#define LK_PERM_IPC          0x00010000
#define LK_PERM_GC_CONTROL   0x00020000
#define LK_PERM_FFI          0x00040000
#define LK_PERM_ENV_READ     0x00080000
#define LK_PERM_ENV_WRITE    0x00100000
#define LK_PERM_TIME         0x00200000
#define LK_PERM_ALL          0xFFFFFFFF  // Unrestricted (no sandbox)

// ── Path filter ───────────────────────────────────────────────────────────────
typedef struct {
    char**  paths;
    int     count;
} LKPathFilter;

// ── Host filter ───────────────────────────────────────────────────────────────
typedef struct {
    char**  hosts;
    int     count;
    int     allowed_ports[16];
    int     port_count;
} LKNetFilter;

// ── Resource limits ───────────────────────────────────────────────────────────
typedef struct {
    size_t  max_memory_bytes;    // 0 = unlimited
    int64_t max_cpu_ms;          // 0 = unlimited
    int64_t max_syscalls;        // 0 = unlimited
    int     max_open_files;      // 0 = unlimited
    int     max_threads;         // 0 = unlimited
    size_t  max_net_bytes_sec;   // 0 = unlimited
} LKLimits;

// ── Capability token ──────────────────────────────────────────────────────────
// Unforgeable: allocated by lilyknight_create_sandbox, not user-constructable.
typedef struct LKCapability {
    uint32_t             perm_flags;   // Which permissions this cap grants
    LKPathFilter         fs_read;
    LKPathFilter         fs_write;
    LKNetFilter          net;
    LKLimits             limits;
    struct LKCapability* next;         // Capability list
} LKCapability;

// ── Sandbox violation result ──────────────────────────────────────────────────
typedef enum {
    LK_OK            = 0,
    LK_DENY_NET      = 1,
    LK_DENY_HOST     = 2,
    LK_DENY_FS       = 3,
    LK_DENY_PATH     = 4,
    LK_DENY_PROC     = 5,
    LK_DENY_FFI      = 6,
    LK_DENY_ENV      = 7,
    LK_LIMIT_MEM     = 8,
    LK_LIMIT_CPU     = 9,
    LK_LIMIT_FILES   = 10,
    LK_LIMIT_SYSCALL = 11,
} LKViolation;

// ── Sandbox (LilyBox instance) ────────────────────────────────────────────────
typedef struct LilyBox {
    LKCapability*   cap;            // Active capability
    uint64_t        id;             // Unique sandbox ID
    char*           name;           // Debug name / manifest path

    // Runtime accounting
    size_t          bytes_allocated;
    int64_t         cpu_ms_used;
    int64_t         syscalls_used;
    int             open_files;
    int             threads_spawned;
    size_t          net_bytes_this_sec;
    int64_t         net_window_start_ms;

    // Isolated heap pointer
    void*           heap;           // Isolated heap for this sandbox (future)

    // Violation log
    char            last_violation[256];
    int             violation_count;

    // Lock
    pthread_mutex_t lock;
} LilyBox;

// ── API ───────────────────────────────────────────────────────────────────────

// Parse a sage.manifest file and create a sandbox with appropriate capabilities
LilyBox* lilyknight_create(const char* manifest_path);

// Create with explicit permission flags (for programmatic use)
LilyBox* lilyknight_create_with_flags(uint32_t perm_flags);

// Create an unrestricted sandbox (same as no sandbox, but still tracked)
LilyBox* lilyknight_create_unrestricted(void);

// Destroy a sandbox and free all resources
void lilyknight_destroy(LilyBox* box);

// ── Permission checks (called at bytecode dispatch) ──────────────────────────
LKViolation lk_check_net_connect(LilyBox* box, const char* host, int port);
LKViolation lk_check_net_send   (LilyBox* box, size_t bytes);
LKViolation lk_check_net_recv   (LilyBox* box, size_t bytes);
LKViolation lk_check_net_bind   (LilyBox* box, int port);
LKViolation lk_check_fs_read    (LilyBox* box, const char* path);
LKViolation lk_check_fs_write   (LilyBox* box, const char* path);
LKViolation lk_check_fs_exec    (LilyBox* box, const char* path);
LKViolation lk_check_proc_fork  (LilyBox* box);
LKViolation lk_check_proc_exec  (LilyBox* box, const char* path);
LKViolation lk_check_ffi        (LilyBox* box, const char* lib_name);
LKViolation lk_check_env        (LilyBox* box, const char* var_name, int write);
LKViolation lk_check_alloc      (LilyBox* box, size_t bytes);

// Get a human-readable description of a violation
const char* lk_violation_str(LKViolation v);

// Format a full error message into the sandbox's violation log
void lk_log_violation(LilyBox* box, LKViolation v, const char* detail);

// ── Fast inline check (checks perm_flags before anything else) ────────────────
static inline LKViolation lk_check_perm(LilyBox* box, uint32_t needed) {
    if (__builtin_expect(box == NULL, 1)) return LK_OK;       // No sandbox = allow all
    if ((box->cap->perm_flags & needed) == needed) return LK_OK;
    // Determine which violation type to return
    if (needed & LK_PERM_NET)  return LK_DENY_NET;
    if (needed & LK_PERM_FS)   return LK_DENY_FS;
    if (needed & LK_PERM_PROC) return LK_DENY_PROC;
    if (needed & LK_PERM_FFI)  return LK_DENY_FFI;
    return LK_DENY_NET;
}

// ── Current sandbox (thread-local — set when executing sandboxed code) ────────
extern _Thread_local LilyBox* lk_current_sandbox;

// Convenience: check current thread's sandbox
#define LK_CHECK(perm_flag) \
    do { LKViolation _v = lk_check_perm(lk_current_sandbox, (perm_flag)); \
         if (_v != LK_OK) { lk_log_violation(lk_current_sandbox, _v, __func__); \
                            return EVAL_EXCEPTION(val_exception(lk_violation_str(_v))); } } while(0)

#define LK_CHECK_NET_CONNECT(host, port) \
    do { LKViolation _v = lk_check_net_connect(lk_current_sandbox, host, port); \
         if (_v != LK_OK) { lk_log_violation(lk_current_sandbox, _v, host); \
                            return EVAL_EXCEPTION(val_exception(lk_violation_str(_v))); } } while(0)

#define LK_CHECK_FS_READ(path) \
    do { LKViolation _v = lk_check_fs_read(lk_current_sandbox, path); \
         if (_v != LK_OK) { lk_log_violation(lk_current_sandbox, _v, path); \
                            return EVAL_EXCEPTION(val_exception(lk_violation_str(_v))); } } while(0)

#define LK_CHECK_FS_WRITE(path) \
    do { LKViolation _v = lk_check_fs_write(lk_current_sandbox, path); \
         if (_v != LK_OK) { lk_log_violation(lk_current_sandbox, _v, path); \
                            return EVAL_EXCEPTION(val_exception(lk_violation_str(_v))); } } while(0)
