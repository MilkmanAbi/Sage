# EXPECT: 120
# EXPECT: 3628800
# Test comptime() expression form

proc factorial(n):
    if n <= 1:
        return 1
    return n * factorial(n - 1)

# comptime() as an expression evaluates at compile time
var five_fact = comptime(factorial(5))
println(five_fact)

var ten_fact = comptime(factorial(10))
println(ten_fact)
