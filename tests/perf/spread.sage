# EXPECT: [1, 2, 3, 4, 5]
# EXPECT: [1, 2, 3, 4]
var mid = [2, 3, 4]
print([1, ...mid, 5])
var a = [1, 2]
var b = [3, 4]
print([...a, ...b])
