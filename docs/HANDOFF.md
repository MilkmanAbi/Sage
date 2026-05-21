# SageTree Development Handoff
# For the next Claude instance continuing this project
# Written: 2026-05-19 | Current: Sage-AP16 | Tests: 294/294

## What Is This

SageTree is a programming language. Started as SageLang by Night-Traders-Dev, then
heavily forked and rewritten by Abinaash (MilkmanAbi). It's written in C (~38,700
lines), has a Sage standard library (~8,700 lines of .sage files), and 316 test files.

The language can be interpreted (MossVM bytecode VM + AST tree-walk fallback) or
compiled (experimental AOT, behind flags). The interpreter is the primary path.

The user is Abinaash, a computer engineering student at Singapore Polytechnic. He
communicates casually and directly, uses strong language, and prefers concise
actionable work. He cares deeply about making the language genuinely friendly and
kind to developers. He does NOT want a Rust clone or a Python clone. He wants
something softer, more patient, with world-class error messages.


## Project Location and Structure

Working directory: /home/claude/Sage-AP16/ (or wherever the zip is extracted)

```
Sage-AP16/
├── src/                 33 C files (the language core)
│   ├── interpreter.c    ~5,500 lines. AST tree-walk interpreter. THE big file.
│   ├── parser.c         Parser
│   ├── lexer.c          Lexer
│   ├── firefly.c        524 lines. Error diagnosis subsystem.
│   ├── sage_python.c    Python FFI (CPython embedding)
│   ├── safety.c         887 lines. Ownership/borrow analysis.
│   ├── stdlib.c         Native C stdlib (math, io, sys modules)
│   ├── main.c           Entry point, REPL, CLI flags
│   ├── env.c            Environments/scopes (has Env pool + EnvNode pool)
│   ├── value.c          Value type system
│   ├── vm/              MossVM bytecode VM (vm.c, bytecode.c, runtime.c, program.c)
│   ├── gc/              SageGC (sagegc.c, env_hashmap.c)
│   └── lilybox/         LilyBox sandbox (lilybox.c)
├── include/             24 headers. firefly.h has the full error code registry.
├── lib/                 51 Sage stdlib modules (.sage files)
│   ├── arrays.sage      REWRITTEN - uses correct Sage syntax
│   ├── strings.sage     REWRITTEN
│   ├── dicts.sage       REWRITTEN
│   ├── iter.sage        REWRITTEN
│   ├── stats.sage       REWRITTEN
│   ├── assert.sage      REWRITTEN
│   ├── utils.sage       REWRITTEN
│   ├── fileio.sage      NEW
│   ├── std/             24 modules (auto-fixed from legacy, mostly working)
│   ├── crypto/          6 modules (906 lines pure Sage crypto - SHA256, AES, etc)
│   └── net/             8 modules (auto-fixed from legacy)
├── tests/               316 test files, 294 have EXPECT comments
│   ├── lang/            Language feature tests
│   ├── stdlib/          Standard library tests
│   ├── safety/          Memory/sandbox tests
│   └── perf/            Performance/conformance tests
├── bench/               Benchmarks (fib, alloc, string, struct x 4 languages)
├── docs/
│   ├── Documentation.md Full language guide
│   ├── Language-Spec.md Formal specification
│   └── Roadmap.md       What's done, what's next
├── Makefile             make / make test / make clean
├── run_tests.py         Test runner
├── README.md            GitHub-facing README
├── LICENSE              MIT
└── VERSION              0.2.0-alpha
```


## Named Subsystems

| Name | What | File(s) |
|------|------|---------|
| MossVM | Bytecode VM, primary interpreter | src/vm/vm.c, bytecode.c |
| SageGC | Tri-color mark-sweep GC | src/gc/sagegc.c, src/gc.c |
| Firefly | Error diagnosis (errors, warnings, suggestions) | src/firefly.c, include/firefly.h |
| LilyBox | Sandbox execution environment | src/lilybox/lilybox.c |
| LilyKnight | Permission enforcement (inside LilyBox) | src/lilybox/lilybox.h |
| FrogPond | User-facing sandbox API | lib/sandbox.sage |


## Build and Test

