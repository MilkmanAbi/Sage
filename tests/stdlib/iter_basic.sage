# EXPECT: [0, 1, 2, 3, 4]
# EXPECT: [0, 2, 4, 6, 8]
import iter
println(iter.range_list(0, 5))
println(iter.range_step(0, 10, 2))
