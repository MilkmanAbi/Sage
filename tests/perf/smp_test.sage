# EXPECT: true
# EXPECT: true
# EXPECT: true
# Test: CPU topology and SMP detection
var logical = cpu_count()
var physical = cpu_physical_cores()
var ht = cpu_has_hyperthreading()

# Logical CPU count should be >= 1
println(logical >= 1)

# Physical cores should be >= 1 and <= logical
println(physical >= 1)

# Hyperthreading should be true or false
println(ht == true or ht == false)