```sh
cd Sage-AP16
make            # Builds ./sage
make test       # Runs python3 run_tests.py
make clean      # Removes obj/ and sage binary
```

Conditional dependencies (auto-detected by Makefile):
- libffi: C FFI (ffi_call with arbitrary args). Without it, limited hardcoded dispatch.
- python3-dev: Python FFI. Without it, -DSAGE_NO_PYTHON.
- libcurl: HTTP in net module. Without it, -DSAGE_HAS_CURL not set.

Run with: `./sage script.sage` (MossVM default) or `./sage --runtime ast script.sage`


## What Works Well (do not break these)

1. Gradual type system: `int x = 42`, `var y = "hello"`, `let z = 3.14`
2. Immutability: `let` and `const` enforced. Firefly error E040 on reassign.
3. Structs with value semantics + impl blocks
4. Classes with inheritance and virtual dispatch
5. ADT enums: `enum Shape: Circle(r: float) Rect(w: float, h: float) Point`
6. Pattern matching with destructuring: `case Shape.Circle(r): ...`
7. Option (Some/None) and Result (Ok/Err) with match
8. Python FFI direct syntax: `math.sqrt(144.0)` works natively
9. C FFI via libffi: `ffi_call(lib, "func", "int", [args])` up to 16 args
10. Firefly error diagnosis with source location, carets, "did you mean?", stack traces
11. Unused variable warnings (W001), non-exhaustive match warnings (W002)
12. Double-free detection (E080), use-after-free detection (E081)
13. Division by zero caught (E030, was silently returning 0 before)
14. Manual memory: @manual blocks, mem_alloc/free/read/write, ptr_add
15. Hybrid memory: gc_disable()/gc_enable()
16. Sandbox: LilyBox + LilyKnight enforces permissions on all stdlib fopen/system/dlopen
17. REPL with expression auto-print
18. Closures, generators, async/await


## Performance Baseline

| Benchmark | Python 3.12 | Sage VM | Ratio |
|-----------|-------------|---------|-------|
| fib(30) | 0.097s | 2.80s | 29x slower |
| alloc 100k | 0.020s | 0.037s | 1.8x slower |
| string 10k | 0.006s | 0.025s | 4.4x slower |
| struct 50k | 0.019s | 0.106s | 5.5x slower |

The Env pool and EnvNode pool (added P13) gave 34% speedup on fib. Still slow
because every function call does: env_create + param binding + GC tracking.


## WHAT TO DO NEXT (priority order)

### 1. FFI Completion (HIGH PRIORITY)

**Struct marshaling for C FFI.**
Currently ffi_call handles int, double, string, pointer, void. It cannot pass or
return C structs. The approach: add ffi_struct_define() that describes a struct
layout, then ffi_struct_new/get/set to create and access fields. Use libffi's
ffi_type struct support.

Files: src/interpreter.c (around line 1366, ffi_call_native)

**ffi_read / ffi_write builtins.**
Raw memory read/write for FFI pointers. mem_read/mem_write exist for @manual blocks
but aren't accessible for FFI-returned pointers in normal code. Expose them as
ffi_read(ptr, offset, type) and ffi_write(ptr, offset, type, value).

Files: src/interpreter.c (around line 2614, builtin registration)

**Callback support.**
Pass a Sage `proc` as a C function pointer. Requires libffi closure allocation
(ffi_closure_alloc). When C calls the function pointer, it invokes the Sage proc.
This is needed for C libraries that take callbacks (qsort, signal handlers, event loops).

Files: src/interpreter.c (new ffi_callback_native function)

### 2. Legacy Test Cleanup (QUICK WIN)

**36 occurrences of `== true` / `== false` in test files.**
These work but are non-idiomatic. Should use bare bool or `not`. All in:
- tests/perf/atomic_sage_test.sage
- tests/perf/smp_test.sage
- tests/perf/channel_test.sage
- tests/perf/threadpool_test.sage
- tests/perf/semaphore_test.sage
- tests/perf/rwlock_test.sage
- tests/perf/equality.sage
- tests/safety/safety_option.sage

Fix: `== true` -> remove, `== false` -> `not`. Then update EXPECT lines.

### 3. Firefly Expansion (THE BIG ONE)

