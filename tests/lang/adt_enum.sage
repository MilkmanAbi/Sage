# EXPECT: 5.0
# EXPECT: 3.0
# EXPECT: 4.0
enum Shape:
    Circle(radius: float)
    Rect(width: float, height: float)
    Point

var c = Shape.Circle(5.0)
println(c.radius)

var r = Shape.Rect(3.0, 4.0)
println(r.width)
println(r.height)
