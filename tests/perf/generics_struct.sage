# EXPECT: 10
# EXPECT: hello
# Test generic type parameters on structs (parsed, dynamic semantics)

struct Wrapper[T]:
    value: T

var w1 = Wrapper(10)
println(w1.value)

var w2 = Wrapper("hello")
println(w2.value)
