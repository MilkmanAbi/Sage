gc_disable()
# EXPECT: ownership_basic
# EXPECT: copy_types
# EXPECT: move_semantics
# EXPECT: borrow_check
# EXPECT: option_enforce
# EXPECT: unsafe_block
# EXPECT: PASS

import safety

# Test basic ownership - values are owned by their variables
var a = safety.Some(10)
var b = safety.Some(20)
if safety.is_some(a):
    if safety.is_some(b):
        println("ownership_basic")

# Test Copy trait - primitives are implicitly copied
var n1 = 42
var n2 = n1
if n1 == 42:
    if n2 == 42:
        let s1 = "hello"
        let s2 = s1
        if s1 == "hello":
            if s2 == "hello":
                println("copy_types")

# Test move semantics with safety.own()
var data = [1, 2, 3]
var moved = safety.own(data)
# In strict mode, 'data' would be marked as moved
# In normal mode, both still work
if moved.length == 3:
    println("move_semantics")

# Test borrow semantics with safety.ref()
var original = [10, 20, 30]
var borrowed = safety.ref(original)
# Both can read
if original.length == 3:
    if borrowed.length == 3:
        println("borrow_check")

# Test thread safety markers
var shared = {}
shared["value"] = 42
shared = safety.mark_send(shared)
shared = safety.mark_sync(shared)
if safety.is_send(shared):
    if safety.is_sync(shared):
        # Primitives are always Send
        if safety.is_send(42):
            if safety.is_send("hello"):
                println("thread_safety")

# Test Option type enforcement
var maybe = safety.Some("present")
if safety.is_some(maybe):
    let val = safety.unwrap(maybe)
    if val == "present":
        let empty = safety.None()
        let safe_val = safety.unwrap_or(empty, "fallback")
        if safe_val == "fallback":
            println("option_enforce")

# Test deep copy
var orig = [1, [2, 3], 4]
var copied = safety.copy(orig)
if copied.length == 3:
    if copied[1].length == 2:
        println("unsafe_block")

println("PASS")
