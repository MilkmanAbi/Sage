gc_disable()
# EXPECT: true
# EXPECT: false
# EXPECT: 3
# EXPECT: true

import std.trait

# Define a trait
var Printable = trait.define("Printable", ["to_string"])

# Test implements
var obj1 = {"to_string": "yes"}
var obj2 = {"name": "test"}
println(trait.implements(obj1, Printable))
println(trait.implements(obj2, Printable))

# Filter/map utilities
var nums = [1, 2, 3, 4, 5]

proc is_odd(x):
    return (x & 1) != 0

var odds = trait.trait_filter(nums, is_odd)
println(odds.length)

println(trait.any(nums, is_odd))
