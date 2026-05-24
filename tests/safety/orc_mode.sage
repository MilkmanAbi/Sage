# RUN: run
# EXPECT: tracing
# EXPECT: orc
# EXPECT: 3
# EXPECT: hello
# EXPECT: 42
# EXPECT: reassigned
# EXPECT: done
# Test ORC (Optimized Reference Counting) mode - trial deletion cycle collector

# Default mode is tracing
println(gc_mode())

# Switch to ORC mode
gc_set_orc()
println(gc_mode())

# Basic operations work in ORC mode
var arr = [1, 2, 3]
println(arr.length)

var s = "hello"
println(s)

var d = {}
d["x"] = 42
println(d["x"])

# Variable reassignment (tests arc_assign_value path in ORC mode)
var a = "first"
a = "second"
a = "reassigned"
println(a)

# Nested structures (potential cycle candidates)
var nested = [[1, 2], [3, 4]]
var flat = []
for sub in nested:
    for item in sub:
        flat.push(item)

# Force cycle collection to exercise ORC trial deletion
gc_collect()

# Class instances (complex object graphs for ORC)
class Node:
    proc init(self, val):
        self.val = val
        self.next = nil

var n1 = Node(10)
var n2 = Node(20)
n1.next = n2

# Overwrite references to trigger ORC candidate marking
n1 = nil
n2 = nil

# Stress: allocate and discard objects to trigger ORC cycle collection
for i in 0..200:
    let tmp = [i, i + 1, i + 2]
    let tmp2 = {"key": tmp}

println("done")
