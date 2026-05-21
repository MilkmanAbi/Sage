// sage_sandbox_rt.c — Compiled LilyBox: Permission Enforcement
//
// Linked into compiled Sage binaries only when @sandbox is active.
// Without this file in the link step, the sandbox has zero runtime cost.
//
// Implements:
//   - sage_rt_sb_check_* (satisfies the hooks declared in sage_runtime.h)
//   - Full Firefly-style violation messages (E090-E097)
//   - .sageperm manifest parser
//   - Manifest C-header emitter (used by aot.c at compile time)

#define _POSIX_C_SOURCE 200809L

#include "sage_sandbox_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fnmatch.h>
#include <time.h>
#include <inttypes.h>

// ─────────────────────────────────────────────────────────────────────────────
// Global state
// ─────────────────────────────────────────────────────────────────────────────

SageSBState sage_sb_state = {0};

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void sage_sb_init(const SagePermManifest* manifest) {
    if (!manifest) {
        sage_sb_state.active = 0;
        return;
    }
    sage_sb_state.manifest        = manifest;
    sage_sb_state.active          = 1;
    sage_sb_state.bytes_allocated = 0;
    sage_sb_state.open_files      = 0;
    sage_sb_state.threads         = 0;
    sage_sb_state.cpu_ms_used     = 0;
    sage_sb_state.violation_count = 0;
    sage_sb_state.last_violation[0] = '\0';
}

void sage_sb_shutdown(void) {
    sage_sb_state.active = 0;
    sage_sb_state.manifest = NULL;
}

// ─────────────────────────────────────────────────────────────────────────────
// Violation strings
// ─────────────────────────────────────────────────────────────────────────────

const char* sage_sb_violation_str(SageSBViolation v) {
    switch (v) {
        case SAGESB_OK:            return "ok";
        case SAGESB_DENY_NET:      return "network access denied";
        case SAGESB_DENY_HOST:     return "host not in allowed list";
        case SAGESB_DENY_FS_READ:  return "filesystem read denied";
        case SAGESB_DENY_FS_WRITE: return "filesystem write denied";
        case SAGESB_DENY_PATH:     return "path not in allowed list";
        case SAGESB_DENY_PROC:     return "process execution denied";
        case SAGESB_DENY_FFI:      return "FFI library access denied";
        case SAGESB_DENY_ENV:      return "environment variable access denied";
        case SAGESB_LIMIT_MEM:     return "memory limit exceeded";
        case SAGESB_LIMIT_FILES:   return "open file limit exceeded";
        case SAGESB_LIMIT_CPU:     return "CPU time limit exceeded";
        default:                   return "unknown violation";
    }
}

