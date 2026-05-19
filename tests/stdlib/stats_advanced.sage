# EXPECT: 18.0
# EXPECT: 15.5
import stats
let d = [4, 8, 15, 16, 23, 42]
println(stats.mean(d))
println(stats.median(d))
