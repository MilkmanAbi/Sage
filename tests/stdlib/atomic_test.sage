gc_disable()
# EXPECT: 0
# EXPECT: 5
# EXPECT: true
# EXPECT: false
# EXPECT: 10

import std.atomic

var a = atomic.atomic_int(0)
println(atomic.load(a))

atomic.add(a, 5)
println(atomic.load(a))

# CAS
println(atomic.cas(a, 5, 10))
println(atomic.cas(a, 5, 20))
println(atomic.load(a))
