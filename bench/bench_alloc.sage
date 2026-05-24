# bench_alloc.sage — Array allocation stress (GC pressure)
let start = clock()
var total = 0
var i = 0
while i < 100000:
    var arr = [i, i + 1, i + 2, i + 3, i + 4]
    total = total + arr[2]
    i = i + 1
let elapsed = clock() - start
println(total)
println("time: " + str(elapsed) + "s")
