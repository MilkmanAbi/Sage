# EXPECT: 55
# EXPECT: hello from comptime
# EXPECT: 256
# Test comptime blocks — execute code at compile time

# Basic comptime block with a computed value
comptime:
    var sum = 0
    for i in 0..11:
        sum = sum + i
    println(sum)

# Comptime block with string
comptime:
    let msg = "hello from comptime"
    println(msg)

# Comptime block with power computation
comptime:
    let base = 2
    var result = 1
    for i in 0..8:
        result = result * base
    println(result)
