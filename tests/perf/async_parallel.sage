# EXPECT: 300
# Test multiple async procs running in parallel
async proc square(x):
    return x * x

var a = square(10)
var b = square(10)
var c = square(10)
println(await a + await b + await c)
