# EXPECT: unwrap_raises_ok
# EXPECT: unwrap_or_else_ok
# EXPECT: or_else_ok
# EXPECT: copy_deep_ok
# EXPECT: PASS
# Tests for safety.sage fixes and edge cases
import safety

# --- unwrap() now raises instead of returning nil ---
var caught = false
try:
    safety.unwrap(safety.None())
catch e:
    if contains(e, "PANIC"):
        caught = true
if caught:
    println("unwrap_raises_ok")

# --- unwrap_or_else ---
proc default_77():
    return 77
var computed = safety.unwrap_or_else(safety.None(), default_77)
var direct = safety.unwrap_or_else(safety.Some(5), default_77)
if computed == 77 and direct == 5:
    println("unwrap_or_else_ok")

# --- or_else ---
proc fallback_99():
    return safety.Some(99)
var fallback = safety.or_else(safety.None(), fallback_99)
var kept = safety.or_else(safety.Some(1), fallback_99)
if safety.unwrap(fallback) == 99 and safety.unwrap(kept) == 1:
    println("or_else_ok")

# --- deep copy ---
var orig = {"a": [1, 2, 3], "b": {"c": 42}}
var cp = safety.copy(orig)
cp["a"][0] = 99
cp["b"]["c"] = 0
# Original must be unchanged
if orig["a"][0] == 1 and orig["b"]["c"] == 42:
    println("copy_deep_ok")

# --- Send/Sync on primitives and dicts ---
if safety.is_send(0) and safety.is_send("x") and safety.is_send(true):
    if safety.is_sync(0) == false:  # primitives are not Sync by default
        let d = {}
        d = safety.mark_send(d)
        d = safety.mark_sync(d)
        if safety.is_send(d) and safety.is_sync(d):
            println("send_sync_ok")

println("PASS")
