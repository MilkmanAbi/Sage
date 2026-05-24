# EXPECT: done
# Test mutex creation, lock, and unlock
import thread

var m = thread.mutex()
thread.lock(m)
thread.unlock(m)
println("done")
