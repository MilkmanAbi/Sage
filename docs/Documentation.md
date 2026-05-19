# SageTree Documentation

Version 0.2.0-alpha


## Table of Contents

1. Getting Started
2. Variables and Types
3. Functions
4. Control Flow
5. Data Structures
6. Structs and Classes
7. Enums and Pattern Matching
8. Memory Management
9. Error Handling
10. Modules and Imports
11. Python Interop
12. The REPL
13. Subsystems
14. Safety Analysis
15. Sandbox (LilyBox)
16. Firefly Error Codes


---


## 1. Getting Started

### Building

Requirements: GCC, Make. Optional: Python 3 dev headers (for Python FFI), libffi (for C FFI).

```sh
make
./sage your_script.sage
```

### Running

```sh
./sage script.sage              # Run with MossVM (default)
./sage --runtime ast script.sage  # Run with AST interpreter
./sage --firefly=minimal script.sage  # Quieter error output
./sage --info                    # Show build info
```

### The REPL

```sh
./sage
>>> 2 + 2
4
>>> "hello".upper()
HELLO
>>> help
```

REPL commands: `help`, `quit`, `vars`, `clear`, `history`, `type <expr>`, `time <expr>`.


---


## 2. Variables and Types

### Declaration styles

```sage
# Mutable, type inferred
var x = 42

# Immutable
let name = "Sage"

# Constant
const PI = 3.14159

# Type-annotated (enforced at runtime)
int count = 0
str label = "hello"
float ratio = 0.5
```

`let` and `const` variables cannot be reassigned. `var` can. Type annotations are checked at runtime. Assigning the wrong type is a Firefly error.

### Primitive types

| Type | Description | Example |
|------|-------------|---------|
| int | 64-bit signed integer | `42`, `-7`, `0xFF` |
| float | 64-bit double | `3.14`, `-0.5` |
| str | Immutable string | `"hello"` |
| bool | Boolean | `true`, `false` |
| nil | Null value | `nil` |

### Type checking

```sage
println(typeof(42))        # int
println(typeof(3.14))      # float
println(typeof("hi"))      # str
```

### Conversions

```sage
int("42")       # 42
float("3.14")   # 3.14
str(42)         # "42"
```

### Null safety

Typed variables reject nil by default. `int x = nil` is an error.

```sage
int x = nil     # Firefly error E013: cannot assign nil to variable 'x'
```

Optional types (planned): `int? x = nil` will be valid.


---


## 3. Functions

### Basic functions

```sage
proc greet(name):
    println("Hello, " + name)

greet("world")
```

### Typed parameters and return types

```sage
proc add(a: int, b: int) -> int:
    return a + b
```

Type annotations on parameters are enforced. Return types are enforced. Passing the wrong type produces a Firefly error with source location and suggestion.

### Closures

```sage
proc make_counter():
    var count = 0
    proc increment():
        count = count + 1
        return count
    return increment

let c = make_counter()
println(c())  # 1
println(c())  # 2
```

### Generators

```sage
proc fibonacci():
    var a = 0
    var b = 1
    while true:
        yield a
        var temp = a
        a = b
        b = temp + b

let gen = fibonacci()
println(next(gen))  # 0
println(next(gen))  # 1
println(next(gen))  # 1
```


---


## 4. Control Flow

### If/elif/else

```sage
if x > 10:
    println("big")
elif x > 0:
    println("small")
else:
    println("zero or negative")
```

### While loops

```sage
var i = 0
while i < 10:
    println(i)
    i = i + 1
```

### For loops

```sage
for item in [1, 2, 3]:
    println(item)

for i in 0..10:
    println(i)

for i in 0..=10:    # inclusive
    println(i)
```

### Break and continue

```sage
for i in 0..100:
    if i == 5:
        break
    if i % 2 == 0:
        continue
    println(i)
```


---


## 5. Data Structures

### Arrays

```sage
let arr = [1, 2, 3, 4, 5]
println(arr[0])       # 1
println(arr[-1])      # 5 (negative indexing)
println(arr.length)   # 5

arr.push(6)
arr.pop()
arr.reverse()
let joined = arr.join(", ")
```

Methods: `.push()`, `.pop()`, `.join()`, `.reverse()`, `.clear()`, `.slice()`, `.indexOf()`, `.contains()`, `.length`.

### Dictionaries

```sage
let person = {"name": "Abi", "age": 20}
println(person.name)
println(person["age"])
println(person.keys())
```

Methods: `.keys()`, `.values()`, `.get()`, `.delete()`, `.contains_key()`, `.len()`.

### Tuples

```sage
let point = (10, 20)
println(point[0])
```

### String methods

```sage
let s = "Hello, World"
s.upper()           # "HELLO, WORLD"
s.lower()           # "hello, world"
s.trim()
s.split(", ")       # ["Hello", "World"]
s.contains("World") # true
s.replace("World", "Sage")
s.starts_with("Hello")
s.ends_with("World")
s.length            # 12
"ha" * 3            # "hahaha"
```


