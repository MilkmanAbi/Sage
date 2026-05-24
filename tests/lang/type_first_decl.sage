# EXPECT: 42
# EXPECT: Abi
# EXPECT: 3.14
# EXPECT: false
# EXPECT: int
# EXPECT: str
# EXPECT: float
# EXPECT: bool
# EXPECT: 3
# EXPECT: 42
int x = 42
str name = "Abi"
float pi = 3.14
bool done = false
println(x)
println(name)
println(pi)
println(done)
println(typeof(x))
println(typeof(name))
println(typeof(pi))
println(typeof(done))
# conversion functions still work
println(int(3.14))
# proc with type name still works
proc double(x):
    return x * 2
println(double(21))
