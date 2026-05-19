# EXPECT: 10
# EXPECT: hello
# EXPECT: [1, 2, 3]
# Test generic type parameters on procs (type params parsed, semantics are dynamic)

proc identity[T](x):
    return x

println(identity(10))
println(identity("hello"))
println(identity([1, 2, 3]))
