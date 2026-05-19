# EXPECT: circle: 5.0
# EXPECT: rect: 12.0
# EXPECT: point
enum Shape:
    Circle(radius: float)
    Rect(w: float, h: float)
    Point

proc area(s):
    match s:
        case Shape.Circle(r):
            return "circle: " + str(r)
        case Shape.Rect(w, h):
            return "rect: " + str(w * h)
        case Shape.Point:
            return "point"
        default:
            return "?"

println(area(Shape.Circle(5.0)))
println(area(Shape.Rect(3.0, 4.0)))
println(area(Shape.Point))
