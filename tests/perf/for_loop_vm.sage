# RUN: bytecode-run
# EXPECT: 6
# EXPECT: outer

var item = "outer"
var total = 0

for item in [1, 2, 3]:
    total = total + item

println(total)
println(item)
