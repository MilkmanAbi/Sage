# EXPECT: 42
# Test basic async proc and await
async proc compute():
    return 42

var future = compute()
var result = await future
println(result)
