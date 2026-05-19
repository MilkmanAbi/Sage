# EXPECT: 3
# EXPECT: 4
# EXPECT: 4
# EXPECT: 3
var a = [1, 2, 3]
print(a.length)
a.push(4)
print(a.length)
print(a[3])
a.pop()
print(a.length)
