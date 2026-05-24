# EXPECT: 3
# EXPECT: a
# EXPECT: b
# EXPECT: c
# EXPECT: a-b-c
var parts = "a,b,c".split(",")
print(parts.length)
print(parts[0])
print(parts[1])
print(parts[2])
print(join(parts, "-"))
