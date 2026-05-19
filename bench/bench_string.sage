# bench_string.sage — String concatenation + methods
let start = clock()
var result = ""
var i = 0
while i < 10000:
    result = result + str(i)
    i = i + 1
let len = result.length
let elapsed = clock() - start
println(len)
println("time: " + str(elapsed) + "s")
