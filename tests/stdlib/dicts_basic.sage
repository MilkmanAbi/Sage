# EXPECT: {"y": 3, "z": 4, "x": 1}
# EXPECT: true
# EXPECT: false
import dicts
let a = {"x": 1, "y": 2}
let b = {"y": 3, "z": 4}
println(dicts.merge(a, b))
println(dicts.has(a, "x"))
println(dicts.has(a, "missing"))
