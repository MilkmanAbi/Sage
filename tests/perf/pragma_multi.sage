# EXPECT: deprecated called
# EXPECT: 7
# Test multiple pragmas on a single declaration

@inline
@deprecated
proc old_add(a, b):
    println("deprecated called")
    return a + b

var result = old_add(3, 4)
println(result)
