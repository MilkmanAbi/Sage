# EXPECT: 7
# EXPECT: hello world
proc add(a: int, b: int) -> int:
    return a + b

proc greet(name: str) -> str:
    return "hello " + name

println(add(3, 4))
println(greet("world"))
