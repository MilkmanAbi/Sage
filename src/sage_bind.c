// sage_bind.c — SageLang C binding generator
// Uses libclang to parse C headers and emit .sage extern declarations.
//
// Usage:
//   sage bind <header.h> [--lib libname] [--output bindings.sage] [-- <clang-flags>]
//
// Output form:
//   @clib("libname")
//   extern proc function_name(arg: type, ...) -> return_type

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <clang-c/Index.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ── Type mapping: C → Sage ────────────────────────────────────────────────

typedef struct {
    FILE* out;
    const char* libname;
    const char* header_file;   // resolved absolute path to the main header
    int func_count;
    int struct_count;
    int typedef_count;
} BindCtx;

// Map a CXType to a Sage type string (caller must free)
static char* c_type_to_sage(CXType t) {
    // Canonical kind
    switch (t.kind) {
        case CXType_Void:             return strdup("void");
        case CXType_Bool:             return strdup("bool");
        case CXType_Char_S:
        case CXType_Char_U:           return strdup("byte");
        case CXType_SChar:            return strdup("i8");
        case CXType_UChar:            return strdup("u8");
        case CXType_Short:            return strdup("i16");
        case CXType_UShort:           return strdup("u16");
        case CXType_Int:              return strdup("int");
        case CXType_UInt:             return strdup("uint");
        case CXType_Long:             return strdup("i64");
        case CXType_ULong:            return strdup("u64");
        case CXType_LongLong:         return strdup("i64");
        case CXType_ULongLong:        return strdup("u64");
        case CXType_Float:            return strdup("f32");
        case CXType_Double:           return strdup("float");
        case CXType_LongDouble:       return strdup("float");
        case CXType_NullPtr:          return strdup("nil");
        case CXType_Pointer: {
            CXType pointee = clang_getPointeeType(t);
            // const char* and char* → str
            if (pointee.kind == CXType_Char_S || pointee.kind == CXType_Char_U) {
                return strdup("str");
            }
            if (pointee.kind == CXType_SChar || pointee.kind == CXType_UChar) {
                return strdup("str");
            }
            if (pointee.kind == CXType_Void) {
                return strdup("ptr<byte>");
            }
            char* inner = c_type_to_sage(pointee);
            char buf[256];
            snprintf(buf, sizeof(buf), "ptr<%s>", inner);
            free(inner);
            return strdup(buf);
        }
        case CXType_ConstantArray:
        case CXType_IncompleteArray: {
            CXType elem = clang_getArrayElementType(t);
            char* inner = c_type_to_sage(elem);
            char buf[256];
            snprintf(buf, sizeof(buf), "Array<%s>", inner);
            free(inner);
            return strdup(buf);
        }
        case CXType_FunctionProto:
        case CXType_FunctionNoProto:
            return strdup("ptr<byte>"); // function pointer = opaque
        case CXType_Typedef: {
            CXString spelling = clang_getTypeSpelling(t);
            const char* s = clang_getCString(spelling);
            // Strip const/volatile
            char* name = strdup(s);
            clang_disposeString(spelling);
            // Remove "const " prefix if present
            if (strncmp(name, "const ", 6) == 0) {
                char* trimmed = strdup(name + 6);
                free(name);
                name = trimmed;
            }
            return name;
        }
        case CXType_Record:
        case CXType_Enum: {
            CXString spelling = clang_getTypeSpelling(t);
            const char* s = clang_getCString(spelling);
            char* name = strdup(s);
            clang_disposeString(spelling);
            // Clean "struct Foo" → "Foo", "enum Foo" → "Foo"
            if (strncmp(name, "struct ", 7) == 0) {
                char* trimmed = strdup(name + 7);
                free(name); name = trimmed;
            } else if (strncmp(name, "enum ", 5) == 0) {
                char* trimmed = strdup(name + 5);
                free(name); name = trimmed;
            } else if (strncmp(name, "const ", 6) == 0) {
                char* trimmed = strdup(name + 6);
                free(name); name = trimmed;
            }
            return name;
        }
        default: {
            CXString spelling = clang_getTypeSpelling(t);
            const char* s = clang_getCString(spelling);
            char* r = strdup(s[0] ? s : "any");
            clang_disposeString(spelling);
            return r;
        }
    }
}

// Sage-ify a C identifier name (nothing to change, but could sanitise)
static const char* safe_name(const char* name) {
    // If name is a Sage keyword, prefix with underscore
    static const char* keywords[] = {
        "let","var","proc","if","else","while","for","in","return","match",
        "class","self","import","from","as","struct","enum","trait","defer",
        "unsafe","true","false","nil","and","or","not","break","continue",NULL
    };
    for (int i = 0; keywords[i]; i++) {
        if (strcmp(name, keywords[i]) == 0) {
            static char buf[128];
            snprintf(buf, sizeof(buf), "_%s", name);
            return buf;
        }
    }
    return name;
}

