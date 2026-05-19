# EXPECT: 0.0
# EXPECT: 5.0
# EXPECT: true
# EXPECT: 10.0
# Test: True C-level atomic operations
var a = atomic_new(0)
println(atomic_load(a))

# Atomic add
atomic_add(a, 5)
println(atomic_load(a))

# Atomic CAS
var ok = atomic_cas(a, 5, 10)
println(ok)

println(atomic_load(a))
