# EXPECT: 20
# EXPECT: 15
# EXPECT: 42
# var is mutable
var x = 10
x = 20
println(x)

# type-first is mutable
int y = 5
y = 15
println(y)

# let is immutable (no output from reassignment - it errors to stderr)
let z = 42
println(z)
