# EXPECT: 10
# EXPECT: 99
# EXPECT: 20
struct Point:
    x: int
    y: int

var p = Point(10, 20)
var q = p
q.x = 99
println(p.x)
println(q.x)
println(p.y)
