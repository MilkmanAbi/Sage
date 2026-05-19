gc_disable()
# EXPECT: true
# EXPECT: true
# EXPECT: 10

import std.profiler

var p = profiler.create()

proc work():
    let s = 0
    for i in 0..100:
        s = s + i
    return s

profiler.begin(p, "work")
work()
profiler.end_section(p, "work")

var keys = dict_keys(p["entries"])
println(keys.length == 1)

var entry = p["entries"]["work"]
println(entry["call_count"] == 1)

# Benchmark
proc add_fn():
    return 1 + 1

var result = profiler.bench("add", add_fn, 10)
println(result["iterations"])
