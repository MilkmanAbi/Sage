# EXPECT: {"y": 2}
# EXPECT: {"x": 1}
import dicts
let d = {"x": 1, "y": 2}
println(dicts.omit(d, ["x"]))
println(dicts.pick(d, ["x"]))
