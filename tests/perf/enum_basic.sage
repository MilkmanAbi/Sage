# EXPECT: {}
# EXPECT: {}
# EXPECT: {}
# EXPECT: Direction
# Test enum declaration
enum Color:
    Red
    Green
    Blue

println(Color["Red"])
println(Color["Green"])
println(Color["Blue"])

enum Direction:
    North
    South
    East
    West

println(Direction["__name__"])
