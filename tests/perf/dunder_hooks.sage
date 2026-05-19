# EXPECT: <instance of Point>
# EXPECT: true
# EXPECT: false
# Test __str__ and __eq__ hooks
class Point:
    proc init(self, x, y):
        self.x = x
        self.y = y
    proc __str__(self):
        return "Point(" + str(self.x) + ", " + str(self.y) + ")"
    proc __eq__(self, other):
        return self.x == other.x and self.y == other.y

var p1 = Point(3, 4)
var p2 = Point(3, 4)
var p3 = Point(5, 6)
println(p1)
println(p1 == p2)
println(p1 == p3)
