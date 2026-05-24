# EXPECT: 30
# Test thread spawn with arguments
import thread

proc add(a, b):
    return a + b

var t = thread.spawn(add, 10, 20)
var result = thread.join(t)
println(result)
