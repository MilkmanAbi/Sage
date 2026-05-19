# RUN: bytecode-run
# EXPECT: 10
# EXPECT: 11
# EXPECT: 7
# EXPECT: 2
# EXPECT: 8
# EXPECT: bytecode

var total = 0
var i = 0

while i < 5:
    total = total + i
    i = i + 1

println(total)

var bits = (0b1010 & 0b1110) | 0b0001
println(bits)

var arr = [3, 4]
println(arr[0] + arr[1])

var stats = {"hp": 2}
println(stats["hp"])

print (8 >> 1) + (1 << 2)
println("byte" + "code")
