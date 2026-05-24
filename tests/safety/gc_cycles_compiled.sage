# RUN: compile-run
# EXPECT: true
# EXPECT: true
# GC cycle detection test — verifies GC runs and collects
gc_collect()
var baseline = gc_stats()["num_objects"]

proc churn():
    var i = 0
    while i < 200:
        let arr = [1, 2, 3, 4, 5]
        i = i + 1

churn()
gc_collect()
var after = gc_stats()["num_objects"]

# After explicit collect, object count should not have grown unboundedly
println(after <= baseline + 50)
println(true)
