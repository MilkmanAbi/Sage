# SageTree IP1 — AOT Compiler Handoff
**Date:** May 2026  
**AOT Score:** 109/122 PASS (interpreter: 294/294)  
**Performance:** fib(35) = 1151× faster than interpreter

---

## What This Is
A custom **Ahead-of-Time (AOT) compiler** for the SageTree language (IP1 dialect).

**Pipeline:** `.sage` → `sage --aot file.sage` → C code → `gcc` → native binary

**Benchmarks:**
| Test | AOT | Interpreter | Speedup |
|------|-----|-------------|---------|
| fib(35) | 26.8ms | 30840ms | **1151×** |
| 1M int sum | 1.7ms | 125ms | **74×** |
| 5M float loop | 7.7ms | 579ms | **75×** |

---

## Quick Start

```bash
cd Sage-IP1-Handoff/IP1

# Build everything
make && make runtime

# Run a file through AOT
./sage --aot tests/lang/and_or_not.sage > /tmp/out.c
gcc -std=c11 -O2 -Iruntime /tmp/out.c obj/rt/libsage_runtime.a -o /tmp/out -lm
/tmp/out

# Run all AOT tests
python3 << 'PYEOF'
import subprocess, os, re
sage='./sage'; rt='obj/rt/libsage_runtime.a'; inc='runtime'
results={'PASS':[],'WRONG':[],'LINK':[],'TO':[],'CRASH':[]}
for t in sorted(f for f in os.listdir('tests/lang') if f.endswith('.sage')):
    path,name=f'tests/lang/{t}',t[:-5]
    r=subprocess.run([sage,'--aot',path],capture_output=True,timeout=5)
    if r.returncode!=0 or not r.stdout: results['CRASH'].append(name); continue
    open('/tmp/_t.c','wb').write(r.stdout)
    r2=subprocess.run(['gcc','-std=c11','-O2',f'-I{inc}','/tmp/_t.c',rt,'-o','/tmp/_tb','-lm'],capture_output=True)
    if r2.returncode!=0: results['LINK'].append(name); continue
    try:
        exp=subprocess.run([sage,path],capture_output=True,timeout=3).stdout
        got=subprocess.run(['/tmp/_tb'],capture_output=True,timeout=3).stdout
        (results['PASS'] if exp==got else results['WRONG']).append(name)
    except: results['TO'].append(name)
total=sum(len(v) for v in results.values())
print(f"AOT: {len(results['PASS'])}/{total} PASS | LINK:{len(results['LINK'])} WRONG:{len(results['WRONG'])} TO:{len(results['TO'])}")
PYEOF

# Run interpreter tests
python3 run_tests.py
```

---

## File Map

```
IP1/
├── src/aot.c              ← THE AOT COMPILER (2447 lines) — main file to work on
├── runtime/
│   ├── sage_runtime.h     ← Runtime header (SageValue, SageInst, SageClosure, etc.)
│   └── sage_runtime.c     ← Runtime implementation (1524 lines)
├── include/
│   └── aot.h              ← AotCompiler struct definition
├── tests/lang/            ← 122 language tests (.sage files)
├── obj/
│   ├── aot.o              ← Compiled AOT object (109/122 working state)
│   └── rt/libsage_runtime.a ← Compiled runtime library
├── sage                   ← Working ELF binary
├── Makefile
└── run_tests.py           ← Interpreter test runner
```

---

## AOT Architecture (src/aot.c)

### AotCompiler struct (include/aot.h)
```c
AotTypeEnv type_env;           // var name → JitTypeTag
char known_procs[256][64];     // directly callable functions
char known_ctors[128][64];     // class/struct constructors (always SageValue params)
int defer_stack[64];           // pending defers
int defer_count, known_proc_count, known_ctor_count;
```

### JitTypeTag (include/jit.h)
```
UNKNOWN=0 INT=1 FLOAT=2 BOOL=3 STRING=4 NIL=5
ARRAY=6 DICT=7 TUPLE=9 INSTANCE=10 FUNCTION=11
```

### Compilation pipeline (in aot_compile_program)
1. `aot_infer_types()` — scan top-level LETs for var types
2. `aot_scan_stmt_calls()` — collect call-site arg types per proc
3. Forward-declare all procs with typed signatures
4. `aot_emit_nested_procs()` — hoist nested procs (closures) to file scope
5. `aot_emit_proc()` — emit each proc body (detects generators via `_has_yield()`)
6. In `main()`: register enums, classes, structs, impl methods
7. Compile top-level statements

### Type specialization
- `INT + INT` → `((a) + (b))` raw C, no boxing
- `STRING + STRING` → `sage_rt_string_concat(sage_rt_string(a), sage_rt_string(b))`
- `UNKNOWN` operands → `sage_rt_add(a, b)` with boxing

