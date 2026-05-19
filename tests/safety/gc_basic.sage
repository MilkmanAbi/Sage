# EXPECT: ok
# Test that GC runs without crashing
gc_collect()
var a = [1, 2, 3]
var d = {"x": 1}
gc_collect()
print("ok")
