# Test combined bitwise operations
# EXPECT: 15
# EXPECT: true
# EXPECT: 240

# Mask and shift: extract bits 0-3 from 0xFF
var val = 255
var mask = 15
println(val & mask)

# Check if bit 2 is set in 7
var x = 7
var bit2 = (x >> 2) & 1
println(bit2 == 1)

# Set upper nibble: shift 15 left by 4
println(15 << 4)
