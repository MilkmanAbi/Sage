# Test shift operators
# EXPECT: 16
# EXPECT: 4
# EXPECT: 256
# EXPECT: 1
# EXPECT: 0

println(1 << 4)
println(16 >> 2)
println(1 << 8)
println(8 >> 3)
println(0 << 10)