// ── Visitor ───────────────────────────────────────────────────────────────

// Check if a cursor is from the primary header file (not system includes)
static int is_from_main_file(CXCursor cursor, const char* header_file) {
    CXSourceLocation loc = clang_getCursorLocation(cursor);
    if (clang_Location_isInSystemHeader(loc)) return 0;
    // Check file name
    CXFile file;
    unsigned line, col, off;
    clang_getFileLocation(loc, &file, &line, &col, &off);
    if (!file) return 0;
    CXString fname = clang_getFileName(file);
    const char* fstr = clang_getCString(fname);
    int match = (fstr && header_file && strstr(fstr, header_file) != NULL);
    clang_disposeString(fname);
    return match;
}

static enum CXChildVisitResult visitor(CXCursor cursor, CXCursor parent, CXClientData data) {
    BindCtx* ctx = (BindCtx*)data;
    (void)parent;

    enum CXCursorKind kind = clang_getCursorKind(cursor);

    // Only emit declarations from the main header file
    if (!is_from_main_file(cursor, ctx->header_file)) {
        return CXChildVisit_Continue;
    }

    // ── Function declarations ───────────────────────────────────────────
    if (kind == CXCursor_FunctionDecl) {
        CXString name_cx = clang_getCursorSpelling(cursor);
        const char* name = clang_getCString(name_cx);

        // Skip static/inline functions — not in the .so
        if (clang_Cursor_getStorageClass(cursor) == CX_SC_Static) {
            clang_disposeString(name_cx);
            return CXChildVisit_Continue;
        }

        CXType func_type = clang_getCursorType(cursor);
        CXType ret_type  = clang_getResultType(func_type);
        char* ret_sage   = c_type_to_sage(ret_type);

        int n_args = clang_Cursor_getNumArguments(cursor);
        int variadic = clang_isFunctionTypeVariadic(func_type);

        fprintf(ctx->out, "@clib(\"%s\")\n", ctx->libname);
        fprintf(ctx->out, "extern proc %s(", safe_name(name));

        for (int i = 0; i < n_args; i++) {
            CXCursor arg = clang_Cursor_getArgument(cursor, i);
            CXString argname_cx = clang_getCursorSpelling(arg);
            const char* argname = clang_getCString(argname_cx);
            CXType argtype = clang_getCursorType(arg);
            char* arg_sage = c_type_to_sage(argtype);

            if (i > 0) fprintf(ctx->out, ", ");
            if (argname && argname[0]) {
                fprintf(ctx->out, "%s: %s", safe_name(argname), arg_sage);
            } else {
                fprintf(ctx->out, "arg%d: %s", i, arg_sage);
            }
            free(arg_sage);
            clang_disposeString(argname_cx);
        }

        if (variadic) {
            if (n_args > 0) fprintf(ctx->out, ", ");
            fprintf(ctx->out, "...args: any");
        }

        if (strcmp(ret_sage, "void") == 0) {
            fprintf(ctx->out, ")\n");
        } else {
            fprintf(ctx->out, ") -> %s\n", ret_sage);
        }

        free(ret_sage);
        clang_disposeString(name_cx);
        ctx->func_count++;
    }

    // ── Typedef declarations ────────────────────────────────────────────
    else if (kind == CXCursor_TypedefDecl) {
        CXString name_cx = clang_getCursorSpelling(cursor);
        const char* name = clang_getCString(name_cx);
        CXType underlying = clang_getTypedefDeclUnderlyingType(cursor);

        // Only emit simple scalar typedefs and opaque structs
        // Skip function-pointer typedefs (too complex for now)
        if (underlying.kind != CXType_FunctionProto &&
            underlying.kind != CXType_FunctionNoProto) {
            char* under_sage = c_type_to_sage(underlying);
            // Only emit if it's different from the name (not a circular typedef)
            if (strcmp(name, under_sage) != 0) {
                fprintf(ctx->out, "# typedef %s = %s\n", name, under_sage);
            }
            free(under_sage);
        }
        clang_disposeString(name_cx);
        ctx->typedef_count++;
    }

    return CXChildVisit_Continue;
}