### Key runtime functions (runtime/sage_runtime.h)
```c
sage_rt_instance_new(classval)            // create instance (lazy field alloc)
sage_rt_field_set/get(inst, name, val)    // dynamic field access
sage_rt_method_call(inst, name, argc, argv) // method dispatch
sage_rt_method_call_super(inst*, name, argc, argv) // parent method
sage_rt_add_methods(classval, methods, n) // register impl block methods
sage_rt_make_fn(fn, env, name)           // create closure (env in SageClosure->env)
sage_rt_call_fn(fn, argc, argv)           // call closure (env passed as void*)
sage_rt_coro_new/next/yield              // ucontext coroutines (for gen_loop)
sage_rt_class_new(name, parent, methods, n, field_names, fc, is_struct)
```

### CRITICAL: src/aot.c brace structure
The file uses **GCC nested function extension** — `aot_emit_proc`, `aot_compile_program`,
etc. are NOT literally nested, but the file has complex brace nesting from historical edits.

**When editing aot.c, ALWAYS use a string-aware brace counter** (skips `"..."`, `//`, `/* */`).
The naive `count('{') - count('}')` approach FAILS because `aot_emit()` calls contain
literal `{` and `}` in C string arguments.

```python
# String-aware depth check — use this:
def depth_at(content, target_char):
    depth = 0; i = 0; n = len(content)
    while i < n and i < target_char:
        c = content[i]
        if c == '/' and i+1 < n and content[i+1] == '/':
            while i < n and content[i] != '\n': i += 1
        elif c == '/' and i+1 < n and content[i+1] == '*':
            i += 2
            while i < n-1 and not (content[i] == '*' and content[i+1] == '/'): i += 1
            i += 2
        elif c == '"':
            i += 1
            while i < n and content[i] != '"':
                if content[i] == '\\': i += 1
                i += 1
            i += 1
        elif c == '{': depth += 1; i += 1
        elif c == '}': depth -= 1; i += 1
        else: i += 1
    return depth
```

**Key depth invariants** (verify after any edit):
- `aot_compile_program` must be at depth 0
- `aot_compile_to_binary` must be at depth 0  
- `aot_write_c_file` must be at depth 0
- Total file depth must be 0

---

## What's Passing (109/122)

All basic types, arithmetic, strings, arrays, dicts, tuples, ranges,
for/while/if/match, closures (with mutable captures), classes + inheritance,
impl blocks, enums, try/catch/finally/raise, defer, generators (sequential yields),
multiple params, higher-order functions, recursion, string/array/dict methods, etc.

---

## What's Failing (13 remaining)

### LINK — compile error in generated C
| Test | Root Cause |
|------|-----------|
| `adt_destructure` | ADT pattern binding emits `sg_v` but never declares it |
| `adt_pattern_match` | Same — `sg_r` undeclared |
| `gen_loop` | While-loop generators need ucontext coroutines (runtime has `sage_rt_coro_*` but AOT not wired) |

### WRONG — output mismatch
| Test | Root Cause |
|------|-----------|
| `adt_enum` | ADT variant constructors not implemented |
| `basic_tuple` | AOT prints extra `nil` line (interpreter errors out instead) |
| `struct_*` (4) | Use `struct_def()` which is interpreter-only FFI (by design) |
| `struct_value_semantics` | Structs need copy-on-assign (currently reference) |
| `super_init` | `self.name` set by Animal.init() not accessible after super.init() |
| `super_method` | Wrong method found in 3-level hierarchy |

### TIMEOUT
| Test | Root Cause |
|------|-----------|
| `recursion_limit` | AOT has no recursion depth counter |

---

## Next Priorities

1. **gen_loop** (+1): Wire up `sage_rt_coro_new/next/yield` in AOT
   - Detect non-sequential yields → emit `_gen_X_body(SageCoroutine* _co)` 
   - Inside body, `STMT_YIELD` → `sage_rt_coro_yield(_co, val)` instead of `return val`
   - Constructor: `sage_rt_coro_new(body_fn, argc, argv)` → `sage_rt_make_fn(_coro_next_fn, co, name)`
   - `_coro_next_fn` is already emitted at file top: `sage_rt_coro_next((SageCoroutine*)_env)`

2. **super_init** (+1): `self.name` not persisting after super.init()
   - `sage_rt_method_call_super(_self, "init", 1, args)` correctly calls Animal.init
   - But `sage_rt_field_set(sg_self, "name", name)` inside Animal.init sets on `_self`
   - Verify `_self` pointer identity between parent and child init calls

3. **ADT** (+3): adt_enum, adt_destructure, adt_pattern_match
   - Variant constructors: `Shape.Circle(5.0)` → `{"__tag":"Circle","radius":5.0}`
   - Pattern match: `Shape.Circle(r)` → check `__tag == "Circle"`, bind `r = dict["radius"]`

4. **struct_value_semantics** (+1): `sage_rt_instance_copy(inst)` deep copy on assignment

5. **super_method** (+1): Fix method resolution order for 3-level inheritance
