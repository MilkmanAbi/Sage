# Test addressof function
# EXPECT: true
# EXPECT: true

# addressof returns a number (memory address)
var arr = [1, 2, 3]
var addr = addressof(arr)
println(addr > 0)

# Two different arrays have different addresses
var arr2 = [4, 5, 6]
var addr2 = addressof(arr2)
println(addr != addr2)
