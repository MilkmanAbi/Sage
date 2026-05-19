# EXPECT: 3
# EXPECT: int
# EXPECT: 42.0
# EXPECT: float
# EXPECT: 42
# EXPECT: 3.14
# EXPECT: 42
# int() truncates
println(int(3.7))
println(typeof(int(3.7)))
# float() promotes
println(float(42))
println(typeof(float(42)))
# int() parses string
println(int("42"))
# float() parses string
println(float("3.14"))
# str() from int
println(str(42))
