# EXPECT: true
# Test thread.id returns a number
import thread

var id = thread.id()
println(id > 0)
