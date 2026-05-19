gc_disable()
# EXPECT: 0
# EXPECT: 1
# EXPECT: true
# EXPECT: 3
# EXPECT: true

import std.condvar

var cv = condvar.create()
println(condvar.waiter_count(cv))

condvar.wait(cv)
println(condvar.waiter_count(cv))

condvar.notify(cv)
println(condvar.is_notified(cv))

# Semaphore
var sem = condvar.create_semaphore(3)
println(condvar.available_permits(sem))
condvar.acquire(sem)
condvar.acquire(sem)
println(condvar.available_permits(sem) == 1)
