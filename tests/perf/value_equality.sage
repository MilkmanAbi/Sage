# EXPECT: true
# EXPECT: false
# EXPECT: true
# EXPECT: true
# EXPECT: false
# EXPECT: true
# Array equality
var a = [1, 2, 3]
var b = [1, 2, 3]
var c = [1, 2, 4]
println(a == b)
println(a == c)

# Instance equality
class Point:
    proc init(self, x, y):
        self.x = x
        self.y = y

var p1 = Point(1, 2)
var p2 = Point(1, 2)
var p3 = Point(3, 4)
println(p1 == p2)
println(p1 == p1)
println(p1 == p3)

# append works (unified with push)
var arr = []
append(arr, 42)
println(arr.length == 1)
