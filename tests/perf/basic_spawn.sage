# EXPECT: hello from thread
# EXPECT: 42
# Test basic thread spawn and join
import thread

proc worker():
    println("hello from thread")
    return 42

var t = thread.spawn(worker)
var result = thread.join(t)
println(result)
