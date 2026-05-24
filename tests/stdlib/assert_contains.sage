# EXPECT: passed
import assert
assert.contains([1, 2, 3], 2)
assert.contains("hello world", "world")
println("passed")
