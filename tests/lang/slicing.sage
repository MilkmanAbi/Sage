# EXPECT: 2
# EXPECT: 2
# EXPECT: 3
var a = [1, 2, 3, 4, 5]
var s = slice(a, 1, 3)
print(s.length)
print(s[0])
print(s[1])
