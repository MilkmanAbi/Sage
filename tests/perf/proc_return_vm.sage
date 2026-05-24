# RUN: bytecode-run
# EXPECT: 12
# EXPECT: 12
# EXPECT: nil

proc add(a, b):
    return a + b

proc twice(x):
    return add(x, x)

proc noop():
    let local = 99

println(add(5, 7))
println(twice(6))
println(noop())