// ── Entry point ───────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: sage-bind <header.h> [--lib libname] [--output file.sage] [-- <clang-flags...>]\n");
        fprintf(stderr, "\nExample:\n");
        fprintf(stderr, "  sage-bind sqlite3.h --lib libsqlite3 --output lib/sqlite3.sage\n");
        return 1;
    }

    const char* header   = NULL;
    const char* libname  = NULL;
    const char* outfile  = NULL;
    const char** extra_flags = NULL;
    int n_extra = 0;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--lib") == 0 && i+1 < argc) {
            libname = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i+1 < argc) {
            outfile = argv[++i];
        } else if (strcmp(argv[i], "--") == 0) {
            extra_flags = (const char**)(argv + i + 1);
            n_extra = argc - i - 1;
            break;
        } else if (argv[i][0] != '-') {
            header = argv[i];
        }
    }

    if (!header) {
        fprintf(stderr, "sage-bind: no header file specified\n");
        return 1;
    }

    // Derive libname from header if not given
    char derived_lib[256] = {0};
    if (!libname) {
        const char* base = strrchr(header, '/');
        base = base ? base+1 : header;
        snprintf(derived_lib, sizeof(derived_lib), "lib%s", base);
        char* dot = strrchr(derived_lib, '.');
        if (dot) *dot = '\0';
        libname = derived_lib;
    }

    // Derive output filename if not given
    char derived_out[512] = {0};
    if (!outfile) {
        const char* base = strrchr(header, '/');
        base = base ? base+1 : header;
        char stem[256];
        strncpy(stem, base, sizeof(stem)-1);
        char* dot = strrchr(stem, '.');
        if (dot) *dot = '\0';
        snprintf(derived_out, sizeof(derived_out), "%s.sage", stem);
        outfile = derived_out;
    }

    // Build clang args
    const char* default_args[] = {
        "-x", "c",
        "-std=c11",
        "-D_GNU_SOURCE",
        NULL
    };
    int n_default = 4;
    int total_args = n_default + n_extra;
    const char** all_args = malloc(sizeof(char*) * total_args);
    for (int i = 0; i < n_default; i++) all_args[i] = default_args[i];
    for (int i = 0; i < n_extra; i++) all_args[n_default+i] = extra_flags[i];

    // Parse the header with libclang
    CXIndex idx = clang_createIndex(0, 0);
    CXTranslationUnit tu = clang_parseTranslationUnit(
        idx, header,
        all_args, total_args,
        NULL, 0,
        CXTranslationUnit_SkipFunctionBodies |
        CXTranslationUnit_DetailedPreprocessingRecord
    );
    free(all_args);

    if (!tu) {
        fprintf(stderr, "sage-bind: failed to parse '%s'\n", header);
        clang_disposeIndex(idx);
        return 1;
    }

    // Report any diagnostics
    unsigned n_diag = clang_getNumDiagnostics(tu);
    int has_error = 0;
    for (unsigned i = 0; i < n_diag; i++) {
        CXDiagnostic diag = clang_getDiagnostic(tu, i);
        enum CXDiagnosticSeverity sev = clang_getDiagnosticSeverity(diag);
        if (sev >= CXDiagnostic_Error) {
            CXString dmsg = clang_getDiagnosticSpelling(diag);
            fprintf(stderr, "sage-bind: parse error: %s\n", clang_getCString(dmsg));
            clang_disposeString(dmsg);
            has_error = 1;
        }
        clang_disposeDiagnostic(diag);
    }

    if (has_error) {
        fprintf(stderr, "sage-bind: header has parse errors — bindings may be incomplete\n");
    }

    // Resolve header to absolute path (for is_from_main_file check)
    char resolved[4096] = {0};
    if (!realpath(header, resolved)) {
        strncpy(resolved, header, sizeof(resolved)-1);
    }
    // Keep just the filename part for matching
    const char* header_base = strrchr(resolved, '/');
    header_base = header_base ? header_base+1 : resolved;

    // Open output file
    FILE* out = strcmp(outfile, "-") == 0 ? stdout : fopen(outfile, "w");
    if (!out) {
        perror(outfile);
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(idx);
        return 1;
    }

    // Write header — flat file with @clib pragma on each extern proc
    fprintf(out, "# Auto-generated by: sage bind %s --lib %s\n", header, libname);
    fprintf(out, "# Do not edit manually — regenerate with the same command.\n");
    fprintf(out, "# Usage: import %s\n\n", libname);

    BindCtx ctx = {out, libname, header_base, 0, 0, 0};
    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, visitor, &ctx);

    fprintf(out, "\n");
    if (out != stdout) fclose(out);

    // Summary
    fprintf(stderr, "sage-bind: wrote %d functions, %d typedefs to %s\n",
            ctx.func_count, ctx.typedef_count, outfile);

    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(idx);
    return 0;
}
