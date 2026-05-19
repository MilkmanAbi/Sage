gc_disable()
# EXPECT: timer_init
# EXPECT: ticks_zero
# EXPECT: frequency_set
# EXPECT: PASS
var PIT_FREQ = 1193182
var ticks = 0
var freq = 100
var divisor = (PIT_FREQ / freq) | 0
println("timer_init")
if ticks == 0:
    println("ticks_zero")
if divisor > 0:
    println("frequency_set")
println("PASS")
