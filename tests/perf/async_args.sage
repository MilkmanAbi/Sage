# EXPECT: 30
# Test async proc with arguments
async proc add(a, b):
    return a + b

var future = add(10, 20)
println(await future)
