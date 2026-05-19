gc_disable()
# EXPECT: 16
# EXPECT: 36
# EXPECT: true
# EXPECT: true
# EXPECT: 10

import crypto.rand

var rng = rand.create(12345)

# Random bytes
var bytes = rand.random_bytes(rng, 16)
println(bytes.length)

# UUID v4
var id = rand.uuid4(rng)
println(id.length)

# Bounded random
var val = rand.next_bounded(rng, 100)
println(val >= 0)
println(val < 100)

# Random string
var s = rand.random_string(rng, 10)
println(s.length)
