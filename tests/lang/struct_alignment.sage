# Test struct alignment matches C layout
# EXPECT: 24
# EXPECT: 65
# EXPECT: 99.99
# EXPECT: 42

# { char a; double b; int c; }
# C layout: a@0(1), pad(7), b@8(8), c@16(4), pad(4) = 24
var S = struct_def([["a", "char"], ["b", "double"], ["c", "int"]])
println(struct_size(S))

var s = struct_new(S)
struct_set(s, S, "a", 65)
struct_set(s, S, "b", 99.99)
struct_set(s, S, "c", 42)

println(struct_get(s, S, "a"))
println(struct_get(s, S, "b"))
println(struct_get(s, S, "c"))

mem_free(s)