// Map violations to Firefly error codes (E090-E097)
const char* sage_sb_violation_code(SageSBViolation v) {
    switch (v) {
        case SAGESB_DENY_FS_READ:  return "E090";
        case SAGESB_DENY_FS_WRITE: return "E091";
        case SAGESB_DENY_NET:      return "E092";
        case SAGESB_DENY_HOST:     return "E092";
        case SAGESB_DENY_PROC:     return "E093";
        case SAGESB_DENY_FFI:      return "E094";
        case SAGESB_DENY_ENV:      return "E095";
        case SAGESB_LIMIT_MEM:
        case SAGESB_LIMIT_CPU:
        case SAGESB_LIMIT_FILES:   return "E097";
        default:                   return "E090";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Firefly-style violation output and abort
// ─────────────────────────────────────────────────────────────────────────────

void sage_sb_deny(SageSBViolation v, const char* detail) {
    const SagePermManifest* m = sage_sb_state.manifest;
    const char* prog = (m && m->program_name) ? m->program_name : "<binary>";
    const char* src  = (m && m->manifest_source) ? m->manifest_source : "compiled-in manifest";

    sage_sb_state.violation_count++;
    snprintf(sage_sb_state.last_violation, sizeof(sage_sb_state.last_violation),
             "[%s] %s: %s", sage_sb_violation_code(v),
             sage_sb_violation_str(v), detail ? detail : "");

    fprintf(stderr, "\n");
    fprintf(stderr, "-- error[%s]: sandbox permission denied (%s) -- %s --\n",
            sage_sb_violation_code(v), sage_sb_violation_str(v), prog);
    fprintf(stderr, "  |\n");

    if (detail && detail[0])
        fprintf(stderr, "  |  attempted: %s\n", detail);

    // Show what IS allowed for context
    if (v == SAGESB_DENY_FS_READ || v == SAGESB_DENY_PATH) {
        if (m && m->fs_read_count > 0) {
            fprintf(stderr, "  |  allowed read paths:\n");
            for (int i = 0; i < m->fs_read_count; i++)
                fprintf(stderr, "  |    %s\n", m->fs_read_paths[i]);
        } else {
            fprintf(stderr, "  |  allowed read paths: (none)\n");
        }
    } else if (v == SAGESB_DENY_FS_WRITE) {
        if (m && m->fs_write_count > 0) {
            fprintf(stderr, "  |  allowed write paths:\n");
            for (int i = 0; i < m->fs_write_count; i++)
                fprintf(stderr, "  |    %s\n", m->fs_write_paths[i]);
        } else {
            fprintf(stderr, "  |  allowed write paths: (none)\n");
        }
    } else if (v == SAGESB_DENY_NET || v == SAGESB_DENY_HOST) {
        if (m && m->net_host_count > 0) {
            fprintf(stderr, "  |  allowed hosts:\n");
            for (int i = 0; i < m->net_host_count; i++)
                fprintf(stderr, "  |    %s\n", m->net_hosts[i]);
        } else {
            fprintf(stderr, "  |  allowed hosts: (none)\n");
        }
    } else if (v == SAGESB_DENY_FFI) {
        if (m && m->ffi_lib_count > 0) {
            fprintf(stderr, "  |  allowed libraries:\n");
            for (int i = 0; i < m->ffi_lib_count; i++)
                fprintf(stderr, "  |    %s\n", m->ffi_libs[i]);
        } else {
            fprintf(stderr, "  |  FFI: disabled\n");
        }
    }

    fprintf(stderr, "  |\n");
    fprintf(stderr, "  Firefly: This binary was compiled with a restricted permission manifest.\n");
    fprintf(stderr, "           Manifest source: %s\n", src);
    fprintf(stderr, "           To allow this operation, update your .sageperm file\n");
    fprintf(stderr, "           and recompile: sage --aot --sandbox %s your_program.sage\n", src);
    fprintf(stderr, "------------------------------------------------------------------------\n\n");

    exit(1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Path matching helpers
// ─────────────────────────────────────────────────────────────────────────────

static int path_in_list(const char* const* paths, int count, const char* path) {
    if (!path) return 0;
    if (count == 0) return 1;  // empty list with flag set = allow all
    for (int i = 0; i < count; i++) {
        if (!paths[i]) continue;
        // Prefix match (directory subtree)
        size_t plen = strlen(paths[i]);
        if (strncmp(path, paths[i], plen) == 0 &&
            (path[plen] == '\0' || path[plen] == '/'))
            return 1;
        // Glob match
        if (fnmatch(paths[i], path, FNM_PATHNAME) == 0)
            return 1;
    }
    return 0;
}

static int host_in_list(const SagePermManifest* m, const char* host) {
    if (m->net_host_count == 0) return 1; // no filter = allow all hosts
    for (int i = 0; i < m->net_host_count; i++) {
        if (!m->net_hosts[i]) continue;
        if (strcmp(m->net_hosts[i], host) == 0)   return 1;
        if (fnmatch(m->net_hosts[i], host, 0) == 0) return 1;
    }
    return 0;
}

static int port_allowed(const SagePermManifest* m, int port) {
    if (m->net_port_count == 0) return 1; // no filter = all ports
    for (int i = 0; i < m->net_port_count; i++)
        if (m->net_allowed_ports[i] == port) return 1;
    return 0;
}

static int lib_in_list(const SagePermManifest* m, const char* lib) {
    if (m->ffi_lib_count == 0) return 1; // SAGEPERM_FFI set, no filter = all libs
    for (int i = 0; i < m->ffi_lib_count; i++) {
        if (!m->ffi_libs[i]) continue;
        if (strcmp(m->ffi_libs[i], lib) == 0)    return 1;
        if (fnmatch(m->ffi_libs[i], lib, 0) == 0) return 1;
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Permission checks
// ─────────────────────────────────────────────────────────────────────────────

SageSBViolation sage_sb_check_fs_read(const char* path) {
    if (!sage_sb_state.active) return SAGESB_OK;
    const SagePermManifest* m = sage_sb_state.manifest;
    if (!(m->flags & SAGEPERM_FS_READ)) return SAGESB_DENY_FS_READ;
    if (!path_in_list(m->fs_read_paths, m->fs_read_count, path))
        return SAGESB_DENY_PATH;
    return SAGESB_OK;
}

SageSBViolation sage_sb_check_fs_write(const char* path) {
    if (!sage_sb_state.active) return SAGESB_OK;
    const SagePermManifest* m = sage_sb_state.manifest;
    if (!(m->flags & SAGEPERM_FS_WRITE)) return SAGESB_DENY_FS_WRITE;
    if (!path_in_list(m->fs_write_paths, m->fs_write_count, path))
        return SAGESB_DENY_PATH;
    return SAGESB_OK;
}

SageSBViolation sage_sb_check_net(const char* host, int port) {
    if (!sage_sb_state.active) return SAGESB_OK;
    const SagePermManifest* m = sage_sb_state.manifest;
    if (!(m->flags & SAGEPERM_NET)) return SAGESB_DENY_NET;
    if (!host_in_list(m, host))     return SAGESB_DENY_HOST;
    if (!port_allowed(m, port))     return SAGESB_DENY_HOST;
    return SAGESB_OK;
}

SageSBViolation sage_sb_check_exec(const char* cmd) {
    if (!sage_sb_state.active) return SAGESB_OK;
    const SagePermManifest* m = sage_sb_state.manifest;
    if (!(m->flags & SAGEPERM_PROC_EXEC)) return SAGESB_DENY_PROC;
    (void)cmd;
    return SAGESB_OK;
}

SageSBViolation sage_sb_check_ffi(const char* lib) {
    if (!sage_sb_state.active) return SAGESB_OK;
    const SagePermManifest* m = sage_sb_state.manifest;
    if (!(m->flags & SAGEPERM_FFI)) return SAGESB_DENY_FFI;
    if (!lib_in_list(m, lib))       return SAGESB_DENY_FFI;
    return SAGESB_OK;
}

SageSBViolation sage_sb_check_env(const char* key) {
    if (!sage_sb_state.active) return SAGESB_OK;
    const SagePermManifest* m = sage_sb_state.manifest;
    if (!(m->flags & SAGEPERM_ENV_READ)) return SAGESB_DENY_ENV;
    (void)key;
    return SAGESB_OK;
}

SageSBViolation sage_sb_check_alloc(size_t bytes) {
    if (!sage_sb_state.active) return SAGESB_OK;
    const SagePermManifest* m = sage_sb_state.manifest;
    if (m->max_memory_mb == 0) return SAGESB_OK;
    sage_sb_state.bytes_allocated += bytes;
    if (sage_sb_state.bytes_allocated > m->max_memory_mb * 1024 * 1024)
        return SAGESB_LIMIT_MEM;
    return SAGESB_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// sage_runtime.h hook implementations
// (these resolve the non-stub versions when this file is linked)
// ─────────────────────────────────────────────────────────────────────────────

int sage_rt_sb_check_fs_read(const char* path) {
    return sage_sb_check_fs_read(path) == SAGESB_OK;
}
int sage_rt_sb_check_fs_write(const char* path) {
    return sage_sb_check_fs_write(path) == SAGESB_OK;
}
int sage_rt_sb_check_net(const char* host, int port) {
    return sage_sb_check_net(host, port) == SAGESB_OK;
}
int sage_rt_sb_check_exec(const char* cmd) {
    return sage_sb_check_exec(cmd) == SAGESB_OK;
}
int sage_rt_sb_check_ffi(const char* lib) {
    return sage_sb_check_ffi(lib) == SAGESB_OK;
}
int sage_rt_sb_check_env(const char* key) {
    return sage_sb_check_env(key) == SAGESB_OK;
}
void sage_rt_sb_init(const void* manifest) {
    sage_sb_init((const SagePermManifest*)manifest);
}

// ─────────────────────────────────────────────────────────────────────────────
// .sageperm manifest parser
// ─────────────────────────────────────────────────────────────────────────────

// Parse a string list like ["path1", "path2"] from a sageperm value.
// Returns count of items parsed.
static int parse_string_list(const char* val,
                              const char** out, int out_max) {
    int count = 0;
    const char* p = val;
    while (*p && count < out_max) {
        if (*p == '"') {
            p++;
            const char* start = p;
            while (*p && *p != '"') p++;
            int len = (int)(p - start);
            if (len > 0) {
                char* item = (char*)malloc(len + 1);
                memcpy(item, start, len);
                item[len] = '\0';
                out[count++] = item;
            }
            if (*p == '"') p++;
        } else {
            p++;
        }
    }
    return count;
}

int sage_sb_parse_manifest(const char* path, SagePermManifest* out) {
    if (!path || !out) return 0;
    memset(out, 0, sizeof(*out));

    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[sage_sandbox_rt] cannot open manifest: %s\n", path);
        return 0;
    }

    out->manifest_source = strdup(path);

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // Strip comments and blank lines
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r') continue;

        char* eq = strchr(p, '=');
        if (!eq) continue;

        // Parse key
        char key[128] = {0};
        size_t klen = (size_t)(eq - p);
        if (klen == 0 || klen >= sizeof(key)) continue;
        strncpy(key, p, klen);
        // Trim trailing whitespace from key
        for (int i = (int)strlen(key)-1;
             i >= 0 && (key[i]==' '||key[i]=='\t'); i--)
            key[i] = '\0';

        // Parse value
        char val[384] = {0};
        const char* vp = eq + 1;
        while (*vp == ' ' || *vp == '\t') vp++;
        strncpy(val, vp, sizeof(val)-1);
        for (int i = (int)strlen(val)-1;
             i >= 0 && (val[i]=='\n'||val[i]=='\r'||val[i]==' '); i--)
            val[i] = '\0';

        // net = true|false
        if (strcmp(key, "net") == 0) {
            if (strncmp(val, "true", 4) == 0) out->flags |= SAGEPERM_NET;

        // net.hosts = ["host1", "host2"]
        } else if (strcmp(key, "net.hosts") == 0) {
            out->flags |= SAGEPERM_NET;
            out->net_host_count = parse_string_list(val,
                out->net_hosts, SAGEPERM_MAX_HOSTS);

        // fs.read = ["path1", "path2"]
        } else if (strcmp(key, "fs.read") == 0) {
            out->flags |= SAGEPERM_FS_READ;
            out->fs_read_count = parse_string_list(val,
                out->fs_read_paths, SAGEPERM_MAX_PATHS);

        // fs.write = ["path1"]
        } else if (strcmp(key, "fs.write") == 0) {
            out->flags |= SAGEPERM_FS_WRITE;
            out->fs_write_count = parse_string_list(val,
                out->fs_write_paths, SAGEPERM_MAX_PATHS);

        // exec = true|false
        } else if (strcmp(key, "exec") == 0) {
            if (strncmp(val, "true", 4) == 0)
                out->flags |= SAGEPERM_PROC_EXEC | SAGEPERM_PROC_FORK;

        // ffi = true|false
        } else if (strcmp(key, "ffi") == 0) {
            if (strncmp(val, "true", 4) == 0) out->flags |= SAGEPERM_FFI;

        // ffi.libs = ["libm.so", "libssl.so"]
        } else if (strcmp(key, "ffi.libs") == 0) {
            out->flags |= SAGEPERM_FFI;
            out->ffi_lib_count = parse_string_list(val,
                out->ffi_libs, SAGEPERM_MAX_LIBS);

        // env = true|false
        } else if (strcmp(key, "env") == 0) {
            if (strncmp(val, "true", 4) == 0)
                out->flags |= SAGEPERM_ENV_READ;

        // max_memory_mb = N
        } else if (strcmp(key, "max_memory_mb") == 0) {
            out->max_memory_mb = (size_t)atoi(val);

        // max_open_files = N
        } else if (strcmp(key, "max_open_files") == 0) {
            out->max_open_files = atoi(val);

        // max_threads = N
        } else if (strcmp(key, "max_threads") == 0) {
            out->max_threads = atoi(val);

        // max_cpu_ms = N
        } else if (strcmp(key, "max_cpu_ms") == 0) {
            out->max_cpu_ms = (int64_t)atoi(val);
        }
    }

    fclose(f);
    return 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Manifest C-header emitter
//
// Called by aot.c when compiling with --sandbox.
// Emits something like:
//
//   // Auto-generated by sage --aot --sandbox myapp.sageperm
//   #include "sage_sandbox_rt.h"
//   static const SagePermManifest SAGE_PERM_MANIFEST = {
//       .flags          = SAGEPERM_FS_READ | SAGEPERM_NET,
//       .fs_read_paths  = {"/tmp", "./data"},
//       .fs_read_count  = 2,
//       ...
//   };
// ─────────────────────────────────────────────────────────────────────────────

int sage_sb_emit_manifest_header(const SagePermManifest* m,
                                  const char* out_path) {
    if (!m || !out_path) return 0;

    FILE* f = fopen(out_path, "w");
    if (!f) {
        fprintf(stderr, "[sage_sandbox_rt] cannot write manifest header: %s\n", out_path);
        return 0;
    }

    fprintf(f, "// Auto-generated by sage --aot --sandbox\n");
    fprintf(f, "// Do not edit. Regenerate by recompiling.\n");
    if (m->manifest_source)
        fprintf(f, "// Source: %s\n", m->manifest_source);
    fprintf(f, "\n");
    fprintf(f, "#pragma once\n");
    fprintf(f, "#include \"sage_sandbox_rt.h\"\n\n");
    fprintf(f, "static const SagePermManifest SAGE_PERM_MANIFEST = {\n");

    // Flags
    fprintf(f, "    .flags = 0");
    if (m->flags & SAGEPERM_NET)      fprintf(f, " | SAGEPERM_NET");
    if (m->flags & SAGEPERM_FS_READ)  fprintf(f, " | SAGEPERM_FS_READ");
    if (m->flags & SAGEPERM_FS_WRITE) fprintf(f, " | SAGEPERM_FS_WRITE");
    if (m->flags & SAGEPERM_FS_EXEC)  fprintf(f, " | SAGEPERM_FS_EXEC");
    if (m->flags & SAGEPERM_PROC_EXEC)fprintf(f, " | SAGEPERM_PROC_EXEC");
    if (m->flags & SAGEPERM_PROC_FORK)fprintf(f, " | SAGEPERM_PROC_FORK");
    if (m->flags & SAGEPERM_FFI)      fprintf(f, " | SAGEPERM_FFI");
    if (m->flags & SAGEPERM_ENV_READ) fprintf(f, " | SAGEPERM_ENV_READ");
    if (m->flags & SAGEPERM_ENV_WRITE)fprintf(f, " | SAGEPERM_ENV_WRITE");
    fprintf(f, ",\n");

    // fs.read paths
    fprintf(f, "    .fs_read_paths  = {");
    for (int i = 0; i < m->fs_read_count; i++)
        fprintf(f, "%s\"%s\"", i > 0 ? ", " : "", m->fs_read_paths[i]);
    fprintf(f, "},\n");
    fprintf(f, "    .fs_read_count  = %d,\n", m->fs_read_count);

    // fs.write paths
    fprintf(f, "    .fs_write_paths = {");
    for (int i = 0; i < m->fs_write_count; i++)
        fprintf(f, "%s\"%s\"", i > 0 ? ", " : "", m->fs_write_paths[i]);
    fprintf(f, "},\n");
    fprintf(f, "    .fs_write_count = %d,\n", m->fs_write_count);

    // net hosts
    fprintf(f, "    .net_hosts      = {");
    for (int i = 0; i < m->net_host_count; i++)
        fprintf(f, "%s\"%s\"", i > 0 ? ", " : "", m->net_hosts[i]);
    fprintf(f, "},\n");
    fprintf(f, "    .net_host_count = %d,\n", m->net_host_count);

    // ffi libs
    fprintf(f, "    .ffi_libs       = {");
    for (int i = 0; i < m->ffi_lib_count; i++)
        fprintf(f, "%s\"%s\"", i > 0 ? ", " : "", m->ffi_libs[i]);
    fprintf(f, "},\n");
    fprintf(f, "    .ffi_lib_count  = %d,\n", m->ffi_lib_count);

    // Limits
    fprintf(f, "    .max_memory_mb  = %zu,\n", m->max_memory_mb);
    fprintf(f, "    .max_open_files = %d,\n",  m->max_open_files);
    fprintf(f, "    .max_threads    = %d,\n",  m->max_threads);
    fprintf(f, "    .max_cpu_ms     = %lld,\n", (long long)m->max_cpu_ms);

    // Metadata
    if (m->program_name)
        fprintf(f, "    .program_name   = \"%s\",\n", m->program_name);
    if (m->manifest_source)
        fprintf(f, "    .manifest_source = \"%s\",\n", m->manifest_source);

    fprintf(f, "};\n");
    fclose(f);
    return 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Preset manifests
// ─────────────────────────────────────────────────────────────────────────────

SagePermManifest sage_sb_manifest_unrestricted(void) {
    SagePermManifest m;
    memset(&m, 0, sizeof(m));
    m.flags = SAGEPERM_ALL;
    m.manifest_source = "unrestricted";
    return m;
}

SagePermManifest sage_sb_manifest_deny_all(void) {
    SagePermManifest m;
    memset(&m, 0, sizeof(m));
    m.flags = SAGEPERM_NONE;
    m.manifest_source = "deny-all";
    return m;
}