---


## 6. Structs and Classes

### Structs

Structs have value semantics (copied on assignment).

```sage
struct Vec2:
    x: float
    y: float

impl Vec2:
    proc magnitude(self) -> float:
        return math.sqrt(self.x * self.x + self.y * self.y)

let v = Vec2(3.0, 4.0)
println(v.magnitude())  # 5.0

let v2 = v              # copy, not reference
v2.x = 99.0
println(v.x)            # still 3.0
```

### Classes

Classes have reference semantics.

```sage
class Animal:
    proc init(self, name):
        self.name = name
    proc speak(self):
        return self.name + " says ..."

class Dog(Animal):
    proc speak(self):
        return self.name + " says woof!"

let d = Dog("Rex")
println(d.speak())  # Rex says woof!
```

### Operator overloading

```sage
struct Vec2:
    x: float
    y: float

impl Vec2:
    proc __add__(self, other):
        return Vec2(self.x + other.x, self.y + other.y)
    proc __mul__(self, scalar):
        return Vec2(self.x * scalar, self.y * scalar)
```


---


## 7. Enums and Pattern Matching

### Simple enums

```sage
enum Color:
    Red
    Green
    Blue

let c = Color.Red
```

### ADT enums (enums with data)

```sage
enum Shape:
    Circle(radius: float)
    Rect(w: float, h: float)
    Point
```

### Pattern matching

```sage
match shape:
    case Shape.Circle(r):
        println("circle with radius " + str(r))
    case Shape.Rect(w, h):
        println("rect " + str(w) + "x" + str(h))
    case Shape.Point:
        println("point")
    default:
        println("unknown")
```

### Option and Result types

```sage
let x = Some(42)
let y = None

match x:
    case Some(v):
        println("got " + str(v))
    case None:
        println("nothing")

let ok = Ok("data")
let err = Err("failed")

match ok:
    case Ok(v):
        println(v)
    case Err(e):
        println("error: " + e)
```


---


## 8. Memory Management

SageTree offers three memory management modes.

### GC mode (default)

SageGC is a tri-color mark-sweep collector with concurrent marking and write barriers. It handles cycles. You do not need to think about memory.

```sage
let data = [1, 2, 3]
# SageGC cleans up when data goes out of scope
```

### Manual mode

`@manual:` blocks pause the GC. You control allocation and deallocation.

```sage
@manual:
    var buf = mem_alloc(1024)
    mem_write(buf, 0, "int", 42)
    mem_write(buf, 4, "int", 100)
    println(mem_read(buf, 0, "int"))   # 42
    println(mem_read(buf, 4, "int"))   # 100

    var p = ptr_add(buf, 4)
    println(mem_read(p, 0, "int"))     # 100

    mem_free(buf)
```

Firefly detects double-free (E080) and use-after-free (E081) in manual blocks.

### Hybrid mode

Mix GC and manual allocation in the same program.

```sage
gc_disable()
var pool = mem_alloc(4096)
# ... performance-critical work ...
mem_free(pool)
gc_enable()

# GC resumes, handles everything else
let normal_data = [1, 2, 3]
```

### Memory functions

| Function | Description |
|----------|-------------|
| `mem_alloc(size)` | Allocate size bytes |
| `mem_free(ptr)` | Free allocated memory |
| `mem_read(ptr, offset, type)` | Read value at offset. Types: "byte", "int", "double", "string" |
| `mem_write(ptr, offset, type, value)` | Write value at offset |
| `ptr_add(ptr, offset)` | Pointer arithmetic |
| `sizeof(type)` | Size of type in bytes |
| `gc_collect()` | Force a GC collection |
| `gc_disable()` / `gc_enable()` | Toggle GC |


---


## 9. Error Handling

### Try/catch/finally

```sage
try:
    let result = risky_operation()
    println(result)
catch e:
    println("Error: " + str(e))
finally:
    println("cleanup done")
```

### Raise

```sage
proc divide(a, b):
    if b == 0:
        raise "division by zero"
    return a / b
```


---


## 10. Modules and Imports

```sage
import math
println(math.sqrt(144))

import json
let data = json.parse('{"key": "value"}')

# Import specific names
from math import sqrt, floor
println(sqrt(144))

# Import with alias
import math as m
println(m.pi)
```

Standard library modules: math, io, sys, strings, arrays, dicts, iter, json, crypto, net, and more. See the `lib/` directory.


---


## 11. Python Interop

SageTree embeds CPython and can call any Python library directly.

