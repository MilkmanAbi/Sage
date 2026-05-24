# EXPECT: Vec2(4, 6)
# EXPECT: Vec2(2, 2)
class Vec2:
    proc init(self, x, y):
        self.x = x
        self.y = y
    proc __add__(self, other):
        return Vec2(self.x + other.x, self.y + other.y)
    proc __sub__(self, other):
        return Vec2(self.x - other.x, self.y - other.y)
    proc __str__(self):
        return "Vec2({self.x}, {self.y})"
var v1 = Vec2(1, 2)
var v2 = Vec2(3, 4)
print(v1 + v2)
print(v2 - v1)
