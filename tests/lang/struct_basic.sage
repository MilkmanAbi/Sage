# Test basic struct definition and field access
# EXPECT: 16
# EXPECT: 10
# EXPECT: 20
# EXPECT: 3.14

# Define: { int x; int y; double z; }
var Point = struct_def([["x", "int"], ["y", "int"], ["z", "double"]])
println(struct_size(Point))

var p = struct_new(Point)
struct_set(p, Point, "x", 10)
struct_set(p, Point, "y", 20)
struct_set(p, Point, "z", 3.14)

println(struct_get(p, Point, "x"))
println(struct_get(p, Point, "y"))
println(struct_get(p, Point, "z"))

mem_free(p)
