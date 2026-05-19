gc_disable()
# EXPECT: int
# EXPECT: string
# EXPECT: 2

import std.debug

println(debug.type_name(42))
println(debug.type_name("hello"))

# Watch (without triggering change output)
var w = debug.create_watcher()
debug.watch(w, "x", 10)
var h = debug.watch_history(w, "x")
debug.watch(w, "y", 20)
println(len(debug.watch_history(w, "x")) + len(debug.watch_history(w, "y")))
