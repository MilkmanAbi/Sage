# EXPECT: 3.33333
# EXPECT: 3.33
# EXPECT: 3.3
# EXPECT: 3
# EXPECT: 3.14159
println(precision(10.0 / 3, 5))
println(precision(10.0 / 3, 2))
println(precision(10.0 / 3, 1))
println(precision(10.0 / 3, 0))
println(precision(3.14159265, 5))
