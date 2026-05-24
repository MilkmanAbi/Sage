# EXPECT: [2, 4]
# EXPECT: 1
# EXPECT: [1, 2, 3]
# EXPECT: [1, 2, 3, 4, 5, 6]
# EXPECT: [[1, 2], [3, 4], [5]]
# EXPECT: [1, 2]
# EXPECT: [3, 4, 5]
# Test arrays module: filter, find, unique, flatten, chunk, take, drop
from arrays import filter, find, unique, flatten, chunk, take, drop

proc is_even(x):
    return x % 2 == 0

println(filter([1, 2, 3, 4], is_even))

proc less_than_3(x):
    return x < 3
println(find([3, 1, 4], less_than_3))

println(unique([1, 2, 3, 2, 1, 3]))
println(flatten([[1, 2], [3, 4], [5, 6]]))
println(chunk([1, 2, 3, 4, 5], 2))
println(take([1, 2, 3, 4, 5], 2))
println(drop([1, 2, 3, 4, 5], 2))
