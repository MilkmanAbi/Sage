# EXPECT: [[1, a], [2, b], [3, c]]
# EXPECT: [[0, x], [1, y], [2, z]]
import arrays
println(arrays.zip([1, 2, 3], ["a", "b", "c"]))
println(arrays.enumerate(["x", "y", "z"]))