Current: 524 lines, 55+ call sites, error codes E001-E109, W001-W002.
Target: 2000+ lines, 100+ call sites, W001-W010 all implemented.

**Planned warnings to implement:**
- W003: String concat in loop (detect `+` on strings inside while/for, suggest .join())
- W004: Shadowed variable (same name in inner scope)
- W005: Missing return in typed function (proc with -> type but no return on some paths)
- W006: Unreachable code after return
- W007: Comparison always true/false
- W008: Integer division truncation (int / int silently drops remainder)

**Planned error improvements:**
- Type flow tracing: "x is str because it was assigned 'hello' at line 3"
- Memory leak detection in @manual: "allocated at line 5, never freed"
- Better "not callable" errors: show what type the value actually is
- Firefly for parse errors (currently uses separate diagnostic.c, should unify)

**Architecture note:** The warning pass (firefly_warn_pass in firefly.c) does a
single AST walk. It currently only tracks variable declarations and usage. To add
W003-W008, extend the walker with: return tracking per function, scope nesting
for shadowing, loop detection for string concat.

Files: src/firefly.c (firefly_warn_pass, around line 400)

### 4. Language Features Still Missing

**Generics** (C2 on roadmap). Even basic ones would unblock real usage:
```sage
proc max<T>(a: T, b: T) -> T:
    if a > b: return a
    return b
```

**Traits** (C3). Interface definitions:
```sage
trait Printable:
    proc to_str(self) -> str
```

**Named parameters** (C4): `connect(host: "x", port: 80)`

**Spread operator** (C5): `[...arr1, ...arr2]`

**pub visibility** (C6): `pub proc api_function(): ...`

### 5. Performance (if time permits)

**Struct field layout** (A2). Struct fields use DictValue (hash table). Every field
access is a hash lookup. Should use fixed-layout array. Field names to indices at
parse time. Would make struct benchmark 3-5x faster.

**String builder** (A3). String concat in loops is O(n^2). Internal StringBuilder
that doubles capacity would fix the string benchmark.

**MossVM bytecode expansion** (A4). Many features fall back from VM to AST
interpreter (struct creation, enum matching, impl blocks). Compiling these to
bytecode would eliminate the fallback overhead.


## Key Decisions Made (respect these)

- Declaration model: `int x = 42` (typed mutable), `var x = 12` (inferred mutable),
  `let x = 12` (immutable), `const PI = 3.14` (constant)
- Integer division: C++ style truncating (10/3 = 3)
- Error format: `-- error[E001]: message -- file:line:col --` with `Firefly:`
  explanation. NOT Rust's `= help:` style.
- JIT: behind SAGE_EXPERIMENTAL_JIT flag. Future plan: LuaJIT-based tracing JIT.
- Crypto stdlib: 906 lines pure Sage (SHA-256, AES, HMAC, Base64, password hash). Keep it.
- All GPU/RP2040/Night-Traders-Dev code removed. Don't add it back.
- `init` and `end` are reserved words. Don't use them as parameter names.
- The language uses indentation for blocks, NOT `end` keywords.
- `push()`, `len()`, `split()` are NOT bare functions. Use `.push()`, `.length`, `.split()`.
  (`len()` is a builtin but `.length` is preferred in stdlib.)

## How Tests Work

Each .sage file has `# EXPECT: output` comments at the top. The test runner
(run_tests.py) runs each file and compares stdout against expected output.
Files without EXPECT comments are skipped.

To add a test: create `tests/category/test_name.sage` with EXPECT lines.
To run: `python3 run_tests.py` (or `python3 run_tests.py -v` for verbose failures).

Current: 294 passing, 0 failing.


## Abinaash's Vision

"A friendlier language for friendlier times." The language should be:
- Kind: world-class error messages that help, not scold
- Honest: alpha software, one maintainer, slow progress, and that's okay
- Flexible: GC or manual or hybrid, interpreted or compiled, typed or untyped
- Practical: Python FFI means you can use any Python library today

Firefly is the heart of this vision. Every error should feel like a friend
explaining what went wrong, not a compiler screaming at you.

Do NOT use emdashes in any user-facing text. Use kaomojis where appropriate
(README, docs). Keep things warm but professional.