```sage
import python

# Import a Python module
let math = python.import("math")
println(math.sqrt(144.0))      # 12.0
println(math.pi)                # 3.14159...

# JSON round-trip
let json = python.import("json")
let data = json.dumps({"name": "Sage"})
println(data)

# Use numpy, pandas, anything installed
let np = python.import("numpy")
let arr = np.array([1, 2, 3, 4, 5])
println(np.mean(arr))
```

Type marshaling is automatic: int, float, str, bool, list, and dict convert between Sage and Python transparently.


---


## 12. The REPL

Start the REPL:

```sh
./sage
```

```
>>> let x = 42
>>> x * 2
84
>>> "hello".upper()
HELLO
>>> typeof(3.14)
float
```

Commands: `help`, `quit`, `vars` (show all variables), `clear`, `history`, `type <expr>`, `time <expr>`.


---


## 13. Subsystems

### MossVM

The bytecode virtual machine. Compiles Sage AST to bytecode and executes it. Falls back to the AST interpreter for features not yet supported in bytecode (some struct/enum operations).

### SageGC

Tri-color mark-sweep garbage collector. Concurrent marking phase. Write barriers for correctness. Handles cycles. Thread-safe with mutex protection.

### Firefly

The error diagnosis subsystem. Every runtime error flows through Firefly. It provides:

- Source file, line, and column for every error
- Caret underlining of the exact token
- "Did you mean?" suggestions using edit distance
- Type-specific help (lists available methods per type)
- Call stack traces
- Three verbosity levels: full, minimal, off

See section 16 for the complete error code registry.

### LilyBox

Sandboxed execution environment. Wraps untrusted code in a capability container that restricts filesystem, network, process, FFI, and environment access.

### LilyKnight

The permission enforcer inside LilyBox. Checks every sensitive operation (fopen, system, dlopen, Python import) against the sandbox's capability set. Logs violations.

### FrogPond

The user-facing sandbox API. `import sandbox` to create and manage sandboxes from Sage code.


---


## 14. Safety Analysis

SageTree has an ownership and borrow analysis system. It is opt-in.

```sh
sage safety your_file.sage           # Analyze only
sage --strict-safety your_file.sage  # Analyze, then run if safe
```

The safety analyzer detects:
- Use after move
- Double move
- Aliased mutable references

This is a static analysis pass that runs before execution. It walks the AST and tracks ownership flow.


---


## 15. Sandbox (LilyBox)

LilyBox provides capability-based sandboxing for untrusted code.

Permission flags:
- `PERM_NET` -- network access
- `PERM_FS_READ` / `PERM_FS_WRITE` -- filesystem access
- `PERM_PROC` -- process execution (system())
- `PERM_FFI` -- foreign function interface (dlopen, Python import)
- `PERM_ENV_READ` -- environment variable access

When a sandbox is active, every sensitive stdlib call checks LilyKnight before executing. Violations are logged and blocked.


---


## 16. Firefly Error Codes

### Errors

| Code | Category | Description |
|------|----------|-------------|
| E001 | Names | Undefined variable |
| E002 | Names | Undefined function |
| E003 | Names | Undefined module |
| E004 | Names | Undefined type |
| E010 | Types | Type mismatch (general) |
| E011 | Types | Parameter type mismatch |
| E012 | Types | Return type mismatch |
| E013 | Types | Assignment type mismatch |
| E016 | Types | Nil assigned to non-optional type |
| E020 | Bounds | Array index out of bounds |
| E021 | Bounds | String index out of bounds |
| E022 | Bounds | Tuple index out of bounds |
| E025 | Bounds | Dict key not found |
| E030 | Arithmetic | Division by zero |
| E031 | Arithmetic | Modulo by zero |
| E040 | Mutability | Cannot reassign let variable |
| E041 | Mutability | Cannot reassign const |
| E050 | Members | No such method on type |
| E051 | Members | No such property on type |
| E052 | Members | Value is not callable |
| E060 | Calls | Wrong number of arguments |
| E070 | Modules | Module not found |
| E072 | Modules | Maximum recursion depth exceeded |
| E074 | Modules | Import failed |
| E080 | Memory | Double free detected |
| E081 | Memory | Use after free |
| E082 | Memory | Memory leak in @manual block |
| E085 | Memory | Use after move |
| E090 | Sandbox | Permission denied (filesystem) |
| E092 | Sandbox | Permission denied (network) |
| E093 | Sandbox | Permission denied (process exec) |
| E094 | Sandbox | Permission denied (FFI) |
| E100 | Operators | Unsupported operand types for + |
| E101 | Operators | Unsupported operand types for - |
| E103 | Operators | Unsupported operand types for / |
| E105 | Operators | Cannot compare types |

### Warnings

| Code | Description |
|------|-------------|
| W001 | Unused variable |
| W002 | Non-exhaustive match (no default case) |
| W003 | String concatenation in loop (planned) |
| W004 | Shadowed variable (planned) |
| W005 | Missing return in typed function (planned) |
| W006 | Unreachable code after return (planned) |
