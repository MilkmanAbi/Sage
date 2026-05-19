# EXPECT: 3.14159
# EXPECT: [0, 1, 4, 9, 16]
# Test comptime block producing values used later

comptime:
    let PI = 3.14159

println(PI)

comptime:
    let squares = []
    for i in 0..5:
        squares.push(i * i)

println(squares)
