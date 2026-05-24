# EXPECT: [1, 2, 3, 5, 8, 9]
# EXPECT: [5, 8, 9]
# EXPECT: [10, 4, 16, 2, 18, 6]
# EXPECT: 28
import arrays
let a = [5, 2, 8, 1, 9, 3]
println(arrays.sort(a))
proc gt4(x):
    return x > 4
println(arrays.filter(a, gt4))
proc dbl(x):
    return x * 2
println(arrays.map(a, dbl))
println(arrays.sum(a))
