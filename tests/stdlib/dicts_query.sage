# EXPECT: 99
# EXPECT: [[y, 2], [x, 1]]
import dicts
let d = {"x": 1, "y": 2}
println(dicts.get_or(d, "missing", 99))
println(dicts.to_pairs(d))
