// sage_sandbox_rt.h — Compiled LilyBox: Sandbox for Compiled Sage Binaries
//
// This is the compiled-binary equivalent of lilybox.h + LilyKnight.
// It is entirely opt-in. If your compiled program does not use @sandbox,
// this file is never linked. The sage_runtime.h stubs (SAGE_RT_NO_SANDBOX)
// compile away to zero cost.
//
// When @sandbox IS active, sage_sandbox_rt.c is compiled into the binary.
// The permission manifest is baked in at compile time as a static C struct.
// All I/O, net, exec, FFI, and env calls go through sage_rt_sb_check_*
// before proceeding.
//
// ── How it works ─────────────────────────────────────────────────────────────
//
//   1. Developer annotates their Sage program:
//
//        @sandbox(fs_read: ["/tmp", "./data"], net: false, ffi: false):
//            import fileio
//            let contents = fileio.read("data/config.txt")
//
//      Or via a .sageperm manifest file:
//
//        # myapp.sageperm
//        fs_read  = ["/tmp", "./data"]
//        fs_write = ["./output"]
//        net      = false
//        ffi      = false
//
//   2. sage --aot --sandbox myapp.sageperm myapp.sage
//      The compiler:
//        a. Parses the .sageperm (or @sandbox annotation)
//        b. Emits sage_perm_manifest.h into the build directory
//        c. Compiles myapp_gen.c + sage_runtime.c + sage_sandbox_rt.c
//        d. Links into myapp binary
//
//   3. At runtime, sage_rt_sb_init() is called from main() with the manifest.
//      All checks from that point are against the baked-in permissions.
//
// ── Manifest format (C struct) ────────────────────────────────────────────────
//
//   static const SagePermManifest SAGE_PERM_MANIFEST = {
//       .flags           = SAGEPERM_FS_READ | SAGEPERM_FS_WRITE,
//       .fs_read_paths   = {"/tmp", "./data"},
//       .fs_read_count   = 2,
//       .fs_write_paths  = {"./output"},
//       .fs_write_count  = 1,
//       .net_hosts       = {},
//       .net_host_count  = 0,
//       .max_memory_mb   = 64,
//       .max_open_files  = 20,
//   };
//
// ── Error output ─────────────────────────────────────────────────────────────
//
//   Violations print a Firefly-style message and abort:
//
//   -- error[E091]: sandbox permission denied (filesystem write) --
//   |
//   |  attempted path: /etc/passwd
//   |  allowed paths:  ./output
//   |
//   Firefly: This binary was compiled with restricted filesystem write access.
//            To allow this path, recompile with an updated .sageperm manifest.
//   -------------------------------------------------------------------------
//
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Permission flags (mirrors lilybox.h LK_PERM_* for interop)
// ─────────────────────────────────────────────────────────────────────────────

#define SAGEPERM_NONE         0x00000000u
#define SAGEPERM_NET_SEND     0x00000001u
#define SAGEPERM_NET_RECV     0x00000002u
#define SAGEPERM_NET_BIND     0x00000004u
#define SAGEPERM_NET_CONNECT  0x00000008u
#define SAGEPERM_NET_DNS      0x00000010u
#define SAGEPERM_NET          (SAGEPERM_NET_SEND|SAGEPERM_NET_RECV| \
                               SAGEPERM_NET_BIND|SAGEPERM_NET_CONNECT|SAGEPERM_NET_DNS)
#define SAGEPERM_FS_READ      0x00000100u
#define SAGEPERM_FS_WRITE     0x00000200u
#define SAGEPERM_FS_EXEC      0x00000400u
#define SAGEPERM_FS           (SAGEPERM_FS_READ|SAGEPERM_FS_WRITE|SAGEPERM_FS_EXEC)
#define SAGEPERM_PROC_FORK    0x00001000u
#define SAGEPERM_PROC_EXEC    0x00002000u
#define SAGEPERM_PROC         (SAGEPERM_PROC_FORK|SAGEPERM_PROC_EXEC)
#define SAGEPERM_IPC          0x00010000u
#define SAGEPERM_FFI          0x00040000u
#define SAGEPERM_ENV_READ     0x00080000u
#define SAGEPERM_ENV_WRITE    0x00100000u
#define SAGEPERM_TIME         0x00200000u
#define SAGEPERM_ALL          0xFFFFFFFFu  // unrestricted (no sandbox effect)

// ─────────────────────────────────────────────────────────────────────────────
// Permission manifest
//
// Baked into every sandboxed compiled binary as a static const.
// Arrays use fixed max sizes to keep it a plain C struct (no malloc at init).
// ─────────────────────────────────────────────────────────────────────────────

#define SAGEPERM_MAX_PATHS  32   // max fs path entries per direction
#define SAGEPERM_MAX_HOSTS  16   // max allowed net hosts
#define SAGEPERM_MAX_LIBS   16   // max allowed FFI libraries

typedef struct {
    uint32_t    flags;           // OR of SAGEPERM_* flags

    // Filesystem
    const char* fs_read_paths[SAGEPERM_MAX_PATHS];
    int         fs_read_count;
    const char* fs_write_paths[SAGEPERM_MAX_PATHS];
    int         fs_write_count;

    // Network
    const char* net_hosts[SAGEPERM_MAX_HOSTS];  // glob patterns, empty = all
    int         net_host_count;
    int         net_allowed_ports[16];
    int         net_port_count;                 // 0 = all ports

    // FFI
    const char* ffi_libs[SAGEPERM_MAX_LIBS];    // allowed library names/globs
    int         ffi_lib_count;                  // 0 = no FFI (unless SAGEPERM_FFI set with no filter)

    // Resource limits (0 = unlimited)
    size_t      max_memory_mb;
    int         max_open_files;
    int         max_threads;
    int64_t     max_cpu_ms;

    // Metadata (optional, for error messages)
    const char* program_name;    // shown in violation messages
    const char* manifest_source; // ".sageperm path" or "@sandbox annotation"
} SagePermManifest;

