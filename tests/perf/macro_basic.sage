# EXPECT: starting timer
# EXPECT: timer result: 45
# EXPECT: ending timer
# Test macro definitions (treated as procs in interpreter mode)

macro timed(label):
    println("starting " + label)
    var result = 0
    for i in 0..10:
        result = result + i
    println(label + " result: " + str(result))
    println("ending " + label)

timed("timer")
