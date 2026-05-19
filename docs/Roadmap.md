# SageTree Roadmap

Where we are, where we are going, and what needs to happen.

Current state: 0.2.0-alpha, 293/295 tests passing, single maintainer.


## Done

These are completed and working in the current release.

- Gradual type system (int, float, str, bool, Array, Dict, Tuple)
- Type-first declarations with runtime enforcement
- Immutability (let/const)
- Structs with value semantics and impl blocks
- Classes with inheritance and virtual dispatch
- ADT enums with associated data
- Pattern matching with destructuring (Shape.Circle(r))
- Option (Some/None) and Result (Ok/Err) types
- Closures, generators, async/await
- String interpolation and builtin methods
- Manual memory management (@manual blocks)
- Hybrid GC/manual mode (gc_disable/gc_enable)
- Double-free and use-after-free detection
- Ownership and borrow analysis (sage safety)
- Python FFI with direct method syntax (math.sqrt(144))
- C FFI via libffi
- Sandbox (LilyBox/LilyKnight) with permission enforcement
- Firefly error diagnosis (55+ call sites, error codes, stack traces)
- Unused variable warnings (W001)
- Non-exhaustive match warnings (W002)
- REPL


## Performance

Current benchmarks against Python 3.12 (lower is better):

| Benchmark | Python | Sage VM | Ratio |
|-----------|--------|---------|-------|
| fib(30) recursive | 0.097s | 3.89s | 40x slower |
| alloc 100k arrays | 0.020s | 0.037s | 1.8x slower |
| string concat 10k | 0.006s | 0.025s | 4.4x slower |
| struct create 50k | 0.019s | 0.106s | 5.5x slower |

Allocation performance is competitive. Recursive call overhead is the main bottleneck.

### Performance work planned

- Struct field layout optimization (dict to fixed array)
- String builder for concatenation in loops
- MossVM bytecode coverage expansion (reduce AST fallbacks)
- Tail call optimization for recursive functions
- SageGC nursery (TLAB fast-path allocation)


## Next: Language Features

These are the features needed to make Sage usable for real programs.

### Generics

```sage
proc max<T>(a: T, b: T) -> T:
    if a > b: return a
    return b
```

Even basic generics would unblock generic container types and utility functions.

### Traits

```sage
trait Printable:
    proc to_str(self) -> str

impl Printable for Vec2:
    proc to_str(self) -> str:
        return "(" + str(self.x) + ", " + str(self.y) + ")"
```

### Named parameters

```sage
proc connect(host: str, port: int, timeout: float):
    ...

connect(host: "localhost", port: 8080, timeout: 5.0)
```

### Spread operator

```sage
let combined = [...arr1, ...arr2]
proc sum(...args): ...
```

### Pub visibility

```sage
pub proc api_function():
    ...

pub struct Config:
    ...
```


## Next: Tooling

### Package manager

```sh
sage init
sage install package_name
sage run
```

Package format: directory with sage.toml and src/.

### LSP server

Basic go-to-definition, hover info, and Firefly diagnostics in editors. The LSP code exists but is untested.

### Documentation generator

```sh
sage doc src/
```

Generate HTML/Markdown from doc comments.


## Next: Firefly

Firefly currently covers 55+ error paths. The goal is complete coverage.

### Planned warnings

- W003: String concatenation in loop (suggest .join())
- W004: Shadowed variable in inner scope
- W005: Missing return in function with return type annotation
- W006: Unreachable code after return statement
- W007: Comparison that is always true or always false
- W008: Integer division result silently truncated

### Planned diagnostics

- Type flow tracing ("x is str because it was assigned 'hello' at line 3")
- Memory leak detection in @manual blocks ("allocated at line 5, never freed")
- Pattern match exhaustiveness checking for known enum types
- Performance hints ("string concat in loop at line 12, consider .join()")


## Next: Safety

### Borrow checker improvements

The current ownership analysis catches use-after-move and double-move. Planned:
- Mutable borrow tracking (prevent aliased mutation)
- Lifetime analysis for references
- Automatic move vs copy semantics based on type

### Compiled safety (planned)

LilyBox currently only works in the interpreter. Plan to add a lightweight sandbox for compiled programs too.


## Long Term

### JIT compiler

Replace the experimental x86-64 method JIT with a LuaJIT-based tracing JIT customized for Sage's type system and GC. This is the realistic path to native-competitive performance for interpreted code.

### AOT compiler rewrite

The current AOT compiler is dead code from the original SageLang. Plan to rewrite as a C backend (emit C, compile with GCC) for portable native compilation.

### Standard library expansion

- HTTP client/server
- File system utilities
- Regular expressions (native, not via Python)
- Database drivers
- Serialization formats (TOML, YAML, MessagePack)


## Contributing

SageTree is maintained by one person. If you want to help:

1. Try the language. Report bugs.
2. Write tests for features that lack coverage.
3. Pick a "planned" item from this roadmap and implement it.
4. Improve error messages. Every error path should go through Firefly.

No contribution is too small. A better error message matters.
