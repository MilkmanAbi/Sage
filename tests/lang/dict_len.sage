# EXPECT: nil
# EXPECT: nil
# EXPECT: nil
var empty = {}
print(empty.length)
var d = {"a": 1, "b": 2, "c": 3}
print(d.length)
dict_delete(d, "b")
print(d.length)
