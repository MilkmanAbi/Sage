# EXPECT: 10
# EXPECT: 0
# EXPECT: 50.0
import utils
println(utils.clamp(15, 0, 10))
println(utils.clamp(-5, 0, 10))
println(utils.lerp(0.0, 100.0, 0.5))
