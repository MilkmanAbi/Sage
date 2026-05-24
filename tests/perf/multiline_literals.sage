# EXPECT: [1, 2, 3]
# EXPECT: nil
# EXPECT: hello
# Test multiline array literal
var arr = [
    1,
    2,
    3,
]
println(arr)

# Test multiline dict literal
var d = {
    "a": 1,
    "b": 2,
    "c": 3,
}
println(d.length)

# Test multiline function call
proc greet(name, greeting):
    println(greeting)

greet(
    "world",
    "hello",
)
