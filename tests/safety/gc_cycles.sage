# EXPECT: true
# EXPECT: true
var baseline_objects = gc_stats()["num_objects"]
var before_collections = gc_collections()

proc churn():
    var i = 0
    while i < 2048:
        let cycle = []
        cycle.push(cycle)
        i = i + 1

churn()
var trigger = 0
while trigger < 8:
    let probe = "x" + "y"
    trigger = trigger + 1
var after_collections = gc_collections()
println(after_collections > before_collections)

gc_collect()
var after_objects = gc_stats()["num_objects"]
println(after_objects <= baseline_objects + 8)
