# EXPECT: 255
# EXPECT: 26
# EXPECT: 493
# EXPECT: 10
# EXPECT: 1000000
# Hex literals
println(0xFF)
println(0x1A)
# Octal literals
println(0o755)
# Binary literals (already supported, verify still works)
println(0b1010)
# Large hex
println(0xF4240)
