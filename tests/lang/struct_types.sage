# Test all supported field types
# EXPECT: 255
# EXPECT: 32000
# EXPECT: 100000
# EXPECT: 10000000
# EXPECT: 2.5
# EXPECT: 3.14159

var S = struct_def([["a", "byte"], ["b", "short"], ["c", "int"], ["d", "long"], ["e", "float"], ["f", "double"]])

var s = struct_new(S)
struct_set(s, S, "a", 255)
struct_set(s, S, "b", 32000)
struct_set(s, S, "c", 100000)
struct_set(s, S, "d", 10000000)
struct_set(s, S, "e", 2.5)
struct_set(s, S, "f", 3.14159)

println(struct_get(s, S, "a"))
println(struct_get(s, S, "b"))
println(struct_get(s, S, "c"))
println(struct_get(s, S, "d"))
println(struct_get(s, S, "e"))
println(struct_get(s, S, "f"))

mem_free(s)
