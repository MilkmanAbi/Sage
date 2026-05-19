# EXPECT: 4950
# EXPECT: 55
# EXPECT: hello world
var sum = 0
var i = 0
while i < 100:
    sum = sum + i
    i = i + 1
println(sum)

proc fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)
println(fib(10))

println("hello " + "world")
