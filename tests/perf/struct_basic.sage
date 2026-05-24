# EXPECT: 3
# EXPECT: 4
# EXPECT: hello
# Test struct as value type with auto-init
struct Point:
    x: Int
    y: Int

var p = Point(3, 4)
println(p.x)
println(p.y)

struct Config:
    name: String
    value: Int

var c = Config("hello", 42)
println(c.name)
