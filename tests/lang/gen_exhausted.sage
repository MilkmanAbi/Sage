# EXPECT: 1
# EXPECT: nil
proc once():
    yield 1
var g = once()
print(next(g))
print(next(g))
