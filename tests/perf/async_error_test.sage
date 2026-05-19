# EXPECT: multi_await_ok
# EXPECT: await_non_thread_ok
# EXPECT: PASS
# Test async: multiple sequential awaits and await on a plain value

async proc safe_compute(x):
    return x * 3

# Multiple sequential awaits on independent async tasks
var t1 = safe_compute(2)
var t2 = safe_compute(3)
var t3 = safe_compute(4)
var r1 = await t1
var r2 = await t2
var r3 = await t3
if r1 == 6 and r2 == 9 and r3 == 12:
    println("multi_await_ok")

# await on a plain (non-thread) value returns it directly
var plain = 42
var r = await plain
if r == 42:
    println("await_non_thread_ok")

println("PASS")
