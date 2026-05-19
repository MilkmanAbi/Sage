# EXPECT: 3.14159
# EXPECT: 12.0
# EXPECT: 1024
# EXPECT: [1, 2, 3]
import python

let math = python.import("math")
let pi_val = python.getattr(math, "pi")
println(precision(pi_val, 5))

let sqrt_result = python.call(math, "sqrt", 144.0)
println(sqrt_result)

let answer = python.eval("2 ** 10")
println(answer)

let json = python.import("json")
let parsed = python.call(json, "loads", "[1, 2, 3]")
println(parsed)
