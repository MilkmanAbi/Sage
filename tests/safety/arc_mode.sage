# RUN: run
# EXPECT: tracing
# EXPECT: arc
# EXPECT: 3
# EXPECT: hello
# EXPECT: 42
# EXPECT: done
# Test ARC (Automatic Reference Counting) mode

# Default mode is tracing
println(gc_mode())

# Switch to ARC mode
gc_set_arc()
println(gc_mode())

# Basic operations work in ARC mode
var arr = [1, 2, 3]
println(arr.length)

var s = "hello"
println(s)

var d = {}
d["x"] = 42
println(d["x"])

# Variable reassignment (tests arc_assign_value path)
var a = "first"
a = "second"
a = "third"

# Nested structures
var nested = [[1, 2], [3, 4]]
var flat = []
for sub in nested:
    for item in sub:
        flat.push(item)

println("done")
