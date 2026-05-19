// stubs.c — Stub implementations for optional/removed subsystems
//
// JIT stubs remain because the real JIT is behind SAGE_EXPERIMENTAL_JIT.

#include <stdio.h>
#include <stdint.h>

// ── JIT stubs ────────────────────────────────────────────────────────
void jit_init(void) {}
void jit_shutdown(void) {}
const char* jit_type_name(int t) { (void)t; return "unknown"; }
void jit_record_call(void) {}
void* jit_get_profile(void) { return NULL; }
int jit_should_compile(void) { return 0; }
void jit_compile_function(void) {}
void jit_record_return(void) {}

// ── LSP stub ─────────────────────────────────────────────────────────
#ifndef SAGE_HAS_LSP
void lsp_run(void) { fprintf(stderr, "LSP not available in this build\n"); }
#endif


// ── Removed modules ──────────────────────────────────────────────────
void create_ml_native_module(void*cache){(void)cache;}
