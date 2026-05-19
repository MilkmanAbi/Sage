# EXPECT: 25.0
# EXPECT: Point(3.0, 4.0)
# EXPECT: Point(4.0, 5.0)
struct Point:
    x: float
    y: float

impl Point:
    proc magnitude(self):
        return self.x * self.x + self.y * self.y

    proc translate(self, dx, dy):
        self.x = self.x + dx
        self.y = self.y + dy

    proc to_str(self):
        return "Point(" + str(self.x) + ", " + str(self.y) + ")"

var p = Point(3.0, 4.0)
println(p.magnitude())
println(p.to_str())
p.translate(1.0, 1.0)
println(p.to_str())
