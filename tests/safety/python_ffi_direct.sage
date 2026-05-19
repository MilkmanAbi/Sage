# EXPECT: 12.0
# EXPECT: 3
# EXPECT: [1, 2, 3]
import python
let math = python.import("math")
println(math.sqrt(144.0))
println(math.floor(3.7))
let json = python.import("json")
println(json.loads("[1, 2, 3]"))
