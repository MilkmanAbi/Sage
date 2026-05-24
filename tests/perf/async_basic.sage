# EXPECT: 49
# EXPECT: 30
async proc square(n):
    return n * n
async proc add(a, b):
    return a + b
print(await square(7))
print(await add(10, 20))
