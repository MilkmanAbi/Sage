# EXPECT: 10
# EXPECT: 15
var s = 0
for i in 0..5:
    s += i
print(s)
var s2 = 0
for i in 1..=5:
    s2 += i
print(s2)
