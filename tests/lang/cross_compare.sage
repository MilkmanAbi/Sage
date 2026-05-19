# EXPECT: true
# EXPECT: true
# EXPECT: true
# EXPECT: false
# EXPECT: true
# int == float cross-compare
println(42 == 42.0)
println(0 == 0.0)
# comparison operators
println(42 > 41.9)
println(42 < 42.0)
println(42 >= 42.0)
