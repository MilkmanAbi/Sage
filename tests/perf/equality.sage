# Conformance: Equality (Spec §11)
# Value equality for all types.
# EXPECT: true
# EXPECT: true
# EXPECT: true
# EXPECT: true
# EXPECT: true
# EXPECT: false
# EXPECT: true
# Nil
println(nil == nil)
# Bool
println(true == true)
# Number
println(42 == 42)
# String
println("hello" == "hello")
# Array (structural)
println([1, 2, 3] == [1, 2, 3])
println([1, 2] == [1, 3])
# Instance (structural)
class Point:
    proc init(self, x, y):
        self.x = x
        self.y = y
println(Point(1, 2) == Point(1, 2))
