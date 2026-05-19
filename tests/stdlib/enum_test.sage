gc_disable()
# EXPECT: true
# EXPECT: Red
# EXPECT: 42
# EXPECT: true
# EXPECT: default
# EXPECT: true
# EXPECT: true

import std.enum

# Basic enum
var Color = enum.enum_def("Color", ["Red", "Green", "Blue"])
var red = enum.variant(Color, "Red")
println(enum.is_variant(red, "Red"))
println(enum.variant_name(red))

# Result type
var success = enum.ok(42)
println(enum.unwrap(success))
println(enum.is_ok(success))

var failure = enum.err("oops")
println(enum.unwrap_or(failure, "default"))
println(enum.is_err(failure))

# Option type
var val = enum.some(10)
println(enum.is_some(val))
