# Test that struct_new returns zeroed memory
# EXPECT: 0
# EXPECT: 0
# EXPECT: 0.0

var S = struct_def([["x", "int"], ["y", "int"], ["z", "double"]])
var s = struct_new(S)

println(struct_get(s, S, "x"))
println(struct_get(s, S, "y"))
println(struct_get(s, S, "z"))

mem_free(s)
