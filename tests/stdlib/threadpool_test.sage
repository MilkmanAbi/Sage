gc_disable()
# EXPECT: 4
# EXPECT: 2
# EXPECT: 0
# EXPECT: true

import std.threadpool

proc square(x):
    return x * x

var pool = threadpool.create(4)
var id1 = threadpool.submit(pool, square, [3])
var id2 = threadpool.submit(pool, square, [5])
threadpool.run_all(pool)

println(pool["num_workers"])
println(pool["completed"])
println(pool["failed"])
println(threadpool.get_result(pool, id2) == 25)
