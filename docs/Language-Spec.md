# SageTree Language Specification

Version 0.2.0-alpha


## 1. Lexical Structure

### Keywords

```
var  let  const  proc  return  if  elif  else  while  for  in
break  continue  class  struct  enum  impl  match  case  default
import  from  as  try  catch  finally  raise  yield  async  await
true  false  nil  and  or  not  defer  pub
```

### Operators

```
+  -  *  /  %  **
==  !=  <  >  <=  >=
=  +=  -=  *=  /=  %=
&  |  ^  ~  <<  >>
..  ..=
??
```

### Literals

```sage
# Integers
42
0xFF        # hex
0b1010      # binary
0o77        # octal

# Floats
3.14
1.0e10

# Strings
"hello"
"interpolation: {expression}"
"escape: \n \t \\ \""

# Boolean
true
false

# Nil
nil

# Array
[1, 2, 3]

# Dict
{"key": "value", "num": 42}

# Tuple
(1, 2, 3)
```

### Comments

```sage
# Single-line comment
```

Block comments are not currently supported.


## 2. Type System

SageTree uses gradual typing. Types are optional annotations enforced at runtime.

### Primitive types

| Type | Size | Range |
|------|------|-------|
| int | 64-bit signed | -2^63 to 2^63-1 |
| float | 64-bit double | IEEE 754 |
| str | Heap-allocated | UTF-8 |
| bool | 1 byte | true / false |
| nil | Singleton | nil |

### Composite types

| Type | Description |
|------|-------------|
| Array | Dynamic array, heterogeneous |
| Dict | Hash map, string keys |
| Tuple | Fixed-size, immutable |

### Type annotations

```sage
int x = 42           # typed declaration (mutable)
let y: int = 42      # alternative syntax
var z = 42            # type inferred
```

Type annotations are checked at runtime on assignment, parameter passing, and return. Nil is rejected for non-optional typed variables.

### Type promotion

When mixing int and float in arithmetic, the result is float:
- `int + int` produces `int`
- `int + float` produces `float`
- `float + float` produces `float`

Integer division truncates: `10 / 3` produces `3` (int).
Mixed division promotes: `10 / 3.0` produces `3.333...` (float).


## 3. Declarations

### Variables

```sage
var x = 42        # mutable, inferred type
let y = 42        # immutable
const Z = 42      # constant (by convention, uppercase)
int n = 0         # typed mutable
str s = "hello"   # typed mutable
```

### Functions

```sage
proc name(params):
    body

proc typed(a: int, b: int) -> int:
    return a + b
```

Functions are first-class values. They can be passed as arguments, returned from functions, and stored in variables.

### Structs

```sage
struct Name:
    field1: type
    field2: type
```

Structs have value semantics. Assignment copies the struct.

### Classes

```sage
class Name:
    proc init(self, params):
        self.field = value
    proc method(self):
        body

class Child(Parent):
    proc method(self):
        # overrides Parent.method
```

Classes have reference semantics. Assignment shares the reference.

### Enums

```sage
# Simple enum
enum Color:
    Red
    Green
    Blue

# ADT enum (with associated data)
enum Result:
    Ok(value: any)
    Err(message: str)
```

### Impl blocks

```sage
impl TypeName:
    proc method(self):
        body
```

Adds methods to structs or classes after their definition.


## 4. Expressions

### Arithmetic

`+`, `-`, `*`, `/`, `%`, `**` (power).

### Comparison

`==`, `!=`, `<`, `>`, `<=`, `>=`.

### Logical

`and`, `or`, `not`. Short-circuit evaluation.

### Bitwise

`&`, `|`, `^`, `~`, `<<`, `>>`.

### String

- Concatenation: `"a" + "b"`
- Repetition: `"ha" * 3`
- Interpolation: `"value is {expr}"`

### Indexing

```sage
arr[0]          # array/tuple index
arr[-1]         # negative index (from end)
dict["key"]     # dict access
dict.key        # dot access on dicts
str[0]          # string character access
```

### Ranges

```sage
0..10           # exclusive (0 to 9)
0..=10          # inclusive (0 to 10)
```

### Null coalescing

```sage
x ?? default_value
```


## 5. Statements

### Assignment

```sage
x = value
x += 1
x -= 1
x *= 2
x /= 2
x %= 3
```

### If

```sage
if condition:
    body
elif condition:
    body
else:
    body
```

### While

```sage
while condition:
    body
```

### For

```sage
for item in iterable:
    body

for i in 0..n:
    body
```

### Match

```sage
match value:
    case pattern:
        body
    case pattern:
        body
    default:
        body
```

Patterns: literals, variable binding, ADT destructuring (`Shape.Circle(r)`), `Some(v)`, `None`, `Ok(v)`, `Err(e)`.

### Try/catch/finally

```sage
try:
    body
catch e:
    handler
finally:
    cleanup
```

### Return, break, continue, yield, raise

Standard control flow. `yield` creates generators. `raise` throws exceptions.

### Defer

```sage
defer:
    cleanup_code
```

Executes when the enclosing scope exits.

### Memory blocks

```sage
@manual:
    # GC paused, manual allocation
    body

@trusted:
    # no GC overhead
    body
```


## 6. Standard Library

### Built-in functions

| Function | Description |
|----------|-------------|
| `println(value)` | Print with newline |
| `print(value)` | Print without newline |
| `typeof(value)` | Type name as string |
| `len(value)` | Length of array/string/dict |
| `int(value)` | Convert to int |
| `float(value)` | Convert to float |
| `str(value)` | Convert to string |
| `precision(float, digits)` | Round float to N decimal places |
| `range(start, end)` | Create array from range |
| `input(prompt)` | Read line from stdin |
| `clock()` | Current time as float (seconds since epoch) |
| `Some(value)` | Wrap in Option |
| `Ok(value)` | Wrap in Result (success) |
| `Err(value)` | Wrap in Result (error) |

### Modules

Located in `lib/`. Import with `import module_name`.

Core: math, strings, arrays, dicts, iter, json, assert, utils, perf.
Crypto: crypto/hash, crypto/cipher, crypto/encoding, crypto/hmac, crypto/password, crypto/rand.
Net: net/url, net/request, net/server, net/headers, net/dns, net/ip.
Std: std/fmt, std/log, std/testing, std/datetime, std/regex, std/argparse, std/process, std/signal, std/channel, std/atomic, std/threadpool, std/condvar, std/rwlock, std/compress, std/unicode, std/db, std/enum, std/trait, std/profiler, std/debug, std/build, std/docgen, std/package, std/interop.


## 7. Execution Model

SageTree has two interpreter backends:

- **MossVM** (default): Compiles AST to bytecode, then executes in a stack-based virtual machine. Faster for loops and simple programs.
- **AST interpreter**: Directly walks the abstract syntax tree. Supports all language features including struct/enum/impl.

The runtime automatically falls back from MossVM to AST for features not yet compiled to bytecode.

### Compilation (experimental)

```sh
sage --aot script.sage         # Compile to native binary
sage --aot script.sage -o out  # Specify output path
```

The AOT compiler is experimental and incomplete.
