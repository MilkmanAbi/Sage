# EXPECT: 50
# EXPECT: int
# EXPECT: 43.5
# EXPECT: float
# EXPECT: 3
# EXPECT: 3.33333
# EXPECT: 1
# EXPECT: 1024
# EXPECT: -1
# int + int = int
println(42 + 8)
println(typeof(42 + 8))
# int + float = float
println(42 + 1.5)
println(typeof(42 + 1.5))
# int / int = int (truncating)
println(10 / 3)
# float / int = float
println(10.0 / 3)
# int % int = int
println(10 % 3)
# bitwise always int
println(1 << 10)
println(~0)
