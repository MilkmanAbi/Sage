# EXPECT: 5
# EXPECT: hel
# String length and slicing
var s = "hello"
print(s.length)
var parts = s.split("")
var sub = slice(parts, 0, 3)
print(join(sub, ""))