// ─────────────────────────────────────────────────────────────────────────────
// Violation types (mirrors LKViolation)
// ─────────────────────────────────────────────────────────────────────────────

typedef enum {
    SAGESB_OK            = 0,
    SAGESB_DENY_NET      = 1,
    SAGESB_DENY_HOST     = 2,
    SAGESB_DENY_FS_READ  = 3,
    SAGESB_DENY_FS_WRITE = 4,
    SAGESB_DENY_PATH     = 5,
    SAGESB_DENY_PROC     = 6,
    SAGESB_DENY_FFI      = 7,
    SAGESB_DENY_ENV      = 8,
    SAGESB_LIMIT_MEM     = 9,
    SAGESB_LIMIT_FILES   = 10,
    SAGESB_LIMIT_CPU     = 11,
} SageSBViolation;

// ─────────────────────────────────────────────────────────────────────────────
// Runtime state (one per process, initialised from manifest at startup)
// ─────────────────────────────────────────────────────────────────────────────

typedef struct {
    const SagePermManifest* manifest;  // points to the baked-in static struct
    int     active;                    // 1 = sandbox is enforcing
    size_t  bytes_allocated;
    int     open_files;
    int     threads;
    int64_t cpu_ms_used;
    int     violation_count;
    char    last_violation[512];
} SageSBState;

// Global sandbox state (one per process)
extern SageSBState sage_sb_state;

// ─────────────────────────────────────────────────────────────────────────────
// API
// ─────────────────────────────────────────────────────────────────────────────

// Called from main() to install the manifest. Idempotent.
// Pass NULL to disable sandbox entirely (same as not linking this file).
void sage_sb_init(const SagePermManifest* manifest);

// Tear down (called from sage_rt_shutdown)
void sage_sb_shutdown(void);

// ── Core permission checks ────────────────────────────────────────────────────
// All return SAGESB_OK if allowed, a violation code if denied.
// On denial, last_violation in SageSBState is also populated.

SageSBViolation sage_sb_check_fs_read (const char* path);
SageSBViolation sage_sb_check_fs_write(const char* path);
SageSBViolation sage_sb_check_net     (const char* host, int port);
SageSBViolation sage_sb_check_exec    (const char* cmd);
SageSBViolation sage_sb_check_ffi     (const char* lib);
SageSBViolation sage_sb_check_env     (const char* key);
SageSBViolation sage_sb_check_alloc   (size_t bytes);

// ── Violation handling ────────────────────────────────────────────────────────

// Human-readable violation name
const char* sage_sb_violation_str(SageSBViolation v);

// Error code string (maps to Firefly error registry E090-E097)
const char* sage_sb_violation_code(SageSBViolation v);

// Print a full Firefly-style violation message and abort.
void sage_sb_deny(SageSBViolation v, const char* detail) __attribute__((noreturn));

// ── Convenience macros ────────────────────────────────────────────────────────
// Used by sage_runtime.c when sandbox is active.
// These replace the no-op stubs declared in sage_runtime.h.

#define SAGE_SB_CHECK_FS_READ(path) \
    do { SageSBViolation _v = sage_sb_check_fs_read(path); \
         if (_v != SAGESB_OK) sage_sb_deny(_v, path); } while(0)

#define SAGE_SB_CHECK_FS_WRITE(path) \
    do { SageSBViolation _v = sage_sb_check_fs_write(path); \
         if (_v != SAGESB_OK) sage_sb_deny(_v, path); } while(0)

#define SAGE_SB_CHECK_NET(host, port) \
    do { SageSBViolation _v = sage_sb_check_net(host, port); \
         if (_v != SAGESB_OK) sage_sb_deny(_v, host); } while(0)

#define SAGE_SB_CHECK_EXEC(cmd) \
    do { SageSBViolation _v = sage_sb_check_exec(cmd); \
         if (_v != SAGESB_OK) sage_sb_deny(_v, cmd); } while(0)

#define SAGE_SB_CHECK_FFI(lib) \
    do { SageSBViolation _v = sage_sb_check_ffi(lib); \
         if (_v != SAGESB_OK) sage_sb_deny(_v, lib); } while(0)

#define SAGE_SB_CHECK_ENV(key) \
    do { SageSBViolation _v = sage_sb_check_env(key); \
         if (_v != SAGESB_OK) sage_sb_deny(_v, key); } while(0)

// ─────────────────────────────────────────────────────────────────────────────
// Manifest builder API
//
// Used by the compiler (aot.c) to generate manifest headers from
// @sandbox annotations or .sageperm files.
// ─────────────────────────────────────────────────────────────────────────────

// Parse a .sageperm file and populate a SagePermManifest.
// Returns 1 on success, 0 on parse failure.
int sage_sb_parse_manifest(const char* path, SagePermManifest* out);

// Emit a C header file containing the static const SagePermManifest.
// This gets #include'd into the generated binary's main.
// Returns 1 on success.
int sage_sb_emit_manifest_header(const SagePermManifest* manifest,
                                 const char* out_path);

// Produce a SagePermManifest for "allow everything" (no sandbox)
SagePermManifest sage_sb_manifest_unrestricted(void);

// Produce a SagePermManifest for "deny everything" (maximum sandbox)
SagePermManifest sage_sb_manifest_deny_all(void);

#ifdef __cplusplus
}
#endif
