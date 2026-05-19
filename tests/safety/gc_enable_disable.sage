# EXPECT: ok
gc_disable()
var a = [1, 2, 3]
gc_enable()
gc_collect()
print("ok")
