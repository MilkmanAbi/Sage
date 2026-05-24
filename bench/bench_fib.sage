# bench_fib.sage — Recursive Fibonacci (CPU bound)
proc fib(n):
    if n <= 1:
        return n
    return fib(n - 1) + fib(n - 2)

let start = clock()
let result = fib(30)
let elapsed = clock() - start
println(result)
println("time: " + str(elapsed) + "s")
