# EXPECT: {}
# EXPECT: {}
# EXPECT: {}
# EXPECT: false
enum Color:
    Red
    Green
    Blue

println(Color.Red)
println(Color.Green)
println(Color.Blue)
println(Color.Red == 0)
